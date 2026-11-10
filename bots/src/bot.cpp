#include "bot.hpp"
#include "echo.hpp"
#include "gaps.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sodium.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <cbmp/base64url.hpp>
#include <cbmp/messages.hpp>
#include <cbmp/quantize.hpp>

namespace cbbots {
namespace {

constexpr double kInputPeriod = 0.05;    // 20 Hz
constexpr double kEchoPeriod = 2.0;
constexpr double kClusterRadiusCm = 3000;  // 30 m
constexpr double kSpeedCmS = 500;        // correndo (limite do servidor 900 cm/s + 20%)
constexpr double kMaxMoveDt = 0.5;       // nunca anda mais que isso num input, mesmo com o processo atrasado
constexpr double kScenarioDelay = 2.0;   // bot 0 joga normalmente por 2 s antes de trapacear
constexpr double kTeleportJumpCm = 10000;  // 100 m por teleporte
constexpr int kFloodPerInput = 25;       // 25 a cada 50 ms = 500 eventos/s
constexpr int kEndKicked = k_ESteamNetConnectionEnd_App_Min + 4;   // mesmo código do cb-server
constexpr int kEndRejected = k_ESteamNetConnectionEnd_App_Min + 1;

enum class Phase { Waiting, Connecting, Handshake, Playing, Done };

struct Bot {
  int idx = 0;
  Phase phase = Phase::Waiting;
  HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
  double connectAt = 0, welcomeAt = -1, endAt = -1;
  bool scenario = false;  // bot 0 do cenário de teleporte/flood
  // movimento
  bool clustered = false;
  double x = 0, y = 0, z = 0, heading = 0, lastMoveAt = 0;
  uint32_t seq = 0;
  double nextInput = 0, nextEcho = 0, phaseOffset = 0;
  // cenário
  double scenarioStart = -1, nextTeleport = -1;
  // snapshots
  uint32_t snapTick = 0;
  uint8_t partsMask = 0, partsCount = 0;
  double lastSnapAt = -1, maxGap = 0;
  // resultado
  bool welcomed = false, kicked = false, rejected = false, disconnected = false;
  std::string reason;
  double kickedAt = -1;
  uint64_t bytesIn = 0, snapshots = 0, corrections = 0, enters = 0;
  double rttSum = 0;
  uint64_t rttN = 0;
  uint64_t echoSent = 0;
};

using Clock = std::chrono::steady_clock;

struct Swarm;
Swarm* gSwarm = nullptr;  // o callback de status do GNS é uma função livre

struct Swarm {
  const BotOptions& opt;
  ISteamNetworkingSockets* gns = nullptr;
  HSteamNetPollGroup poll = k_HSteamNetPollGroup_Invalid;
  std::vector<Bot> bots;
  std::unordered_map<HSteamNetConnection, Bot*> byConn;
  GapStats gaps;
  std::mt19937_64 rng;
  Clock::time_point t0 = Clock::now();
  std::map<std::string, int> rejectReasons, kickReasons, closeReasons;

  explicit Swarm(const BotOptions& o) : opt(o), rng(o.rngSeed) {}

  double now() const { return std::chrono::duration<double>(Clock::now() - t0).count(); }
  double uniform(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }

  bool countsForStats(const Bot& b) const { return !b.scenario; }

  void send(Bot& b, const cbmp::Message& m) {
    const auto bytes = cbmp::encode(m);
    const int flags = cbmp::isReliable(cbmp::typeOf(m)) ? k_nSteamNetworkingSend_Reliable
                                                         : k_nSteamNetworkingSend_UnreliableNoDelay;
    gns->SendMessageToConnection(b.conn, bytes.data(), static_cast<uint32_t>(bytes.size()), flags, nullptr);
  }

  void placeInitial(Bot& b) {
    if (b.clustered) {
      const double r = kClusterRadiusCm * std::sqrt(uniform(0, 1)), a = uniform(0, 2 * std::numbers::pi);
      b.x = r * std::cos(a);
      b.y = r * std::sin(a);
    } else {
      const double half = opt.areaM * 50.0;
      b.x = uniform(-half, half);
      b.y = uniform(-half, half);
    }
    b.heading = uniform(0, 2 * std::numbers::pi);
  }

  bool outside(const Bot& b) const {
    if (b.clustered) return std::hypot(b.x, b.y) > kClusterRadiusCm;
    const double half = opt.areaM * 50.0;
    return std::abs(b.x) > half || std::abs(b.y) > half;
  }

