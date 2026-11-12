#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/Interp.hpp>
#include <cbmp/quantize.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace cbmp::client;
using cbmp::MoveState;

namespace {
cbmp::PlayerState St(int32_t x, int16_t vx = 0, MoveState m = MoveState::Walk, float yawDeg = 0, int32_t y = 0) {
    cbmp::PlayerState s{};
    s.pos = {x, y, 0};
    s.vel = {vx, 0, 0};
    s.yaw = cbmp::quantizeYaw(yawDeg);
    s.state = m;
    return s;
}

bool Near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

// Distância angular até 0 grau (0..180).
double FromZero(double yaw) {
    const double d = std::fmod(std::abs(yaw), 360.0);
    return std::min(d, 360.0 - d);
}

// 20 Hz com relógio sem atraso (deslocamento 0): tick k chega em k/20.
void Feed(Interp& in, uint16_t id, uint32_t tick, const cbmp::PlayerState& s) {
    in.OnTick(tick, tick / 20.0);
    in.Push(id, tick, s);
}
}  // namespace

TEST_CASE("client: Interp desenha 100 ms atras com jitter de 15 ms") {
    Interp in;
    in.SetTickRate(20);
    std::mt19937 rng(20261111u);
    const double latency = 0.080;
    struct Arrival {
        uint32_t tick;
        double at, jitter;
    };
    std::vector<Arrival> arrivals;
    for (uint32_t k = 0; k < 160; ++k) {  // 8 s, entidade andando a 300 cm/s em X (15 cm por tick)
        const double j = (static_cast<int>(rng() % 3001u) - 1500) / 100000.0;  // ±15 ms
        arrivals.push_back({k, k / 20.0 + latency + j, j});
    }
    size_t next = 0;
    double minJitter = std::numeric_limits<double>::infinity();
    int checked = 0;
    for (int frame = 0; frame < 500; ++frame) {
        const double now = frame * 0.016;
        while (next < arrivals.size() && arrivals[next].at <= now) {
            const auto& a = arrivals[next++];
            in.OnTick(a.tick, a.at);
            in.Push(1, a.tick, St(static_cast<int32_t>(15 * a.tick), 300));
            minJitter = std::min(minJitter, a.jitter);
        }
        if (now < 1.0) continue;
        const auto p = in.Sample(1, now);
        REQUIRE(p);
        // Verdade: posição no instante do servidor que corresponde a `now` com a menor latência possível.
        const double truth = 300.0 * (now - (latency - 0.015));
        const double lag = (truth - p->pos.x) / 300.0;
        INFO("now=" << now << " x=" << p->pos.x << " lag=" << lag);
        REQUIRE(lag >= 0.100 - 1e-6);
        REQUIRE(lag <= 0.130 + 1e-6);
        // Contra a posição verdadeira em now − 0,1, descontado o jitter mínimo observado.
        const double expected = 300.0 * (now - 0.1 - (latency + minJitter));
        REQUIRE(std::abs(p->pos.x - expected) <= 5.0);
        REQUIRE(Near(p->vel.x, 300.0));
        REQUIRE(Near(in.DelayOf(1), 0.100, 1e-9));
        ++checked;
    }
    REQUIRE(checked > 400);
}

TEST_CASE("client: Interp extrapola ate 250 ms e para") {
    Interp in;
    for (uint32_t k = 0; k <= 20; ++k) Feed(in, 1, k, St(static_cast<int32_t>(15 * k), 300));
    // última amostra: t = 1,0 s, x = 300, atraso 0,1 s
    REQUIRE(Near(in.Sample(1, 1.1)->pos.x, 300.0));
    REQUIRE(Near(in.Sample(1, 1.2)->pos.x, 330.0));
    const auto at250 = in.Sample(1, 1.1 + 0.250);
    const auto at400 = in.Sample(1, 1.1 + 0.400);
    REQUIRE(Near(at250->pos.x, 375.0));
    REQUIRE(Near(at400->pos.x, 375.0));
    REQUIRE(Near(in.Sample(1, 60.0)->pos.x, 375.0));
}

TEST_CASE("client: Interp antes da primeira amostra devolve a primeira") {
    Interp in;
    Feed(in, 1, 10, St(1000));
    Feed(in, 1, 11, St(1015));
    const auto p = in.Sample(1, 0.0);
    REQUIRE(p);
    REQUIRE(Near(p->pos.x, 1000.0));
    REQUIRE(p->snapped);  // primeiro Sample da entidade
    REQUIRE_FALSE(in.Sample(1, 0.01)->snapped);
    REQUIRE_FALSE(in.Sample(2, 0.0));  // sem amostra
}

