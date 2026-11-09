#include <cbmp/SpikeController.hpp>
#include <format>

namespace cbmp {
SpikeController::SpikeController(IGame& game, LogFn log) : m_game(game), m_log(std::move(log)) {}

void SpikeController::ReportError(const std::string& what) {
    ++m_errors;
    m_log(Level::Error, std::format("erro do jogo ({}/{}): {}", m_errors, kMaxErrors, what));
    if (m_errors >= kMaxErrors) {
        m_disabled = true;
        m_log(Level::Error, "mod desativado após erros repetidos; mande o cb-mp.log");
    }
}

std::string SpikeController::SafeDiagnose() {
    try {
        return m_game.Diagnose();
    } catch (const std::exception& e) {
        return std::string("diag falhou: ") + e.what();
    } catch (...) {
        return "diag falhou: exceção desconhecida";
    }
}

void SpikeController::Tick(double now) {
    if (m_disabled) return;
    if (!m_start) m_start = now;
    // Consulta o jogo só 1x/s (não a cada frame). O timer avança antes da consulta,
    // então um erro também espera o próximo segundo.
    if (m_lastTick && now - *m_lastTick < 1.0) return;
    m_lastTick = now;
    try {
        auto player = m_game.GetLocalPlayer();
        if (!player) {
            if (now - *m_start - m_lastWaitWarn >= kWaitWarnInterval) {
                m_lastWaitWarn = now - *m_start;
                m_log(Level::Warn, std::format("aguardando player há {:.0f} s (menu/carregamento?) | diag: {}",
                                               m_lastWaitWarn, SafeDiagnose()));
            }
            return;
        }
        if (!m_described) {
            m_described = true;
            m_log(Level::Info, "player encontrado: " + m_game.Describe(*player));
        }
        if (auto t = m_game.GetTransform(*player)) {
            m_log(Level::Info, FormatTransform(*t));
        } else {
            m_log(Level::Warn, "não consegui ler o transform do player");
        }
    } catch (const std::exception& e) {
        ReportError(e.what());
    }
}

void SpikeController::OnToggleClone() {
    if (m_disabled) return;
    try {
        if (m_clone) {
            m_game.Destroy(*m_clone);
            m_clone.reset();
            m_log(Level::Info, "F8: clone removido");
            return;
        }
        auto player = m_game.GetLocalPlayer();
        if (!player) { m_log(Level::Warn, "F8: player não encontrado"); return; }
        auto t = m_game.GetTransform(*player);
        if (!t) { m_log(Level::Warn, "F8: sem transform do player"); return; }
        const Transform at = OffsetForward(*t, kCloneDistanceCm);
        m_clone = m_game.SpawnClone(at);
        if (m_clone) m_log(Level::Info, "F8: clone criado em " + FormatTransform(at) + " -> " + m_game.Describe(*m_clone));
        else m_log(Level::Warn, "F8: spawn do clone falhou (ver linhas acima)");
    } catch (const std::exception& e) {
        ReportError(e.what());
    }
}
}
