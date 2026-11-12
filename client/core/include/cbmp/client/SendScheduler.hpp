#pragma once
// Cadência do Input (20 Hz por padrão, tickRateHz do Welcome) sem rajada de compensação: um tick atrasado gera um só
// envio e a cadência recomeça a partir dele (como bots/src/bot.cpp).
#include <cbmp/client/Limits.hpp>

namespace cbmp::client {
class SendScheduler {
public:
    void SetRate(int hz);   // período = 1/hz (hz ≤ 0: ignorado)
    void Reset();           // próximo Due é verdadeiro
    bool Due(double now);   // verdadeiro no máximo 1x por período, sem rajada

private:
    double m_period = 1.0 / limits::kDefaultSendHz;
    double m_next = 0;
    bool m_armed = true;  // próximo Due é verdadeiro
};
}  // namespace cbmp::client
