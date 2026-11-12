#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cbmp/client/Transform.hpp>
#include <string>

using namespace cbmp::client;
using Catch::Matchers::WithinAbs;

TEST_CASE("client: FormatTransform mostra posicao e rotacao") {
    const Transform t{{1.5, -2.0, 300.25}, {0.0, 90.0, -5.5}};
    REQUIRE(FormatTransform(t) == std::string("pos=(1.50, -2.00, 300.25) rot=(p 0.00, y 90.00, r -5.50)"));
}

TEST_CASE("client: OffsetForward com yaw 0 anda em X") {
    const Transform t{{100, 100, 50}, {0, 0, 0}};
    const Transform r = OffsetForward(t, 200);
    CHECK_THAT(r.pos.x, WithinAbs(300, 1e-6));
    CHECK_THAT(r.pos.y, WithinAbs(100, 1e-6));
    CHECK_THAT(r.pos.z, WithinAbs(50, 1e-6));
    CHECK_THAT(r.rot.yaw, WithinAbs(0, 1e-6));
}

TEST_CASE("client: OffsetForward com yaw 90 anda em Y e preserva a rotacao") {
    const Transform t{{0, 0, 0}, {10, 90, 0}};
    const Transform r = OffsetForward(t, 200);
    CHECK_THAT(r.pos.x, WithinAbs(0, 1e-6));
    CHECK_THAT(r.pos.y, WithinAbs(200, 1e-6));
    CHECK_THAT(r.rot.pitch, WithinAbs(10, 1e-6));
}
