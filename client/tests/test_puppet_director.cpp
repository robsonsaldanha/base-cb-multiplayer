#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/PuppetDirector.hpp>

#include "FakeGame.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace cbmp::client;

namespace {
struct Fx {
    FakeGame game;
    RemoteStore store;
    Interp interp;
    std::vector<std::string> logs;
    PuppetDirector dir;
    Vec3 me{};

    explicit Fx(int maxPuppets = 32)
        : dir(game, store, interp, [this](Level, std::string_view m) { logs.emplace_back(m); }, maxPuppets) {}

    static cbmp::PlayerState St(double x, double y, double z) {
        cbmp::PlayerState s{};
        s.pos = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)};
        return s;
    }
    void Add(uint16_t id, double x, double y = 0, double z = 0, double at = 0) {
        cbmp::EntityEnter e{};
        e.id = id;
        e.s = St(x, y, z);
        e.name = "P" + std::to_string(id);
        REQUIRE(store.Enter(e, at));
        interp.PushNow(id, e.s, at);
    }
    void MoveTo(uint16_t id, double x, double at) {
        const auto s = St(x, 0, 0);
        store.Update(id, s, at);
        interp.PushNow(id, s, at);
    }
    void Run(double from, double to, double step = 0.1) {
        for (int i = 0;; ++i) {
            const double t = from + i * step;
            if (t > to + 1e-9) break;
            dir.Update(t, me);
        }
    }
    int LogCount(std::string_view s) const {
        int n = 0;
        for (const auto& l : logs) n += l.find(s) != std::string::npos ? 1 : 0;
        return n;
    }
    bool LogHas(std::string_view s) const { return LogCount(s) > 0; }
    bool HasPuppetAt(double x) const {
        for (const auto& [id, p] : game.puppets) {
            if (p.pos.x == x) return true;
        }
        return false;
    }
};
}  // namespace

TEST_CASE("client: PuppetDirector cria boneco para entidade conhecida e perto") {
    Fx f;
    f.Add(2, 1000);
    f.dir.Update(1.0, f.me);
    REQUIRE(f.game.spawnCalls == 1);
    REQUIRE(f.game.puppets.size() == 1);
    REQUIRE(f.game.puppets.begin()->second.pos.x == 1000.0);
    REQUIRE(f.LogHas("boneco id=2 criado"));
    REQUIRE(f.LogCount("singleton=ok") == 1);
    REQUIRE(f.dir.Stats().alive == 1);
    REQUIRE(f.dir.Stats().created == 1);
    REQUIRE(f.game.moveCalls == 0);

    f.dir.Update(1.1, f.me);
    REQUIRE(f.game.moveCalls == 1);
    REQUIRE(f.game.spawnCalls == 1);

    f.Add(3, 2000, 0, 0, 1.1);
    f.dir.Update(1.2, f.me);
    REQUIRE(f.game.spawnCalls == 2);
    REQUIRE(f.LogHas("boneco id=3 criado"));
    REQUIRE(f.LogCount("singleton=ok") == 1);  // uma vez por sessão
    REQUIRE(f.dir.Stats().alive == 2);
}

TEST_CASE("client: PuppetDirector nao cria boneco do proprio jogador nem de entidade desconhecida") {
    Fx f;
    f.dir.SetSelf(2);
    f.Add(2, 1000);
    const auto s = Fx::St(500, 0, 0);
    f.interp.PushNow(9, s, 0.0);  // só no Interp, sem EntityEnter
    f.Run(0, 2);
    REQUIRE(f.game.spawnCalls == 0);
    REQUIRE(f.dir.Stats().alive == 0);
}

