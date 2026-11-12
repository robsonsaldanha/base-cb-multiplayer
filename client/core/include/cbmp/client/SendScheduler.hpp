#pragma once
// Cadência do Input (20 Hz por padrão, tickRateHz do Welcome) sem rajada de compensação: um tick atrasado gera um só
// envio e a cadência recomeça a partir dele (como bots/src/bot.cpp).
#include <cbmp/client/Limits.hpp>

namespace cbmp::client {
class SendScheduler {
public:
    void SetRate(int hz);   // período = 1/hz (hz ≤ 0: ignorado)
    void Reset();           // próximo Due é verdadeiro
    // Cadência fixa do bot de referência: em média 1 verdadeiro por período, sem rajada de compensação. Um quadro
    // atrasado menos de um período pode dar dois verdadeiros com um quadro de intervalo (a média continua 1/período);
    // atraso de um período ou mais gera um só verdadeiro e a cadência recomeça dali.
    bool Due(double now);

private:
    double m_period = 1.0 / limits::kDefaultSendHz;
    double m_next = 0;
    bool m_armed = true;  // próximo Due é verdadeiro
};
}  // namespace cbmp::client
