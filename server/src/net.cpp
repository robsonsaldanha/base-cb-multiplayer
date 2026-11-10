#include "cbserver/net.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <span>
#include <utility>

#include <spdlog/spdlog.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

namespace cbserver {
namespace {

static_assert(sizeof(HSteamNetConnection) == sizeof(uint32_t));
static_assert(sizeof(HSteamListenSocket) == sizeof(uint32_t));
static_assert(sizeof(HSteamNetPollGroup) == sizeof(uint32_t));

constexpr int kEndRejected = k_ESteamNetConnectionEnd_App_Min + 1;
constexpr int kEndLimits = k_ESteamNetConnectionEnd_App_Min + 2;
constexpr int kEndProtocol = k_ESteamNetConnectionEnd_App_Min + 3;
constexpr int kEndKicked = k_ESteamNetConnectionEnd_App_Min + 4;
constexpr int kMaxBatch = 256;
constexpr int kRecvMaxMessageSize = 8192;   // Hello ≤ 4 KiB, Event ≤ 4 KiB + cabeçalho
constexpr int kRecvBufferSize = 64 * 1024;
constexpr int kRecvBufferMessages = 256;
constexpr int kRecvMaxSegmentsPerPacket = 32;
constexpr int kPortMin = 20000, kPortMax = 32767;  // start(0): fora da faixa efêmera dos SOs
constexpr int kPortAttempts = 20;

// listen socket → servidor dono (o callback do GNS é uma função livre).
std::mutex gServersMu;
std::unordered_map<HSteamListenSocket, NetServer*> gServers;

bool clientMayAuthSend(cbmp::MsgType t) {
  return t == cbmp::MsgType::Input || t == cbmp::MsgType::SnapshotAck || t == cbmp::MsgType::Event;
}

}  // namespace

std::string pendingIpKey(const std::array<uint8_t, 16>& ip) {
  SteamNetworkingIPAddr a;
  a.SetIPv6(ip.data(), 0);
  if (a.IsIPv4()) {
    char buf[SteamNetworkingIPAddr::k_cchMaxString];
    a.ToString(buf, sizeof(buf), false);
    return buf;
  }
  static const char* kHex = "0123456789abcdef";
  std::string k;
  for (size_t i = 0; i < 8; ++i) {
    k += kHex[ip[i] >> 4];
    k += kHex[ip[i] & 15];
    if (i % 2 == 1) k += ':';
  }
  return k + ":/64";
}

bool initNetworking(std::string& err) {
  static std::once_flag once;
  static bool ok = false;
  static std::string msg;
  std::call_once(once, [] {
    SteamNetworkingErrMsg e{};
    ok = GameNetworkingSockets_Init(nullptr, e);
    if (!ok)
      msg = e;
    else  // encerra a thread de serviço do GNS antes do descarregamento da DLL (senão o processo não termina)
      std::atexit([] { GameNetworkingSockets_Kill(); });
  });
  err = msg;
  return ok;
}

NetServer::NetServer(const ServerConfig& cfg, Authenticator& auth, World& world, Metrics& metrics, NetOptions opt)
    : cfg_(cfg), auth_(auth), world_(world), metrics_(metrics), opt_(opt) {}

NetServer::~NetServer() { stop(); }

bool NetServer::start(uint16_t port, std::string& err) {
  if (listen_ != k_HSteamListenSocket_Invalid) {
    err = "rede já iniciada";
    return false;
  }
  if (!initNetworking(err)) {
    err = "GameNetworkingSockets_Init falhou: " + err;
    return false;
  }
  auto* sockets = SteamNetworkingSockets();
  // ponteiro de função → void* sem cast entre função/objeto (aviso no GCC -Wpedantic)
  FnSteamNetConnectionStatusChanged fn = &NetServer::statusThunk;
  void* fnPtr = nullptr;
  static_assert(sizeof(fn) == sizeof(fnPtr));
  std::memcpy(&fnPtr, &fn, sizeof(fnPtr));
  // Configurações do listen socket, herdadas pelas conexões aceitas nele.
  std::vector<SteamNetworkingConfigValue_t> opts(5);
  opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, fnPtr);
  opts[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, kRecvMaxMessageSize);
  opts[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, kRecvBufferSize);
  opts[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, kRecvBufferMessages);
  opts[4].SetInt32(k_ESteamNetworkingConfig_RecvMaxSegmentsPerPacket, kRecvMaxSegmentsPerPacket);
  if (opt_.sendBufferBytes > 0) {
    opts.emplace_back();
    opts.back().SetInt32(k_ESteamNetworkingConfig_SendBufferSize, opt_.sendBufferBytes);
  }
  SteamNetworkingIPAddr base;
  base.Clear();
  if (!opt_.bindAddress.empty() && !base.ParseString(opt_.bindAddress.c_str())) {
    err = "endereço de escuta inválido: " + opt_.bindAddress;
    return false;
  }
  auto listenOn = [&](uint16_t p) {
    SteamNetworkingIPAddr addr = base;
    addr.m_port = p;
    return sockets->CreateListenSocketIP(addr, static_cast<int>(opts.size()), opts.data());
  };
  HSteamListenSocket ls = k_HSteamListenSocket_Invalid;
  if (port != 0) {
    ls = listenOn(port);
  } else {  // o GNS exige porta explícita: sorteia fora da faixa efêmera (onde os clientes se ligam)
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(kPortMin, kPortMax);
    for (int i = 0; i < kPortAttempts && ls == k_HSteamListenSocket_Invalid; ++i)
      ls = listenOn(static_cast<uint16_t>(dist(rng)));
  }
  if (ls == k_HSteamListenSocket_Invalid) {
    err = "não foi possível abrir a porta UDP " + std::to_string(port);
    return false;
  }
  const HSteamNetPollGroup pg = sockets->CreatePollGroup();
  if (pg == k_HSteamNetPollGroup_Invalid) {
    sockets->CloseListenSocket(ls);
    err = "não foi possível criar o poll group";
    return false;
  }
  SteamNetworkingIPAddr bound;
  bound.Clear();
  sockets->GetListenSocketAddress(ls, &bound);
  listen_ = ls;
  poll_ = pg;
  boundPort_ = bound.m_port;
  {
    std::lock_guard lk(gServersMu);
    gServers[ls] = this;
  }
  return true;
}

