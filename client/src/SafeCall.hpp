#pragma once
#include <cbmp/client/IGame.hpp>
#include <string>

namespace cbmp::client {
// Roda f() e transforma qualquer falha em GameError("<what>: …"). Requer /EHa: catch(...) também pega exceções SEH
// (access violation etc. dentro do motor). GameError passa como veio.
template <class F>
auto SafeCall(const char* what, F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const GameError&) {
        throw;
    } catch (const std::exception& e) {
        throw GameError(std::string(what) + ": " + e.what());
    } catch (...) {
        throw GameError(std::string(what) + ": exceção SEH (provável ponteiro inválido)");
    }
}
}  // namespace cbmp::client
