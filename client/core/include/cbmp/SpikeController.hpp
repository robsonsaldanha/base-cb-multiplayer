#pragma once
#include <cbmp/IGame.hpp>
#include <cbmp/Log.hpp>
#include <functional>
#include <optional>
#include <string>

namespace cbmp {
class SpikeController {
public:
    using LogFn = std::function<void(Level, const std::string&)>;

    SpikeController(IGame& game, LogFn log);

    void Tick(double nowSeconds); // chamado todo frame, no game thread
    void OnToggleClone();         // F8, no game thread
    bool Disabled() const { return m_disabled; }

    static constexpr int kMaxErrors = 5;
    static constexpr double kWaitWarnInterval = 30.0;
    static constexpr double kCloneDistanceCm = 200.0;

private:
    void ReportError(const std::string& what);
    std::string SafeDiagnose(); // erros do Diagnose não contam para kMaxErrors

    IGame& m_game;
    LogFn m_log;
    bool m_disabled = false;
    int m_errors = 0;
    std::optional<double> m_start;
    std::optional<double> m_lastTick; // última consulta ao jogo (1x/s)
    double m_lastWaitWarn = 0.0;
    bool m_described = false;
    std::optional<PlayerHandle> m_clone;
};
}