TEST_CASE("client: PuppetDirector raio de 40 000 cm") {
    Fx f;
    f.Add(2, 40001);
    f.Add(3, 0, 0, -40001);
    f.Add(4, 0, 40000);
    f.dir.Update(1.0, f.me);
    REQUIRE(f.game.spawnCalls == 1);
    REQUIRE(f.LogHas("boneco id=4 criado"));

    // o jogador local anda: a entidade 4 sai do raio e o boneco é destruído
    f.me = {0, -1, 0};
    f.dir.Update(1.1, f.me);
    REQUIRE(f.game.destroyCalls == 1);
    REQUIRE(f.dir.Stats().alive == 0);
}

TEST_CASE("client: PuppetDirector teto de bonecos fica com os mais perto e troca quando a ordem muda") {
    Fx f(2);
    f.Add(1, 1000);
    f.Add(2, 2000);
    f.Add(3, 3000);
    f.dir.Update(1.0, f.me);
    REQUIRE(f.game.spawnCalls == 2);
    REQUIRE(f.HasPuppetAt(1000));
    REQUIRE(f.HasPuppetAt(2000));
    REQUIRE_FALSE(f.HasPuppetAt(3000));

    f.MoveTo(3, 500, 5.0);
    f.dir.Update(5.2, f.me);
    REQUIRE(f.game.destroyCalls == 1);
    REQUIRE(f.game.spawnCalls == 3);
    REQUIRE(f.game.puppets.size() == 2);
    REQUIRE(f.HasPuppetAt(500));
    REQUIRE(f.HasPuppetAt(1000));
    REQUIRE_FALSE(f.HasPuppetAt(2000));
    REQUIRE(f.dir.Stats().alive == 2);
}

TEST_CASE("client: PuppetDirector teto acima de 64 vira 64") {
    Fx f(100);
    for (uint16_t id = 1; id <= 70; ++id) f.Add(id, 100.0 * id);
    f.dir.Update(1.0, f.me);
    REQUIRE(f.game.spawnCalls == 64);
    REQUIRE(f.dir.Stats().alive == 64);
}

TEST_CASE("client: PuppetDirector maxPuppets 0 nao cria nenhum boneco") {
    Fx f(0);
    f.Add(2, 1000);
    f.Run(0, 3);
    REQUIRE(f.game.spawnCalls == 0);
}

TEST_CASE("client: PuppetDirector SpawnPuppet nulo tenta de novo em 1 s ate 3 vezes") {
    SECTION("3 falhas: a quarta chamada cria") {
        Fx f;
        f.game.spawnFailures = 3;
        f.Add(2, 1000);
        f.Run(0, 0.9);
        REQUIRE(f.game.spawnCalls == 1);
        f.Run(1.0, 2.9);
        REQUIRE(f.game.spawnCalls == 3);
        f.Run(3.0, 3.5);
        REQUIRE(f.game.spawnCalls == 4);
        REQUIRE(f.LogHas("boneco id=2 criado"));
        REQUIRE_FALSE(f.LogHas("desistiu"));
        REQUIRE(f.dir.Stats().alive == 1);
    }
    SECTION("sempre nulo: 4 chamadas em ~3 s e depois desiste") {
        Fx f;
        f.game.spawnFailures = 1000;
        f.Add(2, 1000);
        f.Run(0, 3.5);
        REQUIRE(f.game.spawnCalls == 4);
        REQUIRE(f.LogCount("boneco id=2 desistiu") == 1);
        f.Run(3.6, 20);
        REQUIRE(f.game.spawnCalls == 4);
        REQUIRE(f.dir.Stats().givenUp == 1);
        REQUIRE(f.dir.Stats().alive == 0);
    }
    SECTION("SpawnPuppet que lança conta como falha e erro de jogo") {
        Fx f;
        f.game.throwOnSpawn = true;
        f.Add(2, 1000);
        f.Run(0, 5);
        REQUIRE(f.game.spawnCalls == 4);
        REQUIRE(f.LogHas("boneco id=2 desistiu"));
        REQUIRE(f.dir.Stats().gameErrors == 4);
    }
}