TEST_CASE("client: Interp Apparate salta sem posicoes intermediarias") {
    Interp in;
    for (uint32_t k = 0; k <= 10; ++k) {
        in.OnTick(k, k / 20.0);
        in.Push(1, k, St(0, 0, MoveState::Walk));
        in.Push(2, k, St(0, 0, MoveState::Walk));
    }
    in.OnTick(11, 11 / 20.0);
    in.Push(1, 11, St(100, 0, MoveState::Apparate));   // salto curto
    in.Push(2, 11, St(2000, 0, MoveState::Apparate));  // salto longo
    for (uint32_t k = 12; k <= 20; ++k) {
        in.OnTick(k, k / 20.0);
        in.Push(1, k, St(100, 0, MoveState::Idle));
        in.Push(2, k, St(2000, 0, MoveState::Idle));
    }
    int snaps2 = 0;
    bool reached1 = false, reached2 = false;
    for (int i = 0; i <= 1000; ++i) {
        const double now = 0.5 + i * 0.001;
        const auto p1 = in.Sample(1, now);
        const auto p2 = in.Sample(2, now);
        REQUIRE((p1->pos.x == 0.0 || p1->pos.x == 100.0));
        REQUIRE((p2->pos.x == 0.0 || p2->pos.x == 2000.0));
        reached1 = reached1 || p1->pos.x == 100.0;
        reached2 = reached2 || p2->pos.x == 2000.0;
        if (i > 0 && p2->snapped) ++snaps2;
    }
    REQUIRE(reached1);
    REQUIRE(reached2);
    REQUIRE(snaps2 == 1);
}

TEST_CASE("client: Interp salto de 600 cm sem Apparate tambem salta") {
    Interp in;
    for (uint32_t k = 0; k <= 10; ++k) Feed(in, 1, k, St(0));
    Feed(in, 1, 11, St(600));
    for (uint32_t k = 12; k <= 20; ++k) Feed(in, 1, k, St(600));
    int snaps = 0;
    REQUIRE(in.Sample(1, 0.5)->snapped);  // primeiro
    for (int i = 1; i <= 1000; ++i) {
        const auto p = in.Sample(1, 0.5 + i * 0.001);
        REQUIRE((p->pos.x == 0.0 || p->pos.x == 600.0));
        if (p->snapped) ++snaps;
    }
    REQUIRE(snaps == 1);

    // 500 cm ainda desliza
    Interp slide;
    Feed(slide, 1, 0, St(0));
    Feed(slide, 1, 1, St(500));
    const auto mid = slide.Sample(1, 0.025 + 0.1);
    REQUIRE(Near(mid->pos.x, 250.0));
}

TEST_CASE("client: Interp yaw pelo menor arco de 350 para 10 graus") {
    Interp in;
    for (uint32_t k = 0; k <= 10; ++k) Feed(in, 1, k, St(0, 0, MoveState::Idle, 350.0f));
    Feed(in, 1, 11, St(0, 0, MoveState::Idle, 10.0f));
    Feed(in, 1, 12, St(0, 0, MoveState::Idle, 10.0f));
    double best = 360;
    for (int i = 0; i <= 100; ++i) {
        const double now = 0.6 + i * 0.0005;  // r de 0,50 a 0,55
        const auto p = in.Sample(1, now);
        REQUIRE(p->yawDeg >= 0.0);
        REQUIRE(p->yawDeg < 360.0);
        INFO("yaw=" << p->yawDeg);
        REQUIRE(FromZero(p->yawDeg) <= 10.01);  // nunca passa por 180
        best = std::min(best, FromZero(p->yawDeg));
    }
    REQUIRE(best < 0.5);  // passa por 0
    REQUIRE(FromZero(in.Sample(1, 0.625)->yawDeg) < 0.5);  // meio do caminho
}

TEST_CASE("client: Interp atraso por entidade conforme o intervalo") {
    Interp in;
    REQUIRE(in.DelayOf(9) == 0.100);  // desconhecida
    for (uint32_t k = 0; k <= 40; ++k) {
        in.OnTick(k, k / 20.0);
        in.Push(1, k, St(static_cast<int32_t>(k)));                             // a cada 50 ms
        if (k % 4 == 0) in.Push(2, k, St(static_cast<int32_t>(k)));             // a cada 200 ms
        if (k % 3 == 0) in.Push(3, k, St(static_cast<int32_t>(k)));             // a cada 150 ms
        if (k % 20 == 0) in.Push(4, k, St(static_cast<int32_t>(k)));            // a cada 1 s: inválido
        if (k == 0 || k == 30 || k >= 32) in.Push(5, k, St(static_cast<int32_t>(k)));  // últimos 4 de 50 ms
    }
    REQUIRE(Near(in.DelayOf(1), 0.100, 1e-9));
    REQUIRE(Near(in.DelayOf(2), 0.400, 1e-9));
    REQUIRE(Near(in.DelayOf(3), 0.300, 1e-9));
    REQUIRE(Near(in.DelayOf(4), 0.100, 1e-9));
    REQUIRE(Near(in.DelayOf(5), 0.100, 1e-9));

    // a cada 400 ms: limitado a 0,400
    Interp slow;
    for (uint32_t k = 0; k <= 40; ++k) {
        slow.OnTick(k, k / 20.0);
        if (k % 8 == 0) slow.Push(1, k, St(static_cast<int32_t>(k)));
    }
    REQUIRE(Near(slow.DelayOf(1), 0.400, 1e-9));
    // o atraso entra no Sample: amostra em t = 2,0 s desenhada só em now = 2,4
    REQUIRE(Near(slow.Sample(1, 2.4)->pos.x, 40.0));
    REQUIRE(Near(slow.Sample(1, 2.2)->pos.x, 36.0));
}

