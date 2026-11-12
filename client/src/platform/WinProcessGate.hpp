#pragma once
// Dois processos na abertura (spec): época = contador de 8 bytes numa seção nomeada (InterlockedIncrement64 ao
// iniciar); dono = mutex nomeado tomado sem esperar. Handles mantidos até o destrutor; a seção some quando o último
// processo sai (reinício sem sobreposição recomeça em 1). Nada de segredo em nenhum dos dois objetos.
// Win32, sem UE4SS; não inclui Windows.h no cabeçalho.
#include <cbmp/client/ProcessGate.hpp>
#include <cstdint>
#include <string>

namespace cbmp::client {
class WinProcessGate final : public IProcessGate {
public:
    static constexpr const wchar_t* kEpochName = L"Local\\cbmp-mod-epoch-v1";
    static constexpr const wchar_t* kOwnerName = L"Local\\cbmp-mod-owner-v1";

    WinProcessGate();                                                // nomes da spec
    WinProcessGate(std::wstring epochName, std::wstring ownerName);  // testes
    ~WinProcessGate() override;
    WinProcessGate(const WinProcessGate&) = delete;
    WinProcessGate& operator=(const WinProcessGate&) = delete;

    // Seção, visão e mutex criados. Sem a seção: Epoch() = Latest() = 1 (como se estivesse sozinho);
    // sem o mutex: TryOwn() é sempre falso (nunca resgata sem ser dono).
    bool Ok() const;

    uint64_t Epoch() const override;
    uint64_t Latest() override;
    bool IsCurrent() override;
    bool TryOwn() override;

private:
    void* mapping_ = nullptr;           // HANDLE
    volatile int64_t* counter_ = nullptr;  // visão de 8 bytes
    void* mutex_ = nullptr;             // HANDLE
    uint64_t epoch_ = 1;
    bool owned_ = false;
    unsigned long ownerThread_ = 0;     // o mutex só pode ser solto pela thread que o tomou
};
}  // namespace cbmp::client
