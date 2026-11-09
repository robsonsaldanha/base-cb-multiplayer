#pragma once
#include <cbmp/IGame.hpp>
#include <string>

// Requer /EHa: catch(...) também pega exceções SEH (access violation etc.).
template <class F>
auto SafeCall(const char* what, F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const cbmp::GameError&) {
        throw;
    } catch (const std::exception& e) {
        throw cbmp::GameError(std::string(what) + ": " + e.what());
    } catch (...) {
        throw cbmp::GameError(std::string(what) + ": exceção SEH (provável ponteiro inválido)");
    }
}