TEST_CASE("client: Interp relogio saltando 3 s limpa as filas") {
    Interp in;
    for (uint32_t k = 0; k <= 20; ++k) {
        in.OnTick(k, k / 20.0 + 0.05);
        in.Push(1, k, St(static_cast<int32_t>(15 * k)));
        in.Push(2, k, St(0));
    }
    REQUIRE(in.Sample(1, 1.1));
    // o tick do servidor anda 3 s de uma vez (candidato cai 3 s)
    in.OnTick(81, 1.1);
    REQUIRE_FALSE(in.Sample(1, 1.1));
    REQUIRE_FALSE(in.Sample(2, 1.1));
    in.Push(1, 81, St(5000));
    const auto p = in.Sample(1, 1.2);
    REQUIRE(p);
    REQUIRE(Near(p->pos.x, 5000.0));
    REQUIRE(p->snapped);
}

TEST_CASE("client: Interp relogio que salta para frente reinicia quando a janela de 10 s passa") {
    Interp in;
    for (uint32_t k = 0; k <= 20; ++k) {
        in.OnTick(k, k / 20.0);
        in.Push(1, k, St(0));
    }
    // servidor reiniciou: ticks voltam a 0 a partir de 4,0 s (candidato sobe 4 s)
    for (uint32_t k = 0; k <= 300; ++k) {
        const double at = 4.0 + k / 20.0;
        in.OnTick(k, at);
        in.Push(1, k, St(static_cast<int32_t>(10 * k)));
    }
    // em 19 s a janela só tem o relógio novo: a pose segue a amostra de ~0,1 s atrás
    const auto p = in.Sample(1, 19.0);
    REQUIRE(p);
    REQUIRE(std::abs(p->pos.x - 10.0 * (19.0 - 0.1 - 4.0) * 20.0) <= 10.0);
}

TEST_CASE("client: Interp PushNow funciona antes de qualquer tick e Push sem tick e ignorado") {
    Interp in;
    in.PushNow(5, St(100), 2.0);
    const auto p = in.Sample(5, 2.0);
    REQUIRE(p);
    REQUIRE(Near(p->pos.x, 100.0));
    REQUIRE(p->snapped);
    REQUIRE(in.Sample(5, 10.0));

    in.Push(6, 7, St(100));  // sem OnTick: ignora
    REQUIRE_FALSE(in.Sample(6, 2.0));
}

TEST_CASE("client: Interp fila de 32 amostras e amostra velha ignorada") {
    Interp in;
    for (uint32_t k = 0; k < 40; ++k) Feed(in, 1, k, St(static_cast<int32_t>(15 * k)));
    // a mais velha na fila é a de tick 8
    REQUIRE(Near(in.Sample(1, 0.0)->pos.x, 120.0));

    Interp dup;
    Feed(dup, 1, 0, St(0));
    Feed(dup, 1, 1, St(15));
    dup.Push(1, 1, St(9999));  // mesmo t: ignorada
    dup.Push(1, 0, St(9999));  // t menor: ignorada
    dup.PushNow(1, St(9999), 0.01);
    REQUIRE(Near(dup.Sample(1, 0.15)->pos.x, 15.0));
}

TEST_CASE("client: Interp Remove, Clear e now nao finito") {
    Interp in;
    Feed(in, 1, 0, St(0));
    Feed(in, 2, 1, St(0));
    in.Remove(1);
    REQUIRE_FALSE(in.Sample(1, 1.0));
    REQUIRE(in.Sample(2, 1.0));
    REQUIRE_FALSE(in.Sample(2, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(in.Sample(2, std::numeric_limits<double>::infinity()));
    in.Clear();
    REQUIRE_FALSE(in.Sample(2, 1.0));
    in.Push(2, 5, St(0));  // Clear zera o relógio: sem OnTick, ignora
    REQUIRE_FALSE(in.Sample(2, 1.0));
    Feed(in, 2, 6, St(0));
    REQUIRE(in.Sample(2, 1.0)->snapped);  // primeira pose depois do Clear
}

TEST_CASE("client: Interp limita o numero de entidades a 255") {
    Interp in;
    for (uint16_t id = 1; id <= 300; ++id) in.PushNow(id, St(0), 0.0);
    REQUIRE(in.Sample(255, 0.5));
    REQUIRE_FALSE(in.Sample(256, 0.5));
    REQUIRE_FALSE(in.Sample(300, 0.5));
    in.Remove(1);
    in.PushNow(1000, St(0), 0.0);  // vaga liberada
    REQUIRE(in.Sample(1000, 0.5));
    in.PushNow(5, St(10), 0.1);  // id já existente continua aceitando amostras
    REQUIRE(in.Sample(5, 0.5));
}
