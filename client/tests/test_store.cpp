#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/Store.hpp>
#include <string>

using namespace cbmp::client;

TEST_CASE("client: DetectStore reconhece a Steam") {
    REQUIRE(DetectStore(L"D:\\SteamLibrary\\steamapps\\common\\Hogwarts Legacy\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") ==
            Store::Steam);
}

TEST_CASE("client: DetectStore reconhece a Epic") {
    REQUIRE(DetectStore(L"C:\\Program Files\\Epic Games\\HogwartsLegacy\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") ==
            Store::Epic);
}

TEST_CASE("client: DetectStore sem loja conhecida") {
    REQUIRE(DetectStore(L"E:\\Jogos\\HL\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") == Store::Unknown);
}

TEST_CASE("client: StoreName") {
    CHECK(std::string(StoreName(Store::Steam)) == "Steam");
    CHECK(std::string(StoreName(Store::Epic)) == "Epic");
    CHECK(std::string(StoreName(Store::Unknown)) == "desconhecida");
}
