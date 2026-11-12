// Peças puras da camada do jogo (client/src/game usa; aqui sem UE4SS): conversões, alternativas fixadas, tabela de
// bonecos e o "registrar uma vez".
#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/GameGlue.hpp>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cbmp::client;

TEST_CASE("client: MovementMode do CharacterMovement vira MoveMode") {
    CHECK(MoveModeFromMovementMode(0) == MoveMode::Other);     // MOVE_None
    CHECK(MoveModeFromMovementMode(1) == MoveMode::Ground);    // MOVE_Walking
    CHECK(MoveModeFromMovementMode(2) == MoveMode::Ground);    // MOVE_NavWalking
    CHECK(MoveModeFromMovementMode(3) == MoveMode::Falling);
    CHECK(MoveModeFromMovementMode(4) == MoveMode::Swimming);
    CHECK(MoveModeFromMovementMode(5) == MoveMode::Flying);
    CHECK(MoveModeFromMovementMode(6) == MoveMode::Other);     // MOVE_Custom
    CHECK(MoveModeFromMovementMode(255) == MoveMode::Other);
}

TEST_CASE("client: ObjectKey junta indice e serie; serie 0 ou indice negativo = nenhum") {
    CHECK(ObjectKey(5, 0) == 0);
    CHECK(ObjectKey(-1, 7) == 0);
    CHECK(ObjectKey(0, 1) != 0);
    CHECK(ObjectKey(5, 7) == ObjectKey(5, 7));
    CHECK(ObjectKey(5, 7) != ObjectKey(5, 8));   // mesmo índice reaproveitado por outro objeto
    CHECK(ObjectKey(5, 7) != ObjectKey(6, 7));
    CHECK(ObjectKey(INT32_MAX, INT32_MAX) != 0);
    CHECK(ObjectKey(1, -3) != 0);               // série é só um número; negativo não acontece, mas não colide com 0
}

TEST_CASE("client: Utf8ToUtf16 converte acentos, emoji e trata bytes invalidos") {
    CHECK(Utf8ToUtf16("") == u"");
    CHECK(Utf8ToUtf16("CB Multiplayer: conectado (2 por perto)") == u"CB Multiplayer: conectado (2 por perto)");
    CHECK(Utf8ToUtf16("desconectado \xE2\x80\x94 conex\xC3\xA3o perdida") == u"desconectado — conexão perdida");
    CHECK(Utf8ToUtf16("\xF0\x9F\x98\x80") == u"\U0001F600");   // fora do BMP → par substituto
    CHECK(Utf8ToUtf16("a\xFF" "b") == u"a�b");                // byte inválido
    CHECK(Utf8ToUtf16("a\xC3") == u"a�");                     // sequência cortada no fim
    CHECK(Utf8ToUtf16("\xC0\xAF") == u"��");             // forma longa demais
    CHECK(Utf8ToUtf16("\xED\xA0\x80") == u"���");   // substituto codificado (proibido)
    CHECK(Utf8ToUtf16("\xF4\x90\x80\x80") == u"����");  // acima de U+10FFFF
    CHECK(Utf8ToUtf16(std::string("a\0b", 3)) == std::u16string(u"a\0b", 3));
}

TEST_CASE("client: FallbackLadder fixa a primeira alternativa que existe") {
    FallbackLadder ladder(3);
    std::vector<size_t> tried;
    auto r = ladder.Run([&](size_t i) {
        tried.push_back(i);
        return i == 1 ? Attempt::Done : Attempt::Absent;
    });
    REQUIRE(r.has_value());
    CHECK(*r == 1);
    CHECK(tried == std::vector<size_t>{0, 1});
    CHECK(ladder.Pinned() == std::optional<size_t>{1});

    // Daí em diante só a fixada é tentada.
    tried.clear();
    r = ladder.Run([&](size_t i) {
        tried.push_back(i);
        return Attempt::Done;
    });
    CHECK(r == std::optional<size_t>{1});
    CHECK(tried == std::vector<size_t>{1});
}

TEST_CASE("client: FallbackLadder sem nenhuma alternativa fica esgotada e nao tenta de novo") {
    FallbackLadder ladder(2);
    int calls = 0;
    auto none = [&](size_t) {
        ++calls;
        return Attempt::Absent;
    };
    CHECK_FALSE(ladder.Run(none).has_value());
    CHECK(calls == 2);
    CHECK(ladder.Exhausted());
    CHECK_FALSE(ladder.Run(none).has_value());
    CHECK(calls == 2);
}

TEST_CASE("client: FallbackLadder deixa a excecao do passo passar sem fixar") {
    FallbackLadder ladder(2);
    CHECK_THROWS_AS(ladder.Run([](size_t i) -> Attempt {
        if (i == 0) throw std::runtime_error("ator morreu");
        return Attempt::Done;
    }),
                    std::runtime_error);
    CHECK_FALSE(ladder.Pinned().has_value());
    CHECK_FALSE(ladder.Exhausted());
    // Na próxima, a mesma ordem.
    std::vector<size_t> tried;
    auto r = ladder.Run([&](size_t i) {
        tried.push_back(i);
        return Attempt::Done;
    });
    CHECK(r == std::optional<size_t>{0});
    CHECK(tried == std::vector<size_t>{0});
}

TEST_CASE("client: FallbackLadder fixada que some devolve nada e continua fixada") {
    FallbackLadder ladder(2);
    (void)ladder.Run([](size_t i) { return i == 0 ? Attempt::Done : Attempt::Absent; });
    CHECK_FALSE(ladder.Run([](size_t) { return Attempt::Absent; }).has_value());
    CHECK(ladder.Pinned() == std::optional<size_t>{0});
}

TEST_CASE("client: PuppetTable da ids a partir de 1 e nunca reutiliza") {
    PuppetTable<std::string> table;
    const auto a = table.Add("a");
    const auto b = table.Add("b");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->v == 1);
    CHECK(b->v == 2);
    CHECK(table.Size() == 2);
    REQUIRE(table.Find(*a) != nullptr);
    CHECK(*table.Find(*a) == "a");

    CHECK(table.Remove(*a));
    CHECK_FALSE(table.Remove(*a));
    CHECK(table.Find(*a) == nullptr);
    const auto c = table.Add("c");
    REQUIRE(c.has_value());
    CHECK(c->v == 3);

    table.Clear();
    CHECK(table.Size() == 0);
    CHECK(table.Find(*b) == nullptr);
    const auto d = table.Add("d");
    REQUIRE(d.has_value());
    CHECK(d->v == 4);   // Clear (mundo trocou) não reinicia a contagem
    CHECK(table.Find(PuppetId{0}) == nullptr);
}

TEST_CASE("client: OnceSet diz sim so na primeira vez de cada chave") {
    OnceSet once;
    CHECK(once.First("K2_TeleportTo"));
    CHECK_FALSE(once.First("K2_TeleportTo"));
    CHECK(once.First("SetInvulnerable"));
    CHECK_FALSE(once.First("SetInvulnerable"));
}
