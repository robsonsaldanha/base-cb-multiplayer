// Placeholder: a Task 6 escreve o laço real. Por ora só carrega e mostra a configuração.
#include <cstdio>
#include <exception>

#include "cbserver/config.hpp"

int main(int argc, char** argv) {
  try {
    auto cfg = cbserver::ServerConfig::load(argc > 1 ? argv[1] : "server.cfg");
    std::printf("cb-server: '%s' porta %d, até %d jogadores\n", cfg.name.c_str(), cfg.port, cfg.maxPlayers);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
