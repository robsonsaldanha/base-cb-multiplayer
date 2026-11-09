#include "TestFramework.hpp"
#include <cbmp/Transform.hpp>
#include <cmath>

using namespace cbmp;

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

TEST(format_transform_mostra_posicao_e_rotacao) {
    Transform t{{1.5, -2.0, 300.25}, {0.0, 90.0, -5.5}};
    CHECK_EQ(FormatTransform(t), std::string("pos=(1.50, -2.00, 300.25) rot=(p 0.00, y 90.00, r -5.50)"));
}

TEST(offset_forward_yaw_zero_anda_em_x) {
    Transform t{{100, 100, 50}, {0, 0, 0}};
    Transform r = OffsetForward(t, 200);
    CHECK(Near(r.pos.x, 300)); CHECK(Near(r.pos.y, 100)); CHECK(Near(r.pos.z, 50));
    CHECK(Near(r.rot.yaw, 0));
}

TEST(offset_forward_yaw_90_anda_em_y) {
    Transform t{{0, 0, 0}, {10, 90, 0}};
    Transform r = OffsetForward(t, 200);
    CHECK(Near(r.pos.x, 0)); CHECK(Near(r.pos.y, 200));
    CHECK(Near(r.rot.pitch, 10)); // rotação preservada
}
