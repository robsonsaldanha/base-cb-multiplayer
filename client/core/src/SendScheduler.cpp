#include <cbmp/client/SendScheduler.hpp>

namespace cbmp::client {
namespace {
constexpr double kTimeEps = 1e-9;  // arredondamento de soma de tempos (s)
}

void SendScheduler::SetRate(int hz) {
    if (hz <= 0) return;
    m_period = 1.0 / hz;
}

void SendScheduler::Reset() { m_armed = true; }

bool SendScheduler::Due(double now) {
    if (m_armed) {
        m_armed = false;
        m_next = now + m_period;
        return true;
    }
    if (now + kTimeEps < m_next) return false;
    // Cadência fixa (sem deriva com quadros de 16 ms); atrasou mais de um período: recomeça daqui, sem rajada.
    m_next += m_period;
    if (m_next <= now + kTimeEps) m_next = now + m_period;
    return true;
}
}  // namespace cbmp::client