  void connect(Bot& b, double t) {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    const std::string hp = opt.host + ":" + std::to_string(opt.port);
    if (!addr.ParseString(hp.c_str())) throw std::runtime_error("endereço inválido: " + hp);
    SteamNetworkingConfigValue_t cfg;
    FnSteamNetConnectionStatusChanged fn = &Swarm::statusThunk;
    void* fnPtr = nullptr;
    static_assert(sizeof(fn) == sizeof(fnPtr));
    std::memcpy(&fnPtr, &fn, sizeof(fnPtr));
    cfg.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, fnPtr);
    b.conn = gns->ConnectByIPAddress(addr, 1, &cfg);
    b.connectAt = t;
    if (b.conn == k_HSteamNetConnection_Invalid) {
      b.phase = Phase::Done;
      b.reason = "ConnectByIPAddress falhou";
      return;
    }
    gns->SetConnectionPollGroup(b.conn, poll);
    byConn[b.conn] = &b;
    b.phase = Phase::Connecting;
  }

  void sendHello(Bot& b) {
    const int64_t iat =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::array<uint8_t, 16> j{};
    randombytes_buf(j.data(), j.size());
    std::string jti;
    static const char* kHex = "0123456789abcdef";
    for (uint8_t c : j) {
      jti += kHex[c >> 4];
      jti += kHex[c & 15];
    }
    cbmp::Hello h;
    h.ticket = makeTicket(opt.seed, b.idx, opt.serverId, iat, jti);
    send(b, h);
    b.phase = Phase::Handshake;
  }

  static void statusThunk(SteamNetConnectionStatusChangedCallback_t* info) {
    if (gSwarm) gSwarm->onStatus(info);
  }

  void onStatus(SteamNetConnectionStatusChangedCallback_t* info) {
    auto it = byConn.find(info->m_hConn);
    if (it == byConn.end()) return;
    Bot& b = *it->second;
    const double t = now();
    switch (info->m_info.m_eState) {
      case k_ESteamNetworkingConnectionState_Connected:
        if (b.phase == Phase::Connecting) sendHello(b);
        break;
      case k_ESteamNetworkingConnectionState_ClosedByPeer:
      case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        // Kick/Reject chegam antes do fechamento: drena o que ainda está na fila desta conexão.
        SteamNetworkingMessage_t* msgs[64];
        int n;
        while ((n = gns->ReceiveMessagesOnConnection(b.conn, msgs, 64)) > 0)
          for (int i = 0; i < n; ++i) {
            onMessage(b, static_cast<const uint8_t*>(msgs[i]->m_pData), static_cast<size_t>(msgs[i]->m_cbSize), t);
            msgs[i]->Release();
          }
        const int end = info->m_info.m_eEndReason;
        const std::string why = info->m_info.m_szEndDebug;
        if (b.welcomed) {
          if (!b.kicked && end == kEndKicked) {
            b.kicked = true;
            b.kickedAt = t;
            if (b.reason.empty()) b.reason = why;
          }
          if (!b.kicked) {
            b.disconnected = true;
            b.reason = why;
          }
        } else {
          if (end == kEndRejected && !b.rejected) {
            b.rejected = true;
            b.reason = why;
          }
          if (b.reason.empty()) b.reason = why;
        }
        close(b, t);
        break;
      }
      default:
        break;
    }
  }

  void close(Bot& b, double t) {
    if (b.phase == Phase::Done) return;
    if (b.welcomed && b.endAt < 0) b.endAt = t;
    gns->CloseConnection(b.conn, 0, "fim", false);
    byConn.erase(b.conn);
    b.phase = Phase::Done;
  }

  void recordGap(Bot& b, double gapSec) {
    b.maxGap = std::max(b.maxGap, gapSec);
    if (countsForStats(b)) gaps.add(gapSec);
  }

