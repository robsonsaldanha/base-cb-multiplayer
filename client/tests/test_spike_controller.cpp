#include "TestFramework.hpp"
#include "FakeGame.hpp"
#include <cbmp/SpikeController.hpp>
#include <cmath>

using namespace cbmp;

struct Captured { std::vector<std::pair<Level, std::string>> lines; };

static SpikeController::LogFn Capture(Captured& c) {
    return [&c](Level l, const std::string& m) { c.lines.emplace_back(l, m); };
}
static int Count(const Captured& c, const std::string& needle) {
    int n = 0;
    for (auto& [l, m] : c.lines) if (m.find(needle) != std::string::npos) ++n;
    return n;
}

TEST(loga_transform_uma_vez_por_segundo) {
    FakeGame g; g.hasPlayer = true; g.playerTransform = {{1, 2, 3}, {0, 0, 0}};
    Captured c; SpikeController s(g, Capture(c));
    s.Tick(0.0); s.Tick(0.5); s.Tick(1.0); s.Tick(1.6); s.Tick(2.0);
    CHECK_EQ(Count(c, "pos=("), 3); // em 0.0, 1.0 e 2.0
    CHECK_EQ(g.getPlayerCalls, 3);  // consulta o jogo só 1x/s, não a cada frame
}

TEST(descreve_player_quando_encontra) {
    FakeGame g; g.hasPlayer = true;
    Captured c; SpikeController s(g, Capture(c));
    s.Tick(0.0); s.Tick(1.0);
    CHECK_EQ(Count(c, "player encontrado: FakePawn"), 1);
}

TEST(avisa_a_cada_30s_sem_player) {
    FakeGame g;
    Captured c; SpikeController s(g, Capture(c));
    for (double t = 0; t <= 61.0; t += 0.5) s.Tick(t);
    CHECK_EQ(Count(c, "aguardando player"), 2); // em 30 s e 60 s
    CHECK_EQ(Count(c, std::string("diag: ") + FakeGame::kDiag), 2);
    CHECK(!s.Disabled());
}

TEST(diagnose_que_lanca_nao_desativa) {
    FakeGame g; g.throwOnDiagnose = true;
    Captured c; SpikeController s(g, Capture(c));
    for (double t = 0; t <= 200.0; t += 0.5) s.Tick(t); // 6 avisos > kMaxErrors
    CHECK_EQ(Count(c, "diag falhou: diag simulado quebrou"), 6);
    CHECK_EQ(Count(c, "erro do jogo"), 0);
    CHECK(!s.Disabled());
}

TEST(f8_cria_clone_2m_a_frente_e_f8_de_novo_remove) {
    FakeGame g; g.hasPlayer = true; g.playerTransform = {{0, 0, 0}, {0, 0, 0}};
    Captured c; SpikeController s(g, Capture(c));
    s.OnToggleClone();
    CHECK_EQ(g.spawnCalls, 1);
    CHECK(std::fabs(g.spawnedAt.at(0).pos.x - 200.0) < 1e-6);
    s.OnToggleClone();
    CHECK_EQ(g.destroyCalls, 1);
    s.OnToggleClone();
    CHECK_EQ(g.spawnCalls, 2);
}

TEST(f8_sem_player_so_avisa) {
    FakeGame g;
    Captured c; SpikeController s(g, Capture(c));
    s.OnToggleClone();
    CHECK_EQ(g.spawnCalls, 0);
    CHECK_EQ(Count(c, "F8: player não encontrado"), 1);
}

TEST(desativa_apos_5_erros_do_jogo) {
    FakeGame g; g.throwOnGetPlayer = true;
    Captured c; SpikeController s(g, Capture(c));
    for (int i = 0; i < 4; ++i) s.Tick(i * 1.0);
    CHECK(!s.Disabled());
    s.Tick(4.0);
    CHECK(s.Disabled());
    CHECK_EQ(Count(c, "mod desativado"), 1);
    s.Tick(5.0); s.OnToggleClone(); // desativado: não faz nada
    CHECK_EQ(Count(c, "falha simulada"), 5);
}
