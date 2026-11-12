#include <cbmp/client/GnsTransport.hpp>

#include <cbmp/client/Limits.hpp>

#include <algorithm>
#include <mutex>
#include <type_traits>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

namespace cbmp::client {
// Os códigos de fim da aplicação em Limits.hpp (kEndRejected = 1001 …) contam a partir daqui.
static_assert(k_ESteamNetConnectionEnd_App_Min == 1000);
static_assert(std::is_same_v<HSteamNetConnection, uint32_t>);
static_assert(k_HSteamNetConnection_Invalid == 0);

namespace {
std::mutex g_initMutex;
bool g_initialized = false;

// Fila acima disso para de puxar: Pending() > kMaxPendingMessages já basta para o NetClient desconectar.
constexpr size_t kPullStop = limits::kMaxPendingMessages + 1;
constexpr int kPullBatch = 64;

bool Initialized() {
    std::lock_guard lock(g_initMutex);
    return g_initialized;
}
}  // namespace

bool GnsInit(std::string* err) {
    std::lock_guard lock(g_initMutex);
    if (g_initialized) return true;
    SteamNetworkingErrMsg msg{};
    if (!GameNetworkingSockets_Init(nullptr, msg)) {
        if (err) *err = msg;
        return false;
    }
    g_initialized = true;
    return true;
}

void GnsShutdown() {
    std::lock_guard lock(g_initMutex);
    if (!g_initialized) return;
    GameNetworkingSockets_Kill();
    g_initialized = false;
}

GnsTransport::GnsTransport(GnsOptions opt) : m_opt(opt) {}

GnsTransport::~GnsTransport() { Close(); }

bool GnsTransport::Connect(const std::string& ipLiteral, uint16_t port) {
    if (m_conn != k_HSteamNetConnection_Invalid || !Initialized()) return false;
    const bool v6 = ipLiteral.find(':') != std::string::npos && !ipLiteral.starts_with("[");
    const std::string hostPort = (v6 ? "[" + ipLiteral + "]" : ipLiteral) + ":" + std::to_string(port);
    SteamNetworkingIPAddr addr;
    addr.Clear();
    if (!addr.ParseString(hostPort.c_str()) || addr.m_port == 0) return false;

    if (m_opt.fakeLossPercent > 0) {
        // A perda falsa do GNS é global (aplicada na camada UDP, sem contexto de conexão): vale para o processo
        // inteiro, que no headless tem uma conexão só.
        const float pct = static_cast<float>(std::clamp(m_opt.fakeLossPercent, 0, 100));
        SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, pct);
        SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, pct);
    }
    SteamNetworkingConfigValue_t cfg[1];
    // Mensagem maior que o limite fecha a conexão já na recepção: um lote de 512 nunca aloca centenas de MiB.
    cfg[0].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, static_cast<int32_t>(limits::kMaxMessageBytes));
    const HSteamNetConnection conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 1, cfg);
    if (conn == k_HSteamNetConnection_Invalid) return false;
    m_conn = conn;
    m_state = LinkState::Connecting;
    m_end = {};
    m_queue.clear();
    return true;
}

bool GnsTransport::Send(std::span<const uint8_t> bytes, bool reliable) {
    if (m_conn == k_HSteamNetConnection_Invalid || m_state != LinkState::Connected) return false;
    const int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoDelay;
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, bytes.data(), static_cast<uint32_t>(bytes.size()), flags, nullptr);
    return r == k_EResultOK;
}

void GnsTransport::Pull() {
    if (m_conn == k_HSteamNetConnection_Invalid) return;
    ISteamNetworkingSockets* gns = SteamNetworkingSockets();
    SteamNetworkingMessage_t* msgs[kPullBatch];
    while (m_queue.size() < kPullStop + 1) {
        const int want = static_cast<int>(std::min<size_t>(kPullBatch, kPullStop + 1 - m_queue.size()));
        const int n = gns->ReceiveMessagesOnConnection(m_conn, msgs, want);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            const auto* data = static_cast<const uint8_t*>(msgs[i]->m_pData);
            m_queue.emplace_back(data, data + msgs[i]->m_cbSize);
            msgs[i]->Release();
        }
        if (n < want) break;
    }
}

size_t GnsTransport::Receive(std::vector<std::vector<uint8_t>>& out, size_t max) {
    if (m_conn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->RunCallbacks();
        Pull();
    }
    size_t n = 0;
    while (n < max && !m_queue.empty()) {
        out.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
        ++n;
    }
    return n;
}

size_t GnsTransport::Pending() const { return m_queue.size(); }

void GnsTransport::Finish(int code, std::string debug) {
    if (m_conn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, nullptr, false);
        m_conn = k_HSteamNetConnection_Invalid;
    }
    m_end = LinkEnd{code, std::move(debug)};
    m_state = LinkState::Closed;
}

LinkState GnsTransport::State() {
    if (m_conn == k_HSteamNetConnection_Invalid) return m_state;
    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(m_conn, &info)) {
        m_conn = k_HSteamNetConnection_Invalid;  // o GNS já não conhece a conexão
        Finish(0, "conexão inválida");
        return m_state;
    }
    switch (info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
        case k_ESteamNetworkingConnectionState_FindingRoute: m_state = LinkState::Connecting; break;
        case k_ESteamNetworkingConnectionState_Connected: m_state = LinkState::Connected; break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        default:
            // Reject/Kick chegam antes do fechamento: drena o que ainda está na fila desta conexão.
            Pull();
            Finish(info.m_eEndReason, info.m_szEndDebug);
            break;
    }
    return m_state;
}

LinkEnd GnsTransport::End() const { return m_end; }

void GnsTransport::Close() {
    if (m_conn != k_HSteamNetConnection_Invalid) {
        if (Initialized()) SteamNetworkingSockets()->CloseConnection(m_conn, 0, "fim", false);
        m_conn = k_HSteamNetConnection_Invalid;
    }
    if (m_state != LinkState::Idle) m_state = LinkState::Closed;
}
}  // namespace cbmp::client