  void onMessage(Bot& b, const uint8_t* data, size_t size, double t) {
    b.bytesIn += size;
    cbmp::Message m;
    try {
      m = cbmp::decode(std::span<const uint8_t>(data, size));
    } catch (const cbmp::DecodeError&) {
      b.reason = "mensagem inválida do servidor";
      return;
    }
    if (const auto* w = std::get_if<cbmp::Welcome>(&m)) {
      (void)w;
      if (b.welcomed) return;
      b.welcomed = true;
      b.welcomeAt = t;
      b.phase = Phase::Playing;
      b.lastMoveAt = t;
      sendInput(b, t);  // primeiro Input = spawn (sim.spawn devTrustFirstInput)
      b.nextInput = t + kInputPeriod + b.phaseOffset;
      b.nextEcho = t + uniform(0.5, kEchoPeriod);
      if (b.scenario) {
        b.scenarioStart = t + kScenarioDelay;
        b.nextTeleport = b.scenarioStart;
      }
    } else if (const auto* r = std::get_if<cbmp::Reject>(&m)) {
      b.rejected = true;
      b.reason = r->reason;
    } else if (const auto* k = std::get_if<cbmp::Kick>(&m)) {
      if (!b.kicked) {
        b.kicked = true;
        b.kickedAt = t;
        b.reason = k->reason;
      }
    } else if (const auto* s = std::get_if<cbmp::Snapshot>(&m)) {
      ++b.snapshots;
      if (s->tick < b.snapTick) return;  // parte atrasada de um tick antigo: não reinicia a montagem
      if (s->tick != b.snapTick) {
        recordGap(b, t - (b.lastSnapAt >= 0 ? b.lastSnapAt : b.welcomeAt));  // 1º: desde o Welcome
        b.lastSnapAt = t;
        b.snapTick = s->tick;
        b.partsMask = 0;
        b.partsCount = s->partCount;
      }
      b.partsMask = static_cast<uint8_t>(b.partsMask | (1u << s->partIndex));
      if (std::popcount(static_cast<unsigned>(b.partsMask)) == b.partsCount && b.phase == Phase::Playing)
        send(b, cbmp::SnapshotAck{s->tick});
    } else if (const auto* c = std::get_if<cbmp::Correction>(&m)) {
      ++b.corrections;
      const bool cheating = b.scenario && opt.teleportEvery > 0 && b.scenarioStart >= 0 && t >= b.scenarioStart;
      if (!cheating) {  // bot honesto adota a posição autoritativa
        b.x = c->pos.x;
        b.y = c->pos.y;
        b.z = c->pos.z;
      }
    } else if (const auto* e = std::get_if<cbmp::Event>(&m)) {
      if (e->name == "echo") {
        if (auto sent = decodeEcho(e->payload)) {
          b.rttSum += t - *sent;
          ++b.rttN;
        }
      }
    } else if (std::holds_alternative<cbmp::EntityEnter>(m)) {
      ++b.enters;
    }
  }

  void sendInput(Bot& b, double t) {
    const double dt = std::clamp(t - b.lastMoveAt, 0.0, kMaxMoveDt);
    b.lastMoveAt = t;
    if (b.seq > 0) {  // o primeiro input só declara o spawn
      b.heading += std::normal_distribution<double>(0, 0.15)(rng);
      if (outside(b)) b.heading = std::atan2(-b.y, -b.x) + uniform(-0.5, 0.5);  // volta para dentro
      b.x += std::cos(b.heading) * kSpeedCmS * dt;
      b.y += std::sin(b.heading) * kSpeedCmS * dt;
    }
    cbmp::Input in;
    in.seq = ++b.seq;
    in.s.pos = {static_cast<int32_t>(std::lround(b.x)), static_cast<int32_t>(std::lround(b.y)),
                static_cast<int32_t>(std::lround(b.z))};
    in.s.yaw = cbmp::quantizeYaw(static_cast<float>(b.heading * 180.0 / std::numbers::pi));
    in.s.pitch = cbmp::quantizePitch(0);
    in.s.vel = {static_cast<int16_t>(std::lround(std::cos(b.heading) * kSpeedCmS)),
                static_cast<int16_t>(std::lround(std::sin(b.heading) * kSpeedCmS)), 0};
    in.s.state = b.seq > 1 ? cbmp::MoveState::Run : cbmp::MoveState::Idle;
    send(b, in);
  }