TEST_CASE("client: PuppetDirector Gone recria depois de 1 s") {
    Fx f;
    f.Add(2, 1000);
    f.dir.Update(0.0, f.me);
    REQUIRE(f.game.puppets.size() == 1);
    f.game.gone.insert(f.game.puppets.begin()->first);
    f.dir.Update(1.0, f.me);  // MovePuppet → Gone
    REQUIRE(f.dir.Stats().alive == 0);
    REQUIRE(f.game.destroyCalls == 0);  // o jogo já destruiu o ator
    f.dir.Update(1.5, f.me);
    REQUIRE(f.game.spawnCalls == 1);
    f.dir.Update(2.0, f.me);
    REQUIRE(f.game.spawnCalls == 2);
    REQUIRE(f.LogHas("boneco id=2 recriado"));
    REQUIRE(f.dir.Stats().recreated == 1);
    REQUIRE(f.dir.Stats().alive == 1);
}

TEST_CASE("client: PuppetDirector 6 Gone em 1 minuto desiste da entidade") {
    Fx f;
    f.Add(2, 1000);
    double t = 0;
    f.dir.Update(t, f.me);
    for (int i = 0; i < 6; ++i) {
        REQUIRE(f.game.puppets.size() == 1);
        f.game.gone.insert(f.game.puppets.begin()->first);
        t += 1;
        f.dir.Update(t, f.me);  // Gone
        t += 1;
        f.dir.Update(t, f.me);  // recria (5 primeiras)
        if (i < 5) REQUIRE(f.LogCount("recriado") == i + 1);
        if (i == 5) break;
    }
    REQUIRE(f.game.spawnCalls == 6);
    REQUIRE(f.LogCount("boneco id=2 desistiu") == 1);
    f.Run(t, t + 120, 0.5);
    REQUIRE(f.game.spawnCalls == 6);
    REQUIRE(f.dir.Stats().givenUp == 1);
}

TEST_CASE("client: PuppetDirector recriacoes fora da janela de 1 minuto nao contam") {
    Fx f;
    f.Add(2, 1000);
    double t = 0;
    f.dir.Update(t, f.me);
    for (int i = 0; i < 10; ++i) {  // uma recriação a cada 20 s: nunca 5 no mesmo minuto
        f.game.gone.insert(f.game.puppets.begin()->first);
        t += 1;
        f.dir.Update(t, f.me);
        t += 19;
        f.dir.Update(t, f.me);
    }
    REQUIRE(f.game.spawnCalls == 11);
    REQUIRE_FALSE(f.LogHas("desistiu"));
}

TEST_CASE("client: PuppetDirector 3 GameError seguidos em MovePuppet destroi e recria") {
    Fx f;
    f.Add(2, 1000);
    f.dir.Update(0.0, f.me);
    f.game.throwOnMove = true;
    f.dir.Update(0.1, f.me);
    f.dir.Update(0.2, f.me);
    REQUIRE(f.game.destroyCalls == 0);
    f.dir.Update(0.3, f.me);
    REQUIRE(f.game.destroyCalls == 1);
    REQUIRE(f.dir.Stats().alive == 0);
    REQUIRE(f.dir.Stats().gameErrors == 3);
    f.game.throwOnMove = false;
    f.dir.Update(0.8, f.me);
    REQUIRE(f.game.spawnCalls == 1);
    f.dir.Update(1.3, f.me);
    REQUIRE(f.game.spawnCalls == 2);
    REQUIRE(f.LogHas("boneco id=2 recriado"));

    // erros que não são seguidos não destroem
    f.game.throwOnMove = true;
    f.dir.Update(1.4, f.me);
    f.dir.Update(1.5, f.me);
    f.game.throwOnMove = false;
    f.dir.Update(1.6, f.me);
    f.game.throwOnMove = true;
    f.dir.Update(1.7, f.me);
    f.dir.Update(1.8, f.me);
    REQUIRE(f.game.destroyCalls == 1);
    REQUIRE(f.dir.Stats().alive == 1);
}

