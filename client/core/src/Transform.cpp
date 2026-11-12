#include <cbmp/client/Transform.hpp>
#include <cmath>
#include <format>
#include <numbers>

namespace cbmp::client {
std::string FormatTransform(const Transform& t) {
    return std::format("pos=({:.2f}, {:.2f}, {:.2f}) rot=(p {:.2f}, y {:.2f}, r {:.2f})",
                       t.pos.x, t.pos.y, t.pos.z, t.rot.pitch, t.rot.yaw, t.rot.roll);
}

Transform OffsetForward(const Transform& t, double distanceCm) {
    const double rad = t.rot.yaw * std::numbers::pi / 180.0;
    Transform r = t;
    r.pos.x += std::cos(rad) * distanceCm;
    r.pos.y += std::sin(rad) * distanceCm;
    return r;
}
}