  void step(Bot& b, double t) {
    if (b.phase != Phase::Playing || b.kicked) return;
    if (b.scenario && b.scenarioStart >= 0 && t >= b.scenarioStart) {
      if (opt.teleportEvery > 0 && t >= b.nextTeleport) {  // trapaceiro persistente: salta e mantém a posição
        b.x += kTeleportJumpCm;
        b.nextTeleport = t + opt.teleportEvery;
      }
      if (opt.flood && t >= b.nextInput) {
        for (int i = 0; i < kFloodPerInput; ++i) send(b, cbmp::Event{"spam", {static_cast<uint8_t>(i)}});
      }
    }
    if (t >= b.nextInput) {
      sendInput(b, t);
      b.nextInput += kInputPeriod;
      if (b.nextInput < t) b.nextInput = t + kInputPeriod;  // atrasou muito: não tenta compensar
    }
    if (t >= b.nextEcho) {
      send(b, cbmp::Event{"echo", encodeEcho(t)});  // respondido pelo resource Lua de exemplo `echo`
      ++b.echoSent;
      b.nextEcho = t + kEchoPeriod;
    }
  }

  void pumpNetwork() {
    gns->RunCallbacks();
    SteamNetworkingMessage_t* msgs[256];
    for (;;) {
      const int n = gns->ReceiveMessagesOnPollGroup(poll, msgs, 256);
      if (n <= 0) break;
      const double t = now();
      for (int i = 0; i < n; ++i) {
        auto it = byConn.find(msgs[i]->m_conn);
        if (it != byConn.end())
          onMessage(*it->second, static_cast<const uint8_t*>(msgs[i]->m_pData),
                    static_cast<size_t>(msgs[i]->m_cbSize), t);
        msgs[i]->Release();
      }
      if (n < 256) break;
    }
  }

  nlohmann::json run() {
    SteamNetworkingErrMsg err{};
    if (!GameNetworkingSockets_Init(nullptr, err)) throw std::runtime_error(std::string("GNS: ") + err);
    gns = SteamNetworkingSockets();
    poll = gns->CreatePollGroup();
    gSwarm = this;

    const int n = std::max(0, opt.count);
    bots.resize(static_cast<size_t>(n));
    const int clustered = static_cast<int>(std::lround(std::clamp(opt.cluster, 0.0, 1.0) * n));
    for (int i = 0; i < n; ++i) {
      Bot& b = bots[static_cast<size_t>(i)];
      b.idx = i;
      b.scenario = i == 0 && (opt.teleportEvery > 0 || opt.flood);
      b.clustered = i < clustered;
      b.connectAt = i / std::max(1.0, opt.rampPerSec);
      b.phaseOffset = n > 0 ? kInputPeriod * (static_cast<double>(i % 50) / 50.0) : 0;
      placeInitial(b);
    }

    t0 = Clock::now();
    size_t nextToConnect = 0;
    for (;;) {
      const double t = now();
      if (t >= opt.durationS) break;
      while (nextToConnect < bots.size() && bots[nextToConnect].connectAt <= t) connect(bots[nextToConnect++], t);
      pumpNetwork();
      const double ts = now();
      for (auto& b : bots) step(b, ts);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double end = now();
    for (auto& b : bots) {
      if (b.phase == Phase::Waiting || b.phase == Phase::Done) continue;
      if (!b.welcomed && b.reason.empty()) b.reason = "sem Welcome até o fim";
      close(b, end);
    }
    gSwarm = nullptr;
    gns->DestroyPollGroup(poll);
    // Deixa o GNS mandar os fechamentos antes de encerrar.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    GameNetworkingSockets_Kill();
    return report(end);
  }

  nlohmann::json report(double end) {
    int connected = 0, failed = 0, rejected = 0, disconnected = 0, kicked = 0;
    uint64_t bytesIn = 0, corrections = 0, snapshots = 0;
    double rttSum = 0, maxGap = 0, kbpsSum = 0;
    uint64_t rttN = 0, echoSent = 0;
    int kbpsN = 0, withoutSnapshots = 0;
    for (auto& b : bots) {
      if (!countsForStats(b)) continue;
      if (b.welcomed) {
        ++connected;
        const double until = b.endAt >= 0 ? b.endAt : end;
        // Intervalo final (último snapshot, ou Welcome, até o fim): pega bot que parou de receber ou nunca recebeu.
        recordGap(b, GapStats::tailGap(b.welcomeAt, b.lastSnapAt, until));
        if (b.snapshots == 0) ++withoutSnapshots;
        const double secs = std::max(1e-3, until - b.welcomeAt);
        kbpsSum += static_cast<double>(b.bytesIn) * 8.0 / 1000.0 / secs;
        ++kbpsN;
      } else {
        ++failed;
        if (b.rejected) {
          ++rejected;
          ++rejectReasons[b.reason];
        } else if (!b.reason.empty()) {
          ++closeReasons[b.reason];
        }
      }
      if (b.disconnected) {
        ++disconnected;
        ++closeReasons[b.reason];
      }
      if (b.kicked) {
        ++kicked;
        ++kickReasons[b.reason];
      }
      bytesIn += b.bytesIn;
      corrections += b.corrections;
      snapshots += b.snapshots;
      rttSum += b.rttSum;
      rttN += b.rttN;
      echoSent += b.echoSent;
      maxGap = std::max(maxGap, b.maxGap);
    }
    nlohmann::json r{{"count", static_cast<int>(bots.size())},
                     {"connected", connected},
                     {"failedToConnect", failed},
                     {"rejected", rejected},
                     {"disconnected", disconnected},
                     {"kicked", kicked},
                     {"maxSnapshotGapMs", maxGap * 1000.0},
                     {"p99SnapshotGapMs", gaps.p99Ms()},
                     {"botsWithoutSnapshots", withoutSnapshots},
                     {"avgRttMs", rttN ? rttSum / static_cast<double>(rttN) * 1000.0 : 0.0},
                     {"echoReplies", rttN},
                     {"echoSent", echoSent},
                     {"bytesIn", bytesIn},
                     {"avgKbpsInPerBot", kbpsN ? kbpsSum / kbpsN : 0.0},
                     {"snapshotsReceived", snapshots},
                     {"correctionsReceived", corrections},
                     {"durationS", end},
                     {"rejectReasons", rejectReasons},
                     {"kickReasons", kickReasons},
                     {"closeReasons", closeReasons}};
    if (!bots.empty() && bots[0].scenario) {
      const Bot& b = bots[0];
      r["scenarioBot"] = {{"mode", opt.flood ? "flood" : "teleport"},
                          {"welcomed", b.welcomed},
                          {"kicked", b.kicked},
                          {"reason", b.reason},
                          {"correctionsReceived", b.corrections},
                          {"kickAfterS", b.kicked && b.scenarioStart >= 0 ? b.kickedAt - b.scenarioStart : -1.0}};
    }
    return r;
  }
};

}  // namespace

