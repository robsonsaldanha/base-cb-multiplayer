// cb-server [--config server.cfg]: carrega a config, sobe rede (UDP) e HTTP (TCP) na mesma porta e roda o tick.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cbmp/messages.hpp>

#include "cbadmin/admin_server.hpp"
#include "cbscript/api.hpp"
#include "cbscript/http_out.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/db.hpp"
#include "cbscript/host.hpp"
#include "cbserver/auth.hpp"
#include "cbserver/config.hpp"
#include "cbserver/http.hpp"
#include "cbserver/hub.hpp"
#include "cbserver/manifest.hpp"
#include "cbserver/metrics.hpp"
#include "cbserver/net.hpp"
#include "cbserver/pool.hpp"
#include "cbserver/script_bridge.hpp"
#include "cbserver/snapshots.hpp"
#include "cbserver/world.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr const char* kDevServerId = "srv_dev";

std::atomic<bool> gStop{false};
std::atomic<bool> gStopped{false};  // parada limpa concluída (rede/HTTP fechados, logs gravados)

#ifdef _WIN32
BOOL WINAPI onConsoleCtrl(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      gStop = true;
      return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT: {
      // O Windows encerra o processo assim que o handler retorna: espera a parada limpa (até 4 s).
      gStop = true;
      for (int i = 0; i < 400 && !gStopped; ++i) Sleep(10);
      return TRUE;
    }
    default:
      return FALSE;
  }
}
#else
extern "C" void onSignal(int) { gStop = true; }
#endif

void installStopHandlers() {
#ifdef _WIN32
  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#else
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
#endif
}

int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void setupLogs(const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>((dir / "server.log").string(), 10 * 1024 * 1024, 5);
  auto logger = std::make_shared<spdlog::logger>("server", spdlog::sinks_init_list{console, file});
  spdlog::set_default_logger(logger);
  spdlog::flush_every(std::chrono::seconds(2));
}

std::shared_ptr<spdlog::logger> makeAnticheatLogger(const std::filesystem::path& dir) {
  auto sink = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>((dir / "anticheat-%Y-%m-%d.jsonl").string(), 0, 0);
  auto logger = std::make_shared<spdlog::logger>("anticheat", sink);
  logger->set_pattern("%v");
  logger->flush_on(spdlog::level::info);
  return logger;
}

std::shared_ptr<spdlog::logger> makeAuditLogger(const std::filesystem::path& dir) {
  auto sink = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>((dir / "audit-%Y-%m-%d.jsonl").string(), 0, 0);
  auto logger = std::make_shared<spdlog::logger>("audit", sink);
  logger->set_pattern("%v");
  logger->flush_on(spdlog::level::info);
  return logger;
}

double steadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void anticheat(spdlog::logger& log, const cbserver::World& world, cbserver::EntityId id, nlohmann::json line) {
  const auto* p = world.find(id);
  line["ts"] = unixNow();
  line["id"] = id;
  if (p) {
    line["did"] = p->did;
    line["sub"] = p->sub;
  }
  log.info("{}", line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

int run(const std::string& cfgPath) {
  auto cfg = cbserver::ServerConfig::load(cfgPath);
  setupLogs(cfg.logsDir);
  auto acLog = makeAnticheatLogger(cfg.logsDir);
  spdlog::info("cb-server {}: '{}' porta {}, até {} jogadores", kVersion, cfg.name, cfg.port, cfg.maxPlayers);

  auto store = cbserver::ResourceStore::scan(cfg.resourcesDir);

  std::unique_ptr<cbserver::KeyProvider> keys;
  if (cfg.devPublicKey) {
    keys = std::make_unique<cbserver::StaticKeys>(cbserver::StaticKeys::fromDevKey(*cfg.devPublicKey));
    spdlog::warn("modo dev: aceitando tickets assinados pela chave dev (kid 'dev'), sem Hub");
  } else if (cfg.hub) {  // hub e devPublicKey são mutuamente exclusivos (validado na config)
    keys = std::make_unique<cbserver::HubKeys>(cfg.hub->url, [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    });
  } else {
    spdlog::error("configure 'hub' ou 'auth.devPublicKey' para validar tickets");
    return 1;
  }
  const std::string serverId = cfg.hub ? cfg.hub->serverId : kDevServerId;

  cbserver::Metrics metrics;
  cbserver::Authenticator auth(*keys, serverId, unixNow);
  cbserver::World world(cfg);
  cbserver::TaskPool pool(cbserver::TaskPool::defaultThreads());
  cbserver::SnapshotBuilder builder(pool);
  cbserver::NetServer net(cfg, auth, world, metrics);
  net.setViolationSink([&acLog](cbserver::EntityId id, const std::string& sub, const std::string& did,
                                const std::string& reason) {
    nlohmann::json line{{"type", "net_close"}, {"ts", unixNow()}, {"id", id},
                        {"sub", sub},          {"did", did},       {"reason", reason}};
    acLog->info("{}", line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  });

  // Scripts Lua: thread própria; a simulação só conversa com ela por filas (ScriptBridge).
  // Banco (opcional): pool em threads próprias; ninguém espera por ele (sobe mesmo com o banco fora do ar).
  std::shared_ptr<cbscript::DbPool> db;
  if (cfg.db) {
    cbscript::DbConfig dc;
    dc.host = cfg.db->host;
    dc.port = cfg.db->port;
    dc.user = cfg.db->user;
    dc.password = cfg.db->password;
    dc.database = cfg.db->database;
    dc.pool = cfg.db->pool;
    dc.timeoutMs = cfg.db->timeoutMs;
    dc.tls = cfg.db->tls == "off"        ? cbscript::DbConfig::Tls::Off
             : cfg.db->tls == "required" ? cbscript::DbConfig::Tls::Required
             : cfg.db->tls == "verify"   ? cbscript::DbConfig::Tls::Verify
                                         : cbscript::DbConfig::Tls::Auto;
    spdlog::info("banco: {} (pool {}, timeout {} ms, tls {})", dc.describe(), dc.pool, dc.timeoutMs, cfg.db->tls);
    if (dc.pool == 1)
      spdlog::warn("banco: pool 1 — uma transação aberta (cb.db.transaction) segura a única conexão e atrasa as "
                   "demais consultas e as gravações de ban; use pool >= 2");
    spdlog::info("banco: plugins de autenticação em {}", cbscript::mysqlPluginDir());
    db = std::make_shared<cbscript::DbPool>(dc, cbscript::mysqlConnectionFactory(dc));
  }
  auto auditLog = makeAuditLogger(cfg.logsDir);
  std::shared_ptr<cbscript::BanStore> bans;
  if (db) {
    cbscript::BanStoreDbOptions bo;
    bo.audit = [auditLog](const std::string& line) { auditLog->info("{}", line); };
    bans = std::make_shared<cbscript::BanStore>(db, unixNow, bo);
  } else {
    bans = std::make_shared<cbscript::BanStore>(cfg.bansFile, unixNow);
  }
  spdlog::info("bans: backend {}", bans->backend());
  cbscript::ApiOptions api;
  api.bans = bans;
  api.db = db;
  // HTTP de saída dos scripts (cb.http): 4 threads próprias; https com verificação de certificado pelas CAs do
  // sistema; só hosts de permissions.http de cada resource; nunca endereços privados/loopback.
  api.http = std::make_shared<cbscript::HttpOutPool>();
  api.audit = [auditLog](const std::string& line) { auditLog->info("{}", line); };
  api.unixNow = unixNow;
  auto apiStats = std::make_shared<cbscript::ApiStats>();
  api.stats = apiStats;
  cbscript::HostOptions hostOpt;
  cbscript::installApi(hostOpt, std::move(api));
  cbscript::Limits limits;
  limits.memoryBytes = static_cast<size_t>(cfg.scriptMemoryLimitMB) << 20;
  limits.warnAfter = std::chrono::milliseconds(cfg.scriptWarnMs);
  limits.abortAfter = std::chrono::milliseconds(cfg.scriptAbortMs);
  cbscript::ScriptHost scripts(cfg.resourcesDir, limits, steadySeconds, std::move(hostOpt));
  scripts.start();
  for (const auto& st : scripts.status())
    if (st.state != cbscript::ResourceStatus::State::Running)
      spdlog::warn("resource '{}' não subiu: {}", st.name, st.lastError);
  cbserver::ScriptBridge bridge(world, scripts);
  net.setAdmission([bans](const std::string&, const std::string& did) -> std::optional<std::string> {
    if (auto b = bans->check(did, unixNow())) {
      std::string why = "banido: " + b->reason;
      size_t n = std::min<size_t>(why.size(), 250);  // Reject aceita até 255 bytes; corta sem quebrar UTF-8
      while (n < why.size() && n > 0 && (static_cast<unsigned char>(why[n]) & 0xC0) == 0x80) --n;
      why.resize(n);
      return why;
    }
    return std::nullopt;
  });

  std::string err;
  if (!net.start(static_cast<uint16_t>(cfg.port), err)) {
    spdlog::error("rede: {}", err);
    return 1;
  }
  cbserver::HttpServer http(cfg, store, metrics, [&cfg, &metrics, serverId] {
    return nlohmann::json{{"serverId", serverId},          {"name", cfg.name},
                          {"players", metrics.players.load()}, {"maxPlayers", cfg.maxPlayers},
                          {"protocol", cbmp::kProtocolVersion}, {"version", kVersion}};
  });
  try {
    http.start("0.0.0.0", cfg.port);
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    net.stop();
    return 1;
  }
  spdlog::info("ouvindo em UDP/TCP {} (serverId {}, {} threads de snapshot)", cfg.port, serverId, pool.workers());

  // Painel admin: HTTP próprio (porta admin.port, bind admin.bind), threads próprias; só lê fotos/status publicados e
  // age pelas filas (canal prioritário da ponte, ScriptHost). Sobe só se houver quem possa entrar.
  std::unique_ptr<cbadmin::AdminServer> admin;
  if (cfg.admin.enabled()) {
    cbadmin::AdminOptions ao;
    ao.bind = cfg.admin.bind;
    ao.port = cfg.admin.port;
    ao.publicUrl = cfg.admin.publicUrl;
    ao.admins = cfg.admin.admins;
    ao.devToken = cfg.admin.devToken;
    ao.trustedProxies = cfg.admin.trustedProxies;
    ao.serverId = serverId;
    ao.hubUrl = cfg.hub ? cfg.hub->url : std::string();
    ao.logsDir = cfg.logsDir;
    ao.dbPassword = cfg.db ? cfg.db->password : std::string();
    auto adminMetrics = [&metrics, &scripts] {
      auto j = metrics.snapshot();
      const auto hm = scripts.metrics();
      j["scripts"] = {{"eventQueueDepth", hm.eventQueueDepth}, {"eventsDropped", hm.eventsDropped},
                      {"commandsDropped", hm.commandsDropped}, {"halted", hm.halted}};
      return j;
    };
    admin = std::make_unique<cbadmin::AdminServer>(
        std::move(ao), cbadmin::makeBackend(scripts, bridge, bans, *keys, std::move(adminMetrics),
                                            [auditLog](const std::string& l) { auditLog->info("{}", l); }, unixNow));
    try {
      admin->start();
    } catch (const std::exception& e) {
      spdlog::error("{}", e.what());
      http.stop();
      net.stop();
      return 1;
    }
    const bool v6 = cfg.admin.bind.find(':') != std::string::npos;
    spdlog::info("painel admin em http://{}{}{}:{} ({} admin(s){})", v6 ? "[" : "", cfg.admin.bind, v6 ? "]" : "",
                 admin->boundPort(), cfg.admin.admins.size(), cfg.admin.devToken ? ", devToken" : "");
    if (cfg.admin.publicUrl) spdlog::info("painel admin: publicUrl {}", *cfg.admin.publicUrl);
    if (cfg.hub && !cfg.admin.publicUrl)
      spdlog::warn("painel admin: sem admin.publicUrl o login via Hub fica desabilitado");
  } else {
    spdlog::info("painel admin desligado (configure admin.admins com ids Discord para ligar)");
  }

  std::unique_ptr<cbserver::Heartbeat> heartbeat;
  if (cfg.hub) {
    cbserver::HeartbeatInfo hi;
    hi.name = cfg.name;
    hi.description = cfg.description;
    hi.maxPlayers = cfg.maxPlayers;
    hi.gamePort = cfg.port;
    hi.httpPort = cfg.port;
    hi.version = kVersion;
    hi.protocol = cbmp::kProtocolVersion;
    hi.tags = cfg.tags;
    hi.locale = cfg.locale;
    hi.adminUrl = cfg.admin.publicUrl.value_or("");
    heartbeat = std::make_unique<cbserver::Heartbeat>(
        cfg.hub->url, cfg.hub->key, std::move(hi),
        [&metrics] { return static_cast<uint32_t>(std::max<int64_t>(0, metrics.players.load())); });
  }

  installStopHandlers();
  using Clock = std::chrono::steady_clock;
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / cfg.tickHz));
  const auto t0 = Clock::now();
  auto next = t0;
  std::vector<cbserver::Inbound> in;
  std::vector<cbserver::Outgoing> out;
  std::vector<cbserver::KickOrder> kicks;
  while (!gStop) {
    const auto start = Clock::now();
    const double now = std::chrono::duration<double>(start - t0).count();
    in.clear();
    out.clear();
    kicks.clear();

    net.pump(now, in);
    for (auto& m : in) {
      if (const auto* x = std::get_if<cbmp::Input>(&m.msg)) {
        world.onInput(m.from, *x, now);
      } else if (const auto* a = std::get_if<cbmp::SnapshotAck>(&m.msg)) {
        world.onSnapshotAck(m.from, a->tick, now);
      } else if (const auto* e = std::get_if<cbmp::Event>(&m.msg)) {
        if (!world.onEvent(m.from, *e, now)) ++metrics.droppedEvents;
      }
    }
    try {
      bridge.applyCommands(now);
    } catch (const std::exception& e) {  // comando de script nunca derruba a simulação
      spdlog::error("scripts: falha ao aplicar comandos: {}", e.what());
    }
    world.tick(now, builder, out, kicks);
    bridge.publishPlayers();
    for (const auto& o : out)
      if (const auto* c = std::get_if<cbmp::Correction>(&o.message()); c && c->seq != 0) {
        ++metrics.corrections;
        anticheat(*acLog, world, o.to,
                  {{"type", "correction"}, {"seq", c->seq}, {"pos", {c->pos.x, c->pos.y, c->pos.z}}});
      }
    net.send(out);
    for (const auto& k : kicks) {
      anticheat(*acLog, world, k.who, {{"type", "kick"}, {"reason", k.reason}});
      net.kick(k.who, k.reason);  // também remove do mundo
    }
    metrics.recordTick(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start));
    if (world.currentTick() % static_cast<uint32_t>(cfg.tickHz) == 0) {  // contadores dos scripts, 1×/s
      const auto hm = scripts.metrics();
      metrics.scriptEventsDropped = static_cast<int64_t>(hm.eventsDropped + bridge.eventsDropped());
      metrics.scriptCommandsDropped = static_cast<int64_t>(hm.commandsDropped + hm.priorityDropped + bridge.commandsDropped());
      metrics.scriptCommandsRejected = static_cast<int64_t>(bridge.commandsRejected());
      metrics.scriptEventsOutDropped = static_cast<int64_t>(world.eventsOutDropped());
      metrics.scriptLogDropped = static_cast<int64_t>(apiStats->logDropped.load());
      metrics.scriptAuditDropped = static_cast<int64_t>(apiStats->auditDropped.load());
      metrics.scriptJoinLeaveDropped = static_cast<int64_t>(bridge.joinLeaveDropped());
      if (db) {
        const auto dm = db->metrics();
        metrics.setDb({{"queries", dm.queries}, {"errors", dm.errors}, {"timeouts", dm.timeouts}, {"qps", dm.qps},
                       {"p50Ms", dm.p50Ms}, {"p99Ms", dm.p99Ms}, {"busy", dm.busy}, {"connected", dm.connected},
                       {"size", dm.size}, {"queued", dm.queued}, {"activeTx", dm.activeTx},
                       {"bansLoaded", bans->loaded()}, {"bansPendingWrites", bans->pendingWrites()}});
      }
    }

    next += period;
    const auto after = Clock::now();
    if (after > next + 5 * period) next = after;  // atraso grande: não tenta compensar ticks perdidos
    std::this_thread::sleep_until(next);
  }

  spdlog::info("encerrando...");
  if (heartbeat) heartbeat->stop();
  if (admin) admin->stop();
  http.stop();
  net.stop();
  spdlog::info("servidor parado");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);  // logs em UTF-8 (pt-BR)
#endif
  std::string cfgPath = "server.cfg";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      cfgPath = argv[++i];
    } else if (a == "--help" || a == "-h") {
      std::printf("uso: cb-server [--config server.cfg]\n");
      return 0;
    } else {
      std::fprintf(stderr, "argumento desconhecido: %s\nuso: cb-server [--config server.cfg]\n", a.c_str());
      return 2;
    }
  }
  try {
    const int rc = run(cfgPath);
    spdlog::shutdown();
    gStopped = true;
    return rc;
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    std::fprintf(stderr, "%s\n", e.what());
    spdlog::shutdown();
    gStopped = true;
    return 1;
  }
}
