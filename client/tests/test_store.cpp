#include "TestFramework.hpp"
#include <cbmp/Store.hpp>

using namespace cbmp;

TEST(detecta_steam) {
    CHECK(DetectStore(L"D:\\SteamLibrary\\steamapps\\common\\Hogwarts Legacy\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") == Store::Steam);
}
TEST(detecta_epic) {
    CHECK(DetectStore(L"C:\\Program Files\\Epic Games\\HogwartsLegacy\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") == Store::Epic);
}
TEST(detecta_desconhecida) {
    CHECK(DetectStore(L"E:\\Jogos\\HL\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") == Store::Unknown);
}
TEST(nome_da_loja) {
    CHECK_EQ(std::string(StoreName(Store::Steam)), std::string("Steam"));
    CHECK_EQ(std::string(StoreName(Store::Epic)), std::string("Epic"));
    CHECK_EQ(std::string(StoreName(Store::Unknown)), std::string("desconhecida"));
}