TEST_CASE("client: PuppetDirector MovePuppet Failed conta como erro de jogo") {
    Fx f;
    f.Add(2, 1000);
    f.dir.Update(0.0, f.me);
    f.game.failMove.insert(f.game.puppets.begin()->first);
    f.dir.Update(0.1, f.me);
    REQUIRE(f.dir.Stats().gameErrors == 1);
    f.dir.Update(0.2, f.me);
    f.dir.Update(0.3, f.me);
    REQUIRE(f.game.destroyCalls == 1);  // 3 seguidos: trata como Gone
}

TEST_CASE("client: PuppetDirector 20 GameError em 10 s desliga os bonecos") {
    Fx f;
    for (uint16_t id = 1; id <= 5; ++id) f.Add(id, 1000.0 * id);
    f.dir.Update(0.0, f.me);
    REQUIRE(f.dir.Stats().alive == 5);
    f.game.throwOnMove = true;
    f.Run(0.1, 10);
    REQUIRE(f.dir.Stats().disabled);
    REQUIRE(f.dir.Stats().disabledWhy == "erros");
    REQUIRE(f.LogCount("bonecos_desligados: erros") == 1);
    REQUIRE(f.game.puppets.empty());
    REQUIRE(f.dir.Stats().alive == 0);
    const int spawns = f.game.spawnCalls, moves = f.game.moveCalls;
    f.game.throwOnMove = false;
    f.Run(10.1, 20);
    REQUIRE(f.game.spawnCalls == spawns);
    REQUIRE(f.game.moveCalls == moves);
    REQUIRE(f.game.puppets.empty());
}

TEST_CASE("client: PuppetDirector singleton trocado desliga os bonecos") {
    SECTION("no primeiro boneco") {
        Fx f;
        f.game.singletonIntact = false;
        f.Add(2, 1000);
        f.Add(3, 2000);
        f.dir.Update(0.0, f.me);
        REQUIRE(f.game.spawnCalls == 1);
        REQUIRE(f.game.destroyCalls == 1);
        REQUIRE(f.game.singletonCalls == 2);  // confere de novo depois de destruir
        REQUIRE(f.game.puppets.empty());
        REQUIRE(f.dir.Stats().disabled);
        REQUIRE(f.dir.Stats().disabledWhy == "singleton");
        REQUIRE(f.LogCount("bonecos_desligados: singleton") == 1);
        REQUIRE_FALSE(f.LogHas("singleton=ok"));
        f.Run(0.1, 5);
        REQUIRE(f.game.spawnCalls == 1);
    }
    SECTION("depois de outros bonecos: destroi todos") {
        Fx f;
        f.Add(2, 1000);
        f.dir.Update(0.0, f.me);
        REQUIRE(f.LogHas("singleton=ok"));
        f.game.singletonIntact = false;
        f.Add(3, 2000, 0, 0, 0.0);
        f.dir.Update(0.1, f.me);
        REQUIRE(f.game.spawnCalls == 2);
        REQUIRE(f.game.destroyCalls == 2);
        REQUIRE(f.game.puppets.empty());
        REQUIRE(f.dir.Stats().disabledWhy == "singleton");
        REQUIRE(f.dir.Stats().alive == 0);
    }
}

TEST_CASE("client: PuppetDirector conferencia do singleton que lanca falha fechada") {
    Fx f;
    f.game.throwOnSingleton = true;
    f.Add(2, 1000);
    f.Add(3, 2000);
    f.dir.Update(0.0, f.me);
    REQUIRE(f.game.spawnCalls == 1);
    REQUIRE(f.game.destroyCalls == 1);  // o boneco sem conferência sai na hora
    REQUIRE(f.game.puppets.empty());
    REQUIRE(f.dir.Stats().disabled);
    REQUIRE(f.dir.Stats().disabledWhy == "singleton_indisponivel");
    REQUIRE(f.dir.Stats().alive == 0);
    REQUIRE(f.LogCount("bonecos_desligados: singleton_indisponivel") == 1);
    REQUIRE_FALSE(f.LogHas("singleton=ok"));
    f.game.throwOnSingleton = false;
    f.Run(0.1, 5);
    REQUIRE(f.game.spawnCalls == 1);  // nenhum SpawnPuppet depois
}

