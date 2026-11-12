#pragma once
// Dois processos na abertura do jogo: contador de época em memória compartilhada + mutex nomeado de dono.
// Implementação Win32 em client/src/platform (WinProcessGate); falsa nos testes (FakeGate).
#include <cstdint>

namespace cbmp::client {
class IProcessGate {
public:
    virtual ~IProcessGate() = default;
    virtual uint64_t Epoch() const = 0;  // valor guardado ao iniciar (≥ 1)
    virtual uint64_t Latest() = 0;       // contador atual
    virtual bool IsCurrent() = 0;        // Latest() == Epoch()
    virtual bool TryOwn() = 0;           // toma o mutex sem esperar; true também se já é dono
};
}  // namespace cbmp::client
