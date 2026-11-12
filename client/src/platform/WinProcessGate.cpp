#include "WinProcessGate.hpp"
#include <windows.h>
#include <utility>

namespace cbmp::client {
namespace {
volatile LONG64* AsLong64(volatile int64_t* p) { return reinterpret_cast<volatile LONG64*>(p); }
}  // namespace

WinProcessGate::WinProcessGate() : WinProcessGate(kEpochName, kOwnerName) {}

WinProcessGate::WinProcessGate(std::wstring epochName, std::wstring ownerName) {
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 8, epochName.c_str());
    if (mapping_) {
        if (void* view = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 8)) {
            counter_ = static_cast<volatile int64_t*>(view);  // visão alinhada à página: 8 bytes alinhados
            epoch_ = static_cast<uint64_t>(InterlockedIncrement64(AsLong64(counter_)));
        }
    }
    mutex_ = CreateMutexW(nullptr, FALSE, ownerName.c_str());
}

WinProcessGate::~WinProcessGate() {
    if (mutex_) {
        // ReleaseMutex só vale na thread dona; em outra thread o mutex fica abandonado e some com o processo.
        if (owned_ && GetCurrentThreadId() == ownerThread_) ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }
    if (counter_) UnmapViewOfFile(const_cast<int64_t*>(counter_));
    if (mapping_) CloseHandle(mapping_);
}

bool WinProcessGate::Ok() const { return mapping_ && counter_ && mutex_; }

uint64_t WinProcessGate::Epoch() const { return epoch_; }

uint64_t WinProcessGate::Latest() {
    if (!counter_) return epoch_;
    return static_cast<uint64_t>(InterlockedCompareExchange64(AsLong64(counter_), 0, 0));
}

bool WinProcessGate::IsCurrent() { return Latest() == epoch_; }

bool WinProcessGate::TryOwn() {
    if (owned_) return true;
    if (!mutex_) return false;
    const DWORD r = WaitForSingleObject(mutex_, 0);
    if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) return false;  // abandonado: o dono anterior morreu, agora é meu
    owned_ = true;
    ownerThread_ = GetCurrentThreadId();
    return true;
}
}  // namespace cbmp::client
