#pragma once
// cb-mp.json (opcional, em Win64\cbmp\, do jogador; nunca vem do servidor).
#include <cbmp/client/Limits.hpp>
#include <cstdint>
#include <span>

namespace cbmp::client {
enum class PuppetAnim { Velocity, Ambulatory, Off };  // "velocidade" | "ambulatory" | "off"

struct ModConfig {
    bool statusOnScreen = true;               // statusNaTela
    bool extraNeutralization = false;         // neutralizacaoExtra
    PuppetAnim puppetAnim = PuppetAnim::Velocity;  // animacaoBoneco
    int maxPuppets = limits::kDefaultMaxPuppets;  // maxBonecos (0..kHardMaxPuppets)
};

// JSON inválido, que não é objeto ou com mais de 4 KiB → padrões. Campo ausente ou inválido → padrão daquele campo.
ModConfig ParseModConfig(std::span<const uint8_t> bytes);
}
