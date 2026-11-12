#pragma once
// Mensagens do cliente sobre um ITransport: codifica o que sai, decodifica e confere o que chega (servidor não
// confiável: tamanho, formato, sentido, enxurrada) e despacha para um INetSink.
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include <cbmp/client/Transport.hpp>
#include <cbmp/messages.hpp>

namespace cbmp::client {
struct NetCounters {
    uint64_t received = 0, oversized = 0, decodeErrors = 0, snapshots = 0, acks = 0, events = 0, corrections = 0,
             enters = 0, leaves = 0, inputs = 0;
};

// Os métodos não devem lançar: uma exceção sai do Pump no meio do lote e as mensagens restantes já recebidas do
// transporte (inclusive confiáveis, como EntityLeave) se perdem. Quem implementa contém as próprias falhas.
class INetSink {
public:
    virtual ~INetSink() = default;
    virtual void OnWelcome(const cbmp::Welcome& w) = 0;
    virtual void OnSnapshot(const cbmp::Snapshot& s, double now) = 0;
    virtual void OnEntityEnter(const cbmp::EntityEnter& e, double now) = 0;
    virtual void OnEntityLeave(const cbmp::EntityLeave& e) = 0;
    virtual void OnCorrection(const cbmp::Correction& c, double now) = 0;
    virtual void OnRefusal(std::string_view sanitizedText, bool kick) = 0;  // Reject (kick=false) ou Kick
};

enum class NetFault { None, TooManyDecodeErrors, QueueOverflow };

class NetClient {
public:
    NetClient(ITransport& transport, INetSink& sink);

    // Confiável. Depois do Send, zera o buffer codificado, a cópia na mensagem e `ticket` (que fica vazio), mesmo se
    // falhar. false = não codificou ou o transporte recusou.
    bool SendHello(const std::array<uint8_t, 32>& buildHash, std::string& ticket);
    void SendInput(uint32_t seq, const cbmp::PlayerState& s);  // não confiável; `inputs` conta só os aceitos
    void SendAck(uint32_t tick);                               // não confiável; `acks` conta só os aceitos
    // Processa até kMaxMessagesPerTick mensagens recebidas. Diferente de None: desconecte.
    NetFault Pump(double now);
    const NetCounters& Counters() const;

private:
    bool Send(const cbmp::Message& m);  // false = o transporte recusou
    bool NoteDecodeError(double now);  // true = passou do limite na janela

    ITransport& m_transport;
    INetSink& m_sink;
    NetCounters m_counters;
    std::deque<double> m_errorTimes;
    std::vector<std::vector<uint8_t>> m_buf;
};

// Texto vindo do servidor para log ou tela: aplica cbmp::escapeLogText e corta em `maxBytes` (resultado já escapado)
// sem partir um caractere UTF-8 nem uma sequência de escape.
std::string SanitizeText(std::string_view raw, size_t maxBytes);
}  // namespace cbmp::client
