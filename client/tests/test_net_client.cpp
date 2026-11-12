#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/Limits.hpp>
#include <cbmp/client/NetClient.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "FakeTransport.hpp"

using namespace cbmp::client;

namespace {
struct TestSink : INetSink {
    std::vector<cbmp::Welcome> welcomes;
    std::vector<cbmp::Snapshot> snapshots;
    std::vector<cbmp::EntityEnter> enters;
    std::vector<cbmp::EntityLeave> leaves;
    std::vector<cbmp::Correction> corrections;
    std::vector<std::pair<std::string, bool>> refusals;
    std::vector<double> times;

    void OnWelcome(const cbmp::Welcome& w) override { welcomes.push_back(w); }
    void OnSnapshot(const cbmp::Snapshot& s, double now) override {
        snapshots.push_back(s);
        times.push_back(now);
    }
    void OnEntityEnter(const cbmp::EntityEnter& e, double now) override {
        enters.push_back(e);
        times.push_back(now);
    }
    void OnEntityLeave(const cbmp::EntityLeave& e) override { leaves.push_back(e); }
    void OnCorrection(const cbmp::Correction& c, double now) override {
        corrections.push_back(c);
        times.push_back(now);
    }
    void OnRefusal(std::string_view text, bool kick) override { refusals.emplace_back(std::string(text), kick); }

    size_t Total() const {
        return welcomes.size() + snapshots.size() + enters.size() + leaves.size() + corrections.size() + refusals.size();
    }
};

std::vector<uint8_t> Invalid() { return {0xFF, 0x01, 0x02}; }  // tipo desconhecido: cbmp::decode lança

void PushEvents(FakeTransport& t, size_t n) {
    const auto bytes = cbmp::encode(cbmp::Message{cbmp::Event{"cb:reply", {}}});
    for (size_t i = 0; i < n; ++i) t.PushRaw(bytes);
}
}  // namespace

TEST_CASE("client: SendHello manda Hello confiavel e zera o ticket do chamador") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    std::array<uint8_t, 32> hash{};
    for (size_t i = 0; i < hash.size(); ++i) hash[i] = static_cast<uint8_t>(i + 1);
    std::string ticket = "eyJhbGciOiJFZERTQSJ9.eyJleHAiOjE3OTAwMDAwMDB9.assinatura";
    const std::string copy = ticket;

    REQUIRE(net.SendHello(hash, ticket));
    REQUIRE(ticket.empty());
    REQUIRE(t.sent.size() == 1);
    REQUIRE(t.sent[0].reliable);
    const auto msgs = t.SentMessages();
    const auto* h = std::get_if<cbmp::Hello>(&msgs[0]);
    REQUIRE(h != nullptr);
    REQUIRE(h->protocol == 1);
    REQUIRE(h->protocol == cbmp::kProtocolVersion);
    REQUIRE(h->buildHash == hash);
    REQUIRE(h->ticket == copy);
}

TEST_CASE("client: SendHello com ticket grande demais falha e ainda zera o ticket") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    std::string ticket(cbmp::kMaxTicketLen + 1, 'a');
    REQUIRE_FALSE(net.SendHello({}, ticket));
    REQUIRE(ticket.empty());
    REQUIRE(t.sent.empty());
}

TEST_CASE("client: SendInput e SendAck nao sao confiaveis") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    cbmp::PlayerState s{};
    s.pos = {100, -200, 300};
    s.state = cbmp::MoveState::Run;
    net.SendInput(7, s);
    net.SendAck(42);
    REQUIRE(t.sent.size() == 2);
    REQUIRE_FALSE(t.sent[0].reliable);
    REQUIRE_FALSE(t.sent[1].reliable);
    const auto msgs = t.SentMessages();
    const auto& in = std::get<cbmp::Input>(msgs[0]);
    REQUIRE(in.seq == 7);
    REQUIRE(in.s == s);
    REQUIRE(std::get<cbmp::SnapshotAck>(msgs[1]).tick == 42);
    REQUIRE(net.Counters().inputs == 1);
    REQUIRE(net.Counters().acks == 1);
}

TEST_CASE("client: Pump despacha cada tipo para o sink") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    t.Push(cbmp::Welcome{3, 20, 123456});
    cbmp::Snapshot snap{};
    snap.tick = 5;
    t.Push(snap);
    cbmp::EntityEnter enter{};
    enter.id = 9;
    enter.name = "Harry";
    t.Push(enter);
    t.Push(cbmp::EntityLeave{9});
    cbmp::Correction corr{};
    corr.seq = 2;
    corr.pos = {1, 2, 3};
    t.Push(corr);
    t.Push(cbmp::Reject{"ticket invalido"});
    t.Push(cbmp::Kick{"conectado em outro lugar"});

    REQUIRE(net.Pump(12.5) == NetFault::None);
    REQUIRE(sink.welcomes.size() == 1);
    REQUIRE(sink.welcomes[0].playerId == 3);
    REQUIRE(sink.welcomes[0].tickRateHz == 20);
    REQUIRE(sink.snapshots.size() == 1);
    REQUIRE(sink.snapshots[0].tick == 5);
    REQUIRE(sink.enters.size() == 1);
    REQUIRE(sink.enters[0].name == "Harry");
    REQUIRE(sink.leaves.size() == 1);
    REQUIRE(sink.leaves[0].id == 9);
    REQUIRE(sink.corrections.size() == 1);
    REQUIRE(sink.corrections[0].pos == cbmp::Vec3i{1, 2, 3});
    REQUIRE(sink.refusals.size() == 2);
    REQUIRE(sink.refusals[0] == std::pair<std::string, bool>{"ticket invalido", false});
    REQUIRE(sink.refusals[1] == std::pair<std::string, bool>{"conectado em outro lugar", true});
    for (double now : sink.times) REQUIRE(now == 12.5);
    const auto& c = net.Counters();
    REQUIRE(c.received == 7);
    REQUIRE(c.snapshots == 1);
    REQUIRE(c.enters == 1);
    REQUIRE(c.leaves == 1);
    REQUIRE(c.corrections == 1);
    REQUIRE(c.decodeErrors == 0);
}