void NetServer::stop() {
  if (listen_ == k_HSteamListenSocket_Invalid) return;
  auto* sockets = SteamNetworkingSockets();
  for (auto& [conn, s] : sessions_) {
    sendTo(conn, cbmp::Kick{"servidor encerrando"});
    sockets->CloseConnection(conn, kEndKicked, "servidor encerrando", true);
    world_.removePlayer(s.id);
    metrics_.removePlayer(s.id);
  }
  for (auto& [conn, p] : pending_) sockets->CloseConnection(conn, kEndKicked, "servidor encerrando", false);
  sessions_.clear();
  connOf_.clear();
  connBySub_.clear();
  pending_.clear();
  pendingByIp_.clear();
  {
    std::lock_guard lk(gServersMu);
    gServers.erase(listen_);
  }
  sockets->CloseListenSocket(listen_);
  sockets->DestroyPollGroup(poll_);
  listen_ = k_HSteamListenSocket_Invalid;
  poll_ = k_HSteamNetPollGroup_Invalid;
  boundPort_ = 0;
  updateGauges();
}

void NetServer::statusThunk(SteamNetConnectionStatusChangedCallback_t* info) {
  NetServer* s = nullptr;
  {
    std::lock_guard lk(gServersMu);
    auto it = gServers.find(info->m_info.m_hListenSocket);
    if (it != gServers.end()) s = it->second;
  }
  if (s) s->onStatus(info);
}

void NetServer::onStatus(SteamNetConnectionStatusChangedCallback_t* info) {
  auto* sockets = SteamNetworkingSockets();
  const HSteamNetConnection conn = info->m_hConn;
  switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting: {
      if (info->m_eOldState != k_ESteamNetworkingConnectionState_None) break;
      std::array<uint8_t, 16> raw{};
      std::memcpy(raw.data(), info->m_info.m_addrRemote.m_ipv6, raw.size());
      std::string ip = pendingIpKey(raw);
      auto byIp = pendingByIp_.find(ip);
      const int fromIp = byIp == pendingByIp_.end() ? 0 : byIp->second;
      if (pending_.size() >= static_cast<size_t>(cfg_.pendingGlobal) || fromIp >= cfg_.pendingPerIp) {
        spdlog::debug("conexão de {} recusada: limite de pendentes", ip);
        sockets->CloseConnection(conn, kEndLimits, "muitas conexões pendentes", false);
        break;
      }
      if (sockets->AcceptConnection(conn) != k_EResultOK || !sockets->SetConnectionPollGroup(conn, poll_)) {
        sockets->CloseConnection(conn, kEndLimits, "falha ao aceitar", false);
        break;
      }
      ++pendingByIp_[ip];
      pending_.emplace(conn, Pending{std::move(ip), now_});
      break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
      if (pending_.count(conn)) {
        dropPending(conn);
      } else if (auto it = sessions_.find(conn); it != sessions_.end()) {
        spdlog::info("jogador {} desconectou", it->second.id);
        world_.removePlayer(it->second.id);
        metrics_.removePlayer(it->second.id);
        connOf_.erase(it->second.id);
        connBySub_.erase(it->second.sub);
        sessions_.erase(it);
      }
      sockets->CloseConnection(conn, 0, nullptr, false);
      break;
    }
    default:
      break;
  }
}