TEST_CASE("client: PuppetDirector ReleaseAll true esquece todos sem DestroyPuppet") {
    Fx f;
    f.Add(2, 1000);
    f.Add(3, 2000);
    f.dir.Update(0.0, f.me);
    REQUIRE(f.dir.Stats().alive == 2);
    f.dir.ReleaseAll(true);
    REQUIRE(f.game.forgetAllCalls == 1);
    REQUIRE(f.game.destroyCalls == 0);
    REQUIRE(f.dir.Stats().alive == 0);
    f.dir.Update(1.0, f.me);  // de volta ao mundo: recria
    REQUIRE(f.game.spawnCalls == 4);
    REQUIRE(f.game.moveCalls == 0);
}

TEST_CASE("client: PuppetDirector ReleaseAll false destroi cada um e zera as desistencias") {
    Fx f;
    f.Add(2, 1000);
    f.Add(3, 2000);
    f.dir.Update(0.0, f.me);
    f.dir.ReleaseAll(false);
    REQUIRE(f.game.destroyCalls == 2);
    REQUIRE(f.game.forgetAllCalls == 0);
    REQUIRE(f.game.puppets.empty());
    REQUIRE(f.dir.Stats().alive == 0);

    // entidade que desistiu volta a ser tentada depois do ReleaseAll
    f.game.spawnFailures = 1000;
    f.Run(1.0, 6.0);
    REQUIRE(f.LogCount("desistiu") == 2);
    const int calls = f.game.spawnCalls;
    f.dir.ReleaseAll(false);
    f.game.spawnFailures = 0;
    f.dir.Update(7.0, f.me);
    REQUIRE(f.game.spawnCalls == calls + 2);
    REQUIRE(f.dir.Stats().alive == 2);
}

TEST_CASE("client: PuppetDirector OnEntityLeave destroi o boneco") {
    Fx f;
    f.Add(2, 1000);
    f.dir.Update(0.0, f.me);
    REQUIRE(f.store.Leave(2));
    f.dir.OnEntityLeave(2);
    REQUIRE(f.game.destroyCalls == 1);
    REQUIRE(f.LogHas("boneco id=2 destruído"));
    REQUIRE(f.dir.Stats().alive == 0);
    REQUIRE(f.dir.Stats().destroyed == 1);
    f.dir.OnEntityLeave(2);  // de novo: nada
    f.dir.OnEntityLeave(77);
    REQUIRE(f.game.destroyCalls == 1);

    // entidade que sumiu do RemoteStore sem OnEntityLeave: destruída no próximo Update
    f.Add(3, 1000, 0, 0, 1.0);
    f.dir.Update(1.0, f.me);
    REQUIRE(f.dir.Stats().alive == 1);
    f.store.Clear();
    f.dir.Update(1.1, f.me);
    REQUIRE(f.game.destroyCalls == 2);
    REQUIRE(f.dir.Stats().alive == 0);
}

TEST_CASE("client: PuppetDirector nunca passa valor nao finito ao jogo") {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    Fx f;
    f.Add(2, 1000);
    f.dir.Update(nan, f.me);
    f.dir.Update(inf, f.me);
    f.dir.Update(1.0, {nan, 0, 0});
    f.dir.Update(1.0, {0, inf, 0});
    REQUIRE(f.game.spawnCalls == 0);
    f.dir.Update(1.0, f.me);
    REQUIRE(f.game.spawnCalls == 1);
    const int moves = f.game.moveCalls;
    f.dir.Update(nan, f.me);
    f.dir.Update(2.0, {0, 0, nan});
    REQUIRE(f.game.moveCalls == moves);  // não move naquele tick
    REQUIRE(f.game.destroyCalls == 0);   // e não destrói
    REQUIRE(f.dir.Stats().alive == 1);
}
