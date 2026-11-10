#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cbserver {

// Pool fixo de threads. `workers()` = max(1, threads); a thread chamadora é o worker 0 e as demais
// (workers()-1) ficam dormindo entre chamadas. parallelFor divide [0,n) em blocos contíguos, cada um
// executado por exatamente um worker; bloqueia até terminar e repassa a primeira exceção.
// Não é reentrante: só uma thread chama parallelFor por vez.
class TaskPool {
 public:
  using Fn = std::function<void(size_t begin, size_t end, unsigned worker)>;

  explicit TaskPool(unsigned threads);
  ~TaskPool();
  TaskPool(const TaskPool&) = delete;
  TaskPool& operator=(const TaskPool&) = delete;

  void parallelFor(size_t n, const Fn& fn);
  unsigned workers() const { return workers_; }

  // max(1, núcleos - 1), conforme a spec.
  static unsigned defaultThreads();

 private:
  void loop(unsigned worker);
  size_t runBlocks(unsigned worker);

  unsigned workers_;
  std::vector<std::thread> threads_;
  std::mutex mu_;
  std::condition_variable wake_, done_;
  const Fn* job_ = nullptr;
  size_t n_ = 0, blocks_ = 0, blockSize_ = 0, doneBlocks_ = 0;
  unsigned active_ = 0;
  uint64_t gen_ = 0;
  bool stop_ = false;
  std::atomic<size_t> next_{0};
  std::exception_ptr error_;
};

}  // namespace cbserver