void NetServer::pump(double now, std::vector<Inbound>& in) {
  if (listen_ == k_HSteamListenSocket_Invalid) return;
  now_ = now;
  auto* sockets = SteamNetworkingSockets();
  sockets->RunCallbacks();
  SteamNetworkingMessage_t* msgs[kMaxBatch];
  size_t budget = opt_.maxMessagesPerPump;  // o resto fica na fila do GNS para o próximo pump
  while (budget > 0) {
    const int want = static_cast<int>(std::min<size_t>(budget, kMaxBatch));
    const int n = sockets->ReceiveMessagesOnPollGroup(poll_, msgs, want);
    if (n <= 0) break;
    budget -= static_cast<size_t>(n);
    for (int i = 0; i < n; ++i) {
      onMessage(msgs[i]->m_conn, static_cast<const uint8_t*>(msgs[i]->m_pData), static_cast<size_t>(msgs[i]->m_cbSize),
                in);
      msgs[i]->Release();
    }
    if (n < want) break;
  }
  // prazo do Hello
  std::vector<uint32_t> expired;
  for (const auto& [conn, p] : pending_)
    if (now - p.t0 > opt_.helloTimeoutSec) expired.push_back(conn);
  for (uint32_t conn : expired) {
    dropPending(conn);
    closeConn(conn, "tempo esgotado para Hello", false);
  }
  if (now - lastWireSample_ >= 1.0) {
    lastWireSample_ = now;
    sampleWire();
  }
  updateGauges();
}

void NetServer::sampleWire() {
  auto* sockets = SteamNetworkingSockets();
  double sum = 0, mx = 0;
  size_t n = 0;
  for (const auto& [conn, s] : sessions_) {
    (void)s;
    SteamNetConnectionRealTimeStatus_t st{};
    if (sockets->GetConnectionRealTimeStatus(conn, &st, 0, nullptr) != k_EResultOK) continue;
    const double kbps = static_cast<double>(st.m_flOutBytesPerSec) * 8.0 / 1000.0;
    sum += kbps;
    mx = std::max(mx, kbps);
    ++n;
  }
  metrics_.setWireOut(n ? sum / static_cast<double>(n) : 0.0, mx);
}

void NetServer::onMessage(uint32_t conn, const uint8_t* data, size_t size, std::vector<Inbound>& in) {
  if (pending_.count(conn)) {
    dropPending(conn);  // a primeira mensagem decide: Hello válido vira sessão, qualquer outra coisa fecha
    if (size > opt_.maxHelloBytes) {
      closeConn(conn, "Hello grande demais", false);
      return;
    }
    cbmp::Message m;
    try {
      m = cbmp::decode(std::span<const uint8_t>(data, size));
    } catch (const cbmp::DecodeError&) {
      ++metrics_.decodeErrors;
      closeConn(conn, "mensagem inválida", false);
      return;
    }
    const auto* h = std::get_if<cbmp::Hello>(&m);
    if (!h) {
      closeConn(conn, "esperado Hello", false);
      return;
    }
    onHello(conn, *h);
    return;
  }
  auto it = sessions_.find(conn);
  if (it == sessions_.end()) return;  // conexão já encerrada neste lote
  cbmp::Message m;
  try {
    m = cbmp::decode(std::span<const uint8_t>(data, size));
  } catch (const cbmp::DecodeError&) {
    ++metrics_.decodeErrors;
    violation(it->second, "mensagem inválida");
    endSession(conn, "mensagem inválida", false);
    return;
  }
  if (!clientMayAuthSend(cbmp::typeOf(m))) {
    violation(it->second, "mensagem não permitida");
    endSession(conn, "mensagem não permitida", false);
    return;
  }
  in.push_back(Inbound{it->second.id, std::move(m)});
}

