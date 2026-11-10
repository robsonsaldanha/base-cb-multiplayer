#pragma once
// Utilitários comuns dos testes do cbscript.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cbscript/resource.hpp"

// Funções C++ chamadas de quadros que o Lua atravessa com longjmp ficam fora de linha (ver resource.cpp).
#if defined(_MSC_VER)
#define CBSCRIPT_TEST_NOINLINE __declspec(noinline)
#else
#define CBSCRIPT_TEST_NOINLINE __attribute__((noinline))
#endif

namespace fs = std::filesystem;
using cbscript::Limits;
using cbscript::Resource;
using cbscript::ResourceManifest;
using namespace std::chrono_literals;

namespace cbtest {

inline void put(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

struct TmpDir {
  fs::path p;
  TmpDir() {
    static std::atomic<unsigned> n{0};
    std::random_device rd;
    p = fs::temp_directory_path() / ("cbscript_" + std::to_string(rd()) + "_" + std::to_string(n++));
    fs::create_directories(p);
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

inline Limits shortLimits(size_t mem = size_t{64} << 20) {
  Limits l;
  l.memoryBytes = mem;
  l.warnAfter = 20ms;
  l.abortAfter = 200ms;
  return l;
}

struct Fixture {
  TmpDir t;
  std::vector<std::string> warns;
  std::unique_ptr<Resource> r;
  explicit Fixture(Limits lim = shortLimits(), std::string name = "teste") {
    ResourceManifest m;
    m.name = std::move(name);
    m.version = "1.0.0";
    r = std::make_unique<Resource>(m, t.p, lim, [this](std::string s) { warns.push_back(std::move(s)); });
  }
  ~Fixture() { r.reset(); }
  // Compila `code` no ambiente sandbox (pelo host) e roda com call().
  Resource::CallResult run(const std::string& code) {
    sol::load_result lr = r->lua().load(code, "=teste", sol::load_mode::text);
    REQUIRE(lr.valid());
    sol::protected_function fn = lr;
    return r->call(fn);
  }
  bool isTrue(const std::string& code) {
    auto res = run(code);
    INFO(res.error);
    REQUIRE(res.ok);
    return res.value.is<bool>() && res.value.as<bool>();
  }
};

inline bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

}  // namespace cbtest

namespace cbtest {

// Roda f numa thread separada com prazo rígido: se não voltar em `deadline`, o processo sai com falha (sem pendurar
// o CI). f não pode usar REQUIRE (asserções do Catch2 não são thread-safe): devolva dados e verifique na thread do teste.
template <class F>
auto guarded(const char* what, F f, std::chrono::seconds deadline = std::chrono::seconds(10)) -> decltype(f()) {
  using R = decltype(f());
  auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
  auto fut = task->get_future();
  std::thread([task] { (*task)(); }).detach();
  if (fut.wait_for(deadline) != std::future_status::ready) {
    std::fprintf(stderr, "FALHA: travou por mais de %lld s: %s\n", static_cast<long long>(deadline.count()), what);
    std::fflush(stderr);
    std::quick_exit(3);
  }
  return fut.get();
}

// CPU (usuário + kernel) gasto pela thread atual, em ms (tests/test_cpu.cpp).
double threadCpuMs();

struct Timed {
  Resource::CallResult res;
  long long ms = 0;     // parede
  double cpuMs = 0;     // CPU da thread que rodou o Lua (a de guarded())
};

// Compila e roda sem REQUIRE (para uso dentro de guarded()).
inline Timed runRaw(Resource& r, const std::string& code) {
  Timed t;
  sol::load_result lr = r.lua().load(code, "=teste", sol::load_mode::text);
  if (!lr.valid()) {
    t.res.error = "LOADERR";
    return t;
  }
  sol::protected_function fn = lr;
  const double c0 = threadCpuMs();
  auto t0 = std::chrono::steady_clock::now();
  t.res = r.call(fn);
  t.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  t.cpuMs = threadCpuMs() - c0;
  return t;
}

}  // namespace cbtest

using namespace cbtest;
