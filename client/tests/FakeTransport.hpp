#pragma once
// Transporte falso para os testes: guarda o que foi enviado e entrega o que o teste põe na caixa de entrada.
#include <cbmp/client/Transport.hpp>
#include <cbmp/messages.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace cbmp::client {
struct FakeTransport : ITransport {
    struct Sent {
        std::vector<uint8_t> bytes;
        bool reliable = false;
    };
    std::vector<Sent> sent;
    std::deque<std::vector<uint8_t>> inbox;
    LinkState state = LinkState::Idle;
    LinkEnd end;
    bool connectOk = true, sendOk = true;
    int connectCalls = 0, closeCalls = 0;
    std::string lastHost;
    uint16_t lastPort = 0;

    void Push(const cbmp::Message& m) { inbox.push_back(cbmp::encode(m)); }
    void PushRaw(std::vector<uint8_t> b) { inbox.push_back(std::move(b)); }
    std::vector<cbmp::Message> SentMessages() const {
        std::vector<cbmp::Message> out;
        for (const auto& s : sent) out.push_back(cbmp::decode(s.bytes));
        return out;
    }

    // Connect: state = Connecting (o teste muda para Connected).
    bool Connect(const std::string& ipLiteral, uint16_t port) override {
        ++connectCalls;
        lastHost = ipLiteral;
        lastPort = port;
        if (!connectOk) return false;
        state = LinkState::Connecting;
        return true;
    }
    bool Send(std::span<const uint8_t> bytes, bool reliable) override {
        if (!sendOk) return false;
        sent.push_back(Sent{std::vector<uint8_t>(bytes.begin(), bytes.end()), reliable});
        return true;
    }
    size_t Receive(std::vector<std::vector<uint8_t>>& out, size_t max) override {
        size_t n = 0;
        while (n < max && !inbox.empty()) {
            out.push_back(std::move(inbox.front()));
            inbox.pop_front();
            ++n;
        }
        return n;
    }
    size_t Pending() const override { return inbox.size(); }
    LinkState State() override { return state; }
    LinkEnd End() const override { return end; }
    void Close() override {
        ++closeCalls;
        state = LinkState::Closed;
    }
};
}  // namespace cbmp::client
