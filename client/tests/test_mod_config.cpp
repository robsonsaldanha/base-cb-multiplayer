#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/ModConfig.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using namespace cbmp::client;

namespace {
ModConfig Parse(std::string_view text) {
    return ParseModConfig(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

void RequireDefaults(const ModConfig& c) {
    CHECK(c.statusOnScreen);
    CHECK_FALSE(c.extraNeutralization);
    CHECK(c.puppetAnim == PuppetAnim::Velocity);
    CHECK(c.maxPuppets == 32);
}
}  // namespace

TEST_CASE("client: ModConfig padroes para entrada vazia ou invalida") {
    RequireDefaults(Parse(""));
    RequireDefaults(Parse("{"));
    RequireDefaults(Parse("[]"));
    RequireDefaults(Parse("{}"));
    RequireDefaults(Parse("null"));
}

TEST_CASE("client: ModConfig le todos os campos") {
    const auto c = Parse(R"({"statusNaTela":false,"neutralizacaoExtra":true,"animacaoBoneco":"ambulatory","maxBonecos":64})");
    CHECK_FALSE(c.statusOnScreen);
    CHECK(c.extraNeutralization);
    CHECK(c.puppetAnim == PuppetAnim::Ambulatory);
    CHECK(c.maxPuppets == 64);
}

TEST_CASE("client: ModConfig maxBonecos fora da faixa vira 32") {
    CHECK(Parse(R"({"maxBonecos":65})").maxPuppets == 32);
    CHECK(Parse(R"({"maxBonecos":-1})").maxPuppets == 32);
    CHECK(Parse(R"({"maxBonecos":"x"})").maxPuppets == 32);
    CHECK(Parse(R"({"maxBonecos":0})").maxPuppets == 0);
}

TEST_CASE("client: ModConfig animacaoBoneco") {
    CHECK(Parse(R"({"animacaoBoneco":"x"})").puppetAnim == PuppetAnim::Velocity);
    CHECK(Parse(R"({"animacaoBoneco":"velocidade"})").puppetAnim == PuppetAnim::Velocity);
    CHECK(Parse(R"({"animacaoBoneco":"off"})").puppetAnim == PuppetAnim::Off);
    CHECK(Parse(R"({"animacaoBoneco":1})").puppetAnim == PuppetAnim::Velocity);
}

TEST_CASE("client: ModConfig campo invalido nao afeta os outros") {
    const auto c = Parse(R"({"statusNaTela":"nao","neutralizacaoExtra":true,"maxBonecos":65})");
    CHECK(c.statusOnScreen);
    CHECK(c.extraNeutralization);
    CHECK(c.maxPuppets == 32);
}

TEST_CASE("client: ModConfig acima de 4 KiB usa padroes") {
    std::string big = R"({"statusNaTela":false,"x":")";
    big += std::string(5 * 1024, 'a');
    big += R"("})";
    RequireDefaults(Parse(big));
}