std::string makeTicket(const std::array<uint8_t, 32>& seed, int index, const std::string& aud, int64_t iat,
                       const std::string& jti) {
  if (sodium_init() < 0) throw std::runtime_error("sodium_init falhou");
  std::array<uint8_t, crypto_sign_PUBLICKEYBYTES> pk{};
  std::array<uint8_t, crypto_sign_SECRETKEYBYTES> sk{};
  crypto_sign_seed_keypair(pk.data(), sk.data(), seed.data());
  const std::string i = std::to_string(index);
  const std::string json = R"({"v":1,"kid":"dev","sub":"bot_)" + i + R"(","did":"9000000000000000)" + i +
                           R"(","name":"Bot )" + i + R"(","aud":")" + aud + R"(","iat":)" + std::to_string(iat) +
                           R"(,"exp":)" + std::to_string(iat + 300) + R"(,"jti":")" + jti + R"("})";
  const std::vector<uint8_t> body(json.begin(), json.end());
  std::array<uint8_t, crypto_sign_BYTES> sig{};
  crypto_sign_detached(sig.data(), nullptr, body.data(), body.size(), sk.data());
  sodium_memzero(sk.data(), sk.size());
  return cbmp::base64urlEncode(body) + "." + cbmp::base64urlEncode(std::vector<uint8_t>(sig.begin(), sig.end()));
}

std::string devPublicKeyB64(const std::array<uint8_t, 32>& seed) {
  if (sodium_init() < 0) throw std::runtime_error("sodium_init falhou");
  std::array<uint8_t, crypto_sign_PUBLICKEYBYTES> pk{};
  std::array<uint8_t, crypto_sign_SECRETKEYBYTES> sk{};
  crypto_sign_seed_keypair(pk.data(), sk.data(), seed.data());
  sodium_memzero(sk.data(), sk.size());
  return cbmp::base64urlEncode(std::vector<uint8_t>(pk.begin(), pk.end()));
}

nlohmann::json runBots(const BotOptions& opt) {
  if (sodium_init() < 0) throw std::runtime_error("sodium_init falhou");
  auto swarm = std::make_unique<Swarm>(opt);
  return swarm->run();
}

}  // namespace cbbots
