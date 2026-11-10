// Tempo de CPU da thread atual (para os testes de regressão da vigia): mede o trabalho feito, não o tempo de parede,
// então não infla com máquina carregada (ctest -j16) e continua pegando uma operação C cara sem checagem.
#include "test_util.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace cbtest {

double threadCpuMs() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  auto ticks = [](const FILETIME& f) {
    return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;  // unidades de 100 ns
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
#endif
}

}  // namespace cbtest
