#pragma once
// Inércia sem o launcher (spec, "Inércia e o ponteiro do launcher"): o mod só arma se acha um cbmp-run.json válido e
// recente subindo a partir da pasta do main.dll e se o connect.json que ele aponta existe. Roda na thread do UE4SS,
// só lê: não cria, não abre para escrita e não apaga nada. Win32, sem UE4SS; não inclui Windows.h no cabeçalho.
#include <cbmp/client/ConnectFile.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cbmp::client {
struct Armed {
    std::filesystem::path container;  // Win64\cbmp (pasta do cbmp-run.json)
    RunPointer pointer;
    std::filesystem::path connectJson;  // <runDir>\connect.json
};

// FindPointerFile(dllDir) → lê (≤ 4 KiB) → ParsePointer(agora) → connect.json existe (GetFileAttributesW, sem abrir).
// Qualquer falha → nullopt (Inerte) e, se `why` não for nulo, um motivo curto (só para testes e diagnóstico).
std::optional<Armed> TryArm(const std::filesystem::path& dllDir, int64_t nowUnix, std::string* why = nullptr);

// Conteúdo de um arquivo comum com no máximo `maxBytes` (abre só para leitura, compartilhando leitura, escrita e
// remoção). Ausente, pasta, maior que `maxBytes` ou erro de leitura → nullopt.
std::optional<std::vector<uint8_t>> ReadSmallFile(const std::filesystem::path& file, size_t maxBytes);
}  // namespace cbmp::client