TEST_CASE("client: Event so conta e e descartado") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    t.Push(cbmp::Event{"cb:reply", {1, 2, 3}});
    REQUIRE(net.Pump(0) == NetFault::None);
    REQUIRE(net.Counters().events == 1);
    REQUIRE(sink.Total() == 0);
}

TEST_CASE("client: mensagem acima de 8 KiB conta em oversized e nao chega ao sink") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    auto big = cbmp::encode(cbmp::Message{cbmp::Welcome{1, 20, 0}});
    big.resize(limits::kMaxMessageBytes + 1, 0);
    t.PushRaw(big);
    REQUIRE(net.Pump(0) == NetFault::None);
    REQUIRE(net.Counters().oversized == 1);
    REQUIRE(net.Counters().decodeErrors == 0);
    REQUIRE(sink.Total() == 0);
}

TEST_CASE("client: 20 erros de decodificacao em 10 s passam e o 21o derruba") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    for (int i = 0; i < 20; ++i) t.PushRaw(Invalid());
    REQUIRE(net.Pump(1.0) == NetFault::None);
    REQUIRE(net.Counters().decodeErrors == 20);
    t.PushRaw(Invalid());
    REQUIRE(net.Pump(9.0) == NetFault::TooManyDecodeErrors);
}

TEST_CASE("client: 21 erros espalhados em 30 s nao derrubam") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    for (int i = 0; i < 21; ++i) {
        t.PushRaw(Invalid());
        REQUIRE(net.Pump(i * 1.5) == NetFault::None);
    }
    REQUIRE(net.Counters().decodeErrors == 21);
}

TEST_CASE("client: mensagens no sentido errado contam como erro de decodificacao") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    t.Push(cbmp::Input{1, {}});
    t.Push(cbmp::Hello{});
    t.Push(cbmp::SnapshotAck{4});
    REQUIRE(net.Pump(0) == NetFault::None);
    REQUIRE(net.Counters().decodeErrors == 3);
    REQUIRE(sink.Total() == 0);
}

TEST_CASE("client: Pump processa no maximo 512 mensagens por chamada") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    PushEvents(t, 600);
    REQUIRE(net.Pump(0) == NetFault::None);
    REQUIRE(net.Counters().received == 512);
    REQUIRE(net.Counters().events == 512);
    REQUIRE(t.Pending() == 88);
    REQUIRE(net.Pump(0.05) == NetFault::None);
    REQUIRE(net.Counters().events == 600);
}

TEST_CASE("client: fila acima de 4096 depois do Pump derruba") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    PushEvents(t, 512 + 4096);
    REQUIRE(net.Pump(0) == NetFault::None);  // sobram 4096: no limite
    t.inbox.clear();
    PushEvents(t, 512 + 4097);
    REQUIRE(net.Pump(0) == NetFault::QueueOverflow);
}

TEST_CASE("client: texto de Reject e saneado e cortado em 255 bytes") {
    FakeTransport t;
    TestSink sink;
    NetClient net(t, sink);
    // O protocolo limita o motivo a 255 bytes; com '\n' escapado ("\\n") o texto saneado ficaria maior.
    std::string reason;
    while (reason.size() < cbmp::kMaxReason) reason += (reason.size() % 3 == 0) ? '\n' : 'x';
    t.Push(cbmp::Reject{reason});
    REQUIRE(net.Pump(0) == NetFault::None);
    REQUIRE(sink.refusals.size() == 1);
    const auto& text = sink.refusals[0].first;
    REQUIRE(text.size() <= limits::kMaxReasonBytes);
    REQUIRE(text.find('\n') == std::string::npos);
    REQUIRE(text.starts_with("\\nxx\\nxx"));
}

TEST_CASE("client: SanitizeText corta 300 bytes com quebra de linha em 255") {
    std::string raw(150, 'a');
    raw += '\n';
    raw += std::string(149, 'b');
    REQUIRE(raw.size() == 300);
    const auto out = SanitizeText(raw, 255);
    REQUIRE(out.size() == 255);
    REQUIRE(out.find('\n') == std::string::npos);
    REQUIRE(out.substr(150, 2) == "\\n");
}

TEST_CASE("client: SanitizeText nao parte caractere UTF-8 nem escape no corte") {
    const std::string euro = "\xE2\x82\xAC";  // 3 bytes
    REQUIRE(SanitizeText("ab" + euro, 5) == "ab" + euro);
    REQUIRE(SanitizeText("ab" + euro, 4) == "ab");
    REQUIRE(SanitizeText("abc" + euro + "d", 5) == "abc");
    REQUIRE(SanitizeText("a\nb", 2) == "a");     // "\\n" não cabe inteiro
    REQUIRE(SanitizeText("a\nb", 3) == "a\\n");
    REQUIRE(SanitizeText("\xE2\x80\xA8", 7).empty());  // U+2028 vira "\\u{2028}" (8 bytes)
    REQUIRE(SanitizeText("\xE2\x80\xA8", 8) == "\\u{2028}");
    REQUIRE(SanitizeText("", 10).empty());
}
