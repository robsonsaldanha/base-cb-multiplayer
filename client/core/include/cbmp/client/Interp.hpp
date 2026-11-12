#pragma once
// Interpolação das entidades remotas. Todas as amostras ficam no relógio local: uma amostra de snapshot vale
// tick/hz + deslocamento, onde o deslocamento é o mínimo de (chegada − tick/hz) nos últimos 10 s; mudança de mais
// de 2 s limpa as filas. A pose é desenhada `atraso` no passado (100 ms, até 400 ms conforme o intervalo observado
// da entidade), linear entre amostras, com yaw pelo menor arco, extrapolada no máximo 250 ms depois da última e
// sem deslizar em saltos (> 500 cm ou Apparate).
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>

#include <cbmp/client/Limits.hpp>
#include <cbmp/client/Transform.hpp>
#include <cbmp/messages.hpp>

namespace cbmp::client {
struct InterpPose {
    Vec3 pos;
    double yawDeg = 0;
    Vec3 vel;
    uint8_t moveState = 0;
    bool snapped = false;  // primeira pose da entidade, ou a mais de 500 cm da pose devolvida antes
};

class Interp {
public:
    void SetTickRate(int hz);                                                  // hz ≤ 0: ignorado
    void OnTick(uint32_t tick, double arrival);                                // um por tick aplicado: alimenta o relógio
    void Push(uint16_t id, uint32_t tick, const cbmp::PlayerState& s);         // amostra de snapshot (sem OnTick: ignora)
    void PushNow(uint16_t id, const cbmp::PlayerState& s, double arrival);     // EntityEnter (sem tick)
    void Remove(uint16_t id);
    void Clear();                                                              // filas, poses e relógio
    std::optional<InterpPose> Sample(uint16_t id, double now);                 // nullopt = sem amostra
    double DelayOf(uint16_t id) const;

private:
    struct Point {
        double t = 0;
        cbmp::PlayerState s;
    };
    struct Track {
        std::deque<Point> q;
        bool hasLast = false;
        Vec3 last;  // última pose devolvida
    };
    struct Candidate {
        double arrival = 0, value = 0;
    };

    void Add(uint16_t id, double t, const cbmp::PlayerState& s);

    double m_hz = limits::kDefaultSendHz;
    std::deque<Candidate> m_window;  // mínimo deslizante: valores estritamente crescentes
    bool m_hasOffset = false;
    double m_offset = 0;
    std::map<uint16_t, Track> m_tracks;
};
}  // namespace cbmp::client