void NetServer::onHello(uint32_t conn, const cbmp::Hello& h) {
  if (h.protocol != cbmp::kProtocolVersion) {
    reject(conn, "protocolo incompatível");
    return;
  }
  auto t = auth_.check(h.ticket);
  if (!t) {
    reject(conn, t.error());
    return;
  }
  if (auto old = connBySub_.find(t->sub); old != connBySub_.end())
    endSession(old->second, "conectado em outro lugar", true);
  auto id = world_.addPlayer(t->sub, t->did, t->name, now_);
  if (!id) {
    reject(conn, "servidor cheio");
    return;
  }
  sessions_.emplace(conn, Session{*id, t->sub, t->did});
  connOf_[*id] = conn;
  connBySub_[t->sub] = conn;
  cbmp::Welcome w;
  w.playerId = *id;
  w.tickRateHz = static_cast<uint8_t>(cfg_.tickHz);
  w.serverTimeMs = static_cast<uint64_t>(now_ * 1000.0);
  sendTo(conn, w);
  spdlog::info("jogador {} entrou (id {}, did {})", t->name, *id, t->did);
}

int NetServer::sendTo(uint32_t conn, const cbmp::Message& m) {
  const auto bytes = cbmp::encode(m);
  const int flags = cbmp::isReliable(cbmp::typeOf(m)) ? k_nSteamNetworkingSend_Reliable
                                                       : k_nSteamNetworkingSend_UnreliableNoDelay;
  const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
      conn, bytes.data(), static_cast<uint32_t>(bytes.size()), flags, nullptr);
  if (r == k_EResultOK) return static_cast<int>(bytes.size());
  ++metrics_.sendFailures;
  return r == k_EResultLimitExceeded ? -2 : -1;
}

void NetServer::send(const std::vector<Outgoing>& out) {
  slow_.clear();
  for (const auto& o : out) {
    auto it = connOf_.find(o.to);
    if (it == connOf_.end()) continue;
    const int n = sendTo(it->second, o.msg);
    if (n >= 0) {
      metrics_.addBytesOut(o.to, static_cast<size_t>(n));
    } else if (n == -2 && cbmp::isReliable(cbmp::typeOf(o.msg)) &&
               std::find(slow_.begin(), slow_.end(), o.to) == slow_.end()) {
      slow_.push_back(o.to);  // fila confiável cheia: o cliente não acompanha
    }
  }
  for (EntityId id : slow_) kick(id, "conexão lenta demais");
}

void NetServer::violation(const Session& s, const std::string& reason) {
  if (violations_) violations_(s.id, s.sub, s.did, reason);
}

void NetServer::kick(EntityId id, const std::string& reason) {
  auto it = connOf_.find(id);
  if (it == connOf_.end()) {
    world_.removePlayer(id);
    return;
  }
  ++metrics_.kicks;
  endSession(it->second, reason, true);
  updateGauges();
}

void NetServer::closeConn(uint32_t conn, const std::string& reason, bool linger) {
  SteamNetworkingSockets()->CloseConnection(conn, kEndProtocol, reason.c_str(), linger);
}

void NetServer::dropPending(uint32_t conn) {
  auto it = pending_.find(conn);
  if (it == pending_.end()) return;
  auto ip = pendingByIp_.find(it->second.ip);
  if (ip != pendingByIp_.end() && --ip->second <= 0) pendingByIp_.erase(ip);
  pending_.erase(it);
}

void NetServer::reject(uint32_t conn, const std::string& reason) {
  spdlog::info("conexão recusada: {}", reason);
  sendTo(conn, cbmp::Reject{reason});
  SteamNetworkingSockets()->CloseConnection(conn, kEndRejected, reason.c_str(), true);
}

void NetServer::endSession(uint32_t conn, const std::string& reason, bool sendKick) {
  auto it = sessions_.find(conn);
  if (it == sessions_.end()) return;
  const Session s = it->second;
  sessions_.erase(it);
  connOf_.erase(s.id);
  if (auto b = connBySub_.find(s.sub); b != connBySub_.end() && b->second == conn) connBySub_.erase(b);
  if (sendKick) sendTo(conn, cbmp::Kick{reason});
  SteamNetworkingSockets()->CloseConnection(conn, kEndKicked, reason.c_str(), sendKick);
  world_.removePlayer(s.id);
  metrics_.removePlayer(s.id);
  spdlog::info("jogador {} saiu: {}", s.id, reason);
}

void NetServer::updateGauges() {
  metrics_.pending = static_cast<int64_t>(pending_.size());
  metrics_.players = static_cast<int64_t>(world_.playerCount());
}

}  // namespace cbserver
