#include "cbserver/pool.hpp"

#include <algorithm>

namespace cbserver {

TaskPool::TaskPool(unsigned threads) : workers_(std::max(1u, threads)) {
  threads_.reserve(workers_ - 1);
  for (unsigned w = 1; w < workers_; ++w) threads_.emplace_back([this, w] { loop(w); });
}

TaskPool::~TaskPool() {
  {
    std::lock_guard lk(mu_);
    stop_ = true;
  }
  wake_.notify_all();
  for (auto& t : threads_) t.join();
}

unsigned TaskPool::defaultThreads() {
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 1 ? hw - 1 : 1;
}

size_t TaskPool::runBlocks(unsigned worker) {
  size_t count = 0;
  for (size_t b = next_.fetch_add(1); b < blocks_; b = next_.fetch_add(1)) {
    const size_t begin = b * blockSize_;
    const size_t end = std::min(n_, begin + blockSize_);
    try {
      (*job_)(begin, end, worker);
    } catch (...) {
      std::lock_guard lk(mu_);
      if (!error_) error_ = std::current_exception();
    }
    ++count;
  }
  return count;
}

void TaskPool::loop(unsigned worker) {
  uint64_t seen = 0;
  std::unique_lock lk(mu_);
  for (;;) {
    wake_.wait(lk, [&] { return stop_ || (job_ != nullptr && gen_ != seen); });
    if (stop_) return;
    seen = gen_;
    ++active_;
    lk.unlock();
    const size_t did = runBlocks(worker);
    lk.lock();
    --active_;
    doneBlocks_ += did;
    if (doneBlocks_ == blocks_ && active_ == 0) done_.notify_all();
  }
}

void TaskPool::parallelFor(size_t n, const Fn& fn) {
  if (n == 0) return;
  if (workers_ == 1) {
    fn(0, n, 0);
    return;
  }
  {
    std::lock_guard lk(mu_);
    job_ = &fn;
    n_ = n;
    // Até 4 blocos por worker: equilibra carga sem custo de sincronização relevante.
    blocks_ = std::min<size_t>(n, static_cast<size_t>(workers_) * 4);
    blockSize_ = (n + blocks_ - 1) / blocks_;
    blocks_ = (n + blockSize_ - 1) / blockSize_;
    doneBlocks_ = 0;
    error_ = nullptr;
    next_.store(0);
    ++gen_;
  }
  wake_.notify_all();
  const size_t did = runBlocks(0);
  std::exception_ptr err;
  {
    std::unique_lock lk(mu_);
    doneBlocks_ += did;
    done_.wait(lk, [&] { return doneBlocks_ == blocks_ && active_ == 0; });
    job_ = nullptr;
    err = error_;
    error_ = nullptr;
  }
  if (err) std::rethrow_exception(err);
}

}  // namespace cbserver
