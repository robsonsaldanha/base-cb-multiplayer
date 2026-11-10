// cb-bots: N jogadores simulados contra um cb-server em modo dev (tickets assinados com a seed dev).
#include <cstdio>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

#include "bot.hpp"

namespace {

constexpr const char* kUsage =
    "uso: cb-bots --server HOST:PORT --dev-seed <hex32> --server-id <id> --count N --area <m> --cluster <0..1>\n"
    "             --duration <s> --report <arquivo> [--teleport-every <s>] [--flood] [--ramp <por s>]\n"
    "     cb-bots --dev-seed <hex32> --print-dev-key   (chave pública base64url para auth.devPublicKey)\n";

bool parseSeed(const std::string& hex, std::array<uint8_t, 32>& out) {
  if (hex.size() != 64) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < 32; ++i) {
    const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi * 16 + lo);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  timeBeginPeriod(1);  // sleep de 1 ms de verdade (padrão do Windows é ~15,6 ms)
#endif
  cbbots::BotOptions opt;
  std::string reportPath, seedHex;
  bool printKey = false;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto val = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error("falta valor para " + a);
        return argv[++i];
      };
      if (a == "--server") {
        const std::string v = val();
        const auto colon = v.rfind(':');
        if (colon == std::string::npos) throw std::runtime_error("--server deve ser HOST:PORT");
        opt.host = v.substr(0, colon);
        opt.port = static_cast<uint16_t>(std::stoi(v.substr(colon + 1)));
      } else if (a == "--dev-seed") {
        seedHex = val();
      } else if (a == "--server-id") {
        opt.serverId = val();
      } else if (a == "--count") {
        opt.count = std::stoi(val());
      } else if (a == "--area") {
        opt.areaM = std::stod(val());
      } else if (a == "--cluster") {
        opt.cluster = std::stod(val());
      } else if (a == "--duration") {
        opt.durationS = std::stod(val());
      } else if (a == "--report") {
        reportPath = val();
      } else if (a == "--teleport-every") {
        opt.teleportEvery = std::stod(val());
      } else if (a == "--flood") {
        opt.flood = true;
      } else if (a == "--ramp") {
        opt.rampPerSec = std::stod(val());
      } else if (a == "--print-dev-key") {
        printKey = true;
      } else if (a == "--help" || a == "-h") {
        std::printf("%s", kUsage);
        return 0;
      } else {
        throw std::runtime_error("argumento desconhecido: " + a);
      }
    }
    if (!parseSeed(seedHex, opt.seed)) throw std::runtime_error("--dev-seed deve ter 64 dígitos hex");
    if (printKey) {
      std::printf("%s\n", cbbots::devPublicKeyB64(opt.seed).c_str());
      return 0;
    }
    if (opt.count < 1 || opt.count > 4096) throw std::runtime_error("--count deve estar entre 1 e 4096");
    if (opt.durationS <= 0) throw std::runtime_error("--duration deve ser > 0");

    const auto report = cbbots::runBots(opt);
    const std::string text = report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    if (!reportPath.empty()) {
      std::ofstream f(reportPath, std::ios::binary);
      f << text << "\n";
      if (!f) throw std::runtime_error("não foi possível gravar " + reportPath);
    }
    std::printf("%s\n", text.c_str());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "erro: %s\n%s", e.what(), kUsage);
    return 2;
  }
}
