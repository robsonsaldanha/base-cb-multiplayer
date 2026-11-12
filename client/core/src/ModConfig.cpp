#include <cbmp/client/ModConfig.hpp>
#include <cbmp/client/Limits.hpp>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cbmp::client {
ModConfig ParseModConfig(std::span<const uint8_t> bytes) {
    ModConfig cfg;
    if (bytes.size() > limits::kMaxModConfigBytes) return cfg;
    const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return cfg;

    if (const auto it = j.find("statusNaTela"); it != j.end() && it->is_boolean()) cfg.statusOnScreen = it->get<bool>();
    if (const auto it = j.find("neutralizacaoExtra"); it != j.end() && it->is_boolean()) {
        cfg.extraNeutralization = it->get<bool>();
    }
    if (const auto it = j.find("animacaoBoneco"); it != j.end() && it->is_string()) {
        const auto& s = it->get_ref<const std::string&>();
        if (s == "velocidade") cfg.puppetAnim = PuppetAnim::Velocity;
        else if (s == "ambulatory") cfg.puppetAnim = PuppetAnim::Ambulatory;
        else if (s == "off") cfg.puppetAnim = PuppetAnim::Off;
    }
    if (const auto it = j.find("maxBonecos"); it != j.end()) {
        // Não negativos chegam como sem sinal; negativos (sempre fora da faixa) como com sinal. 32.0 não é inteiro.
        if (it->is_number_unsigned()) {
            const auto v = it->get<uint64_t>();
            if (v <= static_cast<uint64_t>(limits::kHardMaxPuppets)) cfg.maxPuppets = static_cast<int>(v);
        }
    }
    return cfg;
}
}
