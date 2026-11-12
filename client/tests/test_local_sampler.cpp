#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/LocalSampler.hpp>
#include <cbmp/quantize.hpp>

#include <limits>

using namespace cbmp::client;
using cbmp::MoveState;

namespace {
LocalState St(double x, double y = 0, double z = 0, MoveMode mode = MoveMode::Ground) {
    LocalState s{};
    s.pos = {x, y, z};
    s.yawDeg = 0;
    s.pitchDeg = 0;
    s.vel = {};
    s.mode = mode;
    return s;
}
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
}  // namespace

TEST_CASE("client: LocalSampler quantiza posicao, yaw, pitch e velocidade") {
    LocalSampler ls;
    LocalState s = St(1.5, -1.5, 300.4);
    s.yawDeg = 90;
    s.pitchDeg = 45;  // ignorado: o 5a envia pitch 0
    s.vel = {100.4, -200.6, 0};
    const auto p = ls.Sample(s);
    REQUIRE(p);
    REQUIRE(p->pos == cbmp::Vec3i{2, -2, 300});
    REQUIRE(p->yaw == cbmp::quantizeYaw(90.0f));
    REQUIRE(p->pitch == cbmp::quantizePitch(0.0f));
    REQUIRE(p->vel == cbmp::Vec3s{100, -201, 0});
    REQUIRE(p->state == MoveState::Walk);  // |v| ≈ 224

    // yaw fora de 0..360 normalizado
    LocalState w = St(0);
    w.yawDeg = 450;
    REQUIRE(ls.Sample(w)->yaw == cbmp::quantizeYaw(90.0f));
    w.yawDeg = -90;
    REQUIRE(ls.Sample(w)->yaw == cbmp::quantizeYaw(270.0f));
    w.yawDeg = 1e300;  // finito e enorme: não estoura a conversão para float
    REQUIRE(ls.Sample(w));
}

TEST_CASE("client: LocalSampler pula NaN e inf em cada campo") {
    for (const double bad : {kNaN, kInf, -kInf}) {
        for (int field = 0; field < 7; ++field) {
            LocalSampler ls;
            LocalState s = St(100, 200, 300);
            switch (field) {
                case 0: s.pos.x = bad; break;
                case 1: s.pos.y = bad; break;
                case 2: s.pos.z = bad; break;
                case 3: s.yawDeg = bad; break;
                case 4: s.vel.x = bad; break;
                case 5: s.vel.y = bad; break;
                default: s.vel.z = bad; break;
            }
            INFO("campo " << field << " valor " << bad);
            REQUIRE_FALSE(ls.Sample(s));
        }
    }
}

TEST_CASE("client: LocalSampler pula posicao fora de 2 000 000 000 cm") {
    LocalSampler ls;
    REQUIRE_FALSE(ls.Sample(St(2'000'000'001.0)));
    REQUIRE_FALSE(ls.Sample(St(0, -2'000'000'001.0)));
    REQUIRE_FALSE(ls.Sample(St(0, 0, 2'000'000'001.0)));
    const auto p = ls.Sample(St(2'000'000'000.0, -2'000'000'000.0, 0));
    REQUIRE(p);
    REQUIRE(p->pos.x == 2'000'000'000);
    REQUIRE(p->pos.y == -2'000'000'000);
}

TEST_CASE("client: LocalSampler limita a velocidade a 32767 por eixo") {
    LocalSampler ls;
    LocalState s = St(0);
    s.vel = {40000, -40000, 32767.4};
    const auto p = ls.Sample(s);
    REQUIRE(p);
    REQUIRE(p->vel == cbmp::Vec3s{32767, -32767, 32767});
    REQUIRE(p->state == MoveState::Broom);
}

TEST_CASE("client: ClassifyMove mapeia modo e velocidade") {
    REQUIRE(ClassifyMove(MoveMode::Falling, 0) == MoveState::Fall);
    REQUIRE(ClassifyMove(MoveMode::Falling, 5000) == MoveState::Fall);
    REQUIRE(ClassifyMove(MoveMode::Swimming, 0) == MoveState::Swim);
    REQUIRE(ClassifyMove(MoveMode::Swimming, 5000) == MoveState::Swim);
    REQUIRE(ClassifyMove(MoveMode::Flying, 0) == MoveState::Broom);
    for (const MoveMode m : {MoveMode::Ground, MoveMode::Other}) {
        REQUIRE(ClassifyMove(m, 0) == MoveState::Idle);
        REQUIRE(ClassifyMove(m, 9.9) == MoveState::Idle);
        REQUIRE(ClassifyMove(m, 10) == MoveState::Walk);
        REQUIRE(ClassifyMove(m, 380) == MoveState::Walk);
        REQUIRE(ClassifyMove(m, 380.1) == MoveState::Run);
        REQUIRE(ClassifyMove(m, 1000) == MoveState::Run);
        REQUIRE(ClassifyMove(m, 1000.1) == MoveState::Broom);
    }
}

TEST_CASE("client: LocalSampler usa a norma da velocidade no estado") {
    LocalSampler ls;
    LocalState s = St(0, 0, 0, MoveMode::Ground);
    s.vel = {300, 300, 0};  // 424 cm/s
    REQUIRE(ls.Sample(s)->state == MoveState::Run);
    s.mode = MoveMode::Swimming;
    REQUIRE(ls.Sample(s)->state == MoveState::Swim);
}

TEST_CASE("client: LocalSampler MarkJump vale para o proximo Sample so") {
    LocalSampler ls;
    REQUIRE(ls.Sample(St(0))->state == MoveState::Idle);
    REQUIRE_FALSE(ls.LastWasJump());
    ls.MarkJump();
    REQUIRE(ls.Sample(St(10))->state == MoveState::Apparate);
    REQUIRE(ls.LastWasJump());
    REQUIRE(ls.Sample(St(20))->state == MoveState::Idle);
    REQUIRE_FALSE(ls.LastWasJump());

    // MarkJump antes do primeiro Sample (entrada em InWorld)
    LocalSampler first;
    first.MarkJump();
    REQUIRE(first.Sample(St(5000))->state == MoveState::Apparate);
    REQUIRE(first.Sample(St(5000))->state == MoveState::Idle);
}

TEST_CASE("client: LocalSampler salto de mais de 3000 cm vira Apparate") {
    LocalSampler ls;
    REQUIRE(ls.Sample(St(0))->state == MoveState::Idle);
    REQUIRE(ls.Sample(St(3000))->state == MoveState::Idle);      // exatamente 3000: não
    REQUIRE(ls.Sample(St(6001))->state == MoveState::Apparate);  // 3001
    REQUIRE(ls.LastWasJump());
    REQUIRE(ls.Sample(St(6001, 3000))->state == MoveState::Idle);
    REQUIRE(ls.Sample(St(6001, 3000, 3001))->state == MoveState::Apparate);  // distância 3D
}

TEST_CASE("client: LocalSampler amostra pulada nao muda a referencia do salto nem consome a marca") {
    LocalSampler ls;
    REQUIRE(ls.Sample(St(0)));
    REQUIRE_FALSE(ls.Sample(St(kNaN)));
    REQUIRE_FALSE(ls.Sample(St(2'500'000'000.0)));
    REQUIRE(ls.Sample(St(3001))->state == MoveState::Apparate);  // referência continua em 0

    ls.MarkJump();
    REQUIRE_FALSE(ls.Sample(St(kInf)));
    REQUIRE(ls.Sample(St(3001))->state == MoveState::Apparate);  // a marca não foi consumida
    REQUIRE(ls.Sample(St(3001))->state == MoveState::Idle);
}
