#include <cbmp/client/NetClient.hpp>

#include <cbmp/client/ConnectFile.hpp>
#include <cbmp/client/Limits.hpp>
#include <cbmp/log_text.hpp>

#include <exception>
#include <type_traits>
#include <variant>

namespace cbmp::client {
namespace {
// Tamanho da sequência UTF-8 que começa em `lead` (1 para byte inválido: passa sozinho, como no escapeLogText).
size_t Utf8Len(unsigned char lead) {
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    if (lead >= 0xE0) return lead <= 0xEF ? 3 : 1;
    if (lead >= 0xC2) return 2;
    return 1;
}
}  // namespace

std::string SanitizeText(std::string_view raw, size_t maxBytes) {
    std::string out;
    size_t i = 0;
    while (i < raw.size()) {
        size_t n = Utf8Len(static_cast<unsigned char>(raw[i]));
        if (i + n > raw.size()) n = raw.size() - i;
        for (size_t k = 1; k < n; ++k)  // continuação inválida: o líder vai sozinho
            if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80) {
                n = 1;
                break;
            }
        // Cada caractere é escapado inteiro e só entra se couber inteiro: o corte nunca parte UTF-8 nem um escape.
        const std::string piece = cbmp::escapeLogText(raw.substr(i, n));
        if (out.size() + piece.size() > maxBytes) break;
        out += piece;
        i += n;
    }
    return out;
}

NetClient::NetClient(ITransport& transport, INetSink& sink) : m_transport(transport), m_sink(sink) {}

void NetClient::Send(const cbmp::Message& m) {
    const auto bytes = cbmp::encode(m);
    m_transport.Send(bytes, cbmp::isReliable(cbmp::typeOf(m)));
}

bool NetClient::SendHello(const std::array<uint8_t, 32>& buildHash, std::string& ticket) {
    cbmp::Message msg{cbmp::Hello{cbmp::kProtocolVersion, buildHash, ticket}};
    bool ok = false;
    std::vector<uint8_t> bytes;
    try {
        bytes = cbmp::encode(msg);
        ok = m_transport.Send(bytes, true);
    } catch (const std::exception&) {
        ok = false;
    }
    SecureZero(bytes);
    SecureZero(std::get<cbmp::Hello>(msg).ticket);
    SecureZero(ticket);
    return ok;
}

void NetClient::SendInput(uint32_t seq, const cbmp::PlayerState& s) {
    Send(cbmp::Input{seq, s});
    ++m_counters.inputs;
}

void NetClient::SendAck(uint32_t tick) {
    Send(cbmp::SnapshotAck{tick});
    ++m_counters.acks;
}

bool NetClient::NoteDecodeError(double now) {
    ++m_counters.decodeErrors;
    m_errorTimes.push_back(now);
    while (!m_errorTimes.empty() && m_errorTimes.front() < now - limits::kDecodeErrorWindowSec) m_errorTimes.pop_front();
    return m_errorTimes.size() > static_cast<size_t>(limits::kMaxDecodeErrors);
}

NetFault NetClient::Pump(double now) {
    m_buf.clear();
    m_transport.Receive(m_buf, limits::kMaxMessagesPerTick);
    for (const auto& bytes : m_buf) {
        ++m_counters.received;
        if (bytes.size() > limits::kMaxMessageBytes) {
            ++m_counters.oversized;
            continue;
        }
        cbmp::Message msg;
        bool bad = false;
        try {
            msg = cbmp::decode(bytes);
            const auto t = cbmp::typeOf(msg);
            bad = t == cbmp::MsgType::Hello || t == cbmp::MsgType::Input || t == cbmp::MsgType::SnapshotAck;
        } catch (...) {
            bad = true;
        }
        if (bad) {
            if (NoteDecodeError(now)) return NetFault::TooManyDecodeErrors;
            continue;
        }
        std::visit(
            [&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, cbmp::Welcome>) {
                    m_sink.OnWelcome(x);
                } else if constexpr (std::is_same_v<T, cbmp::Snapshot>) {
                    ++m_counters.snapshots;
                    m_sink.OnSnapshot(x, now);
                } else if constexpr (std::is_same_v<T, cbmp::EntityEnter>) {
                    ++m_counters.enters;
                    m_sink.OnEntityEnter(x, now);
                } else if constexpr (std::is_same_v<T, cbmp::EntityLeave>) {
                    ++m_counters.leaves;
                    m_sink.OnEntityLeave(x);
                } else if constexpr (std::is_same_v<T, cbmp::Correction>) {
                    ++m_counters.corrections;
                    m_sink.OnCorrection(x, now);
                } else if constexpr (std::is_same_v<T, cbmp::Reject>) {
                    m_sink.OnRefusal(SanitizeText(x.reason, limits::kMaxReasonBytes), false);
                } else if constexpr (std::is_same_v<T, cbmp::Kick>) {
                    m_sink.OnRefusal(SanitizeText(x.reason, limits::kMaxReasonBytes), true);
                } else if constexpr (std::is_same_v<T, cbmp::Event>) {
                    ++m_counters.events;  // 5a: nenhum evento é tratado; nada do servidor vira caminho ou código
                }
                // Hello/Input/SnapshotAck já foram recusados acima.
            },
            msg);
    }
    m_buf.clear();
    if (m_transport.Pending() > limits::kMaxPendingMessages) return NetFault::QueueOverflow;
    return NetFault::None;
}

const NetCounters& NetClient::Counters() const { return m_counters; }
}  // namespace cbmp::client
