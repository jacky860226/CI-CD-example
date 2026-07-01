#include "ScopeTimer.hpp"

#include <cassert>
#include <ctime>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

struct ProcessMemoryInfo {
  long long resident;   // 實體記憶體 (RSS / WorkingSetSize)
  long long virtualMem; // 虛擬記憶體 (VmSize / PagefileUsage)
};

ProcessMemoryInfo GetProcessMemory(); // 前向宣告

#ifdef _WIN32
#include <psapi.h>
#include <windows.h>

ProcessMemoryInfo GetProcessMemory() {
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return {pmc.WorkingSetSize, pmc.PagefileUsage};
  }
  return {0, 0};
}
#endif

#if defined(__linux__)
ProcessMemoryInfo GetProcessMemory() {
  long long rss = 0, vmsize = 0;
  auto read_mem_info = [](auto &line) {
    std::istringstream iss(line);
    std::string key;
    long long kb;
    iss >> key >> kb;
    return kb * 1024;
  };
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.find("VmRSS:") == 0)
      rss = read_mem_info(line);
    else if (line.find("VmSize:") == 0)
      vmsize = read_mem_info(line);
  }
  return {rss, vmsize};
}
#endif

namespace {
inline double get_cpu_time() {
#ifdef _WIN32
  FILETIME a, b, kernel, user;
  if (GetProcessTimes(GetCurrentProcess(), &a, &b, &kernel, &user)) {
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 1e-7;
  }
  return 0;
#else
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static std::mutex log_mtx;

static std::deque<std::string> global_stk;
static std::mutex global_stk_mtx;
static thread_local std::deque<std::string> tl_stk = [] {
  std::deque<std::string> stk;
  {
    std::lock_guard lock(global_stk_mtx);
    stk = global_stk;
  }
  std::stringstream ss;
  ss << std::this_thread::get_id();
  stk.push_back(std::format("Thread {}", ss.str()));
  return stk;
}();

} // namespace

std::deque<std::string> &ScopeTimerPolicy::GlobalStack::stack() {
  return global_stk;
}
std::mutex *ScopeTimerPolicy::GlobalStack::mutex() { return &global_stk_mtx; }

std::deque<std::string> &ScopeTimerPolicy::ThreadLocalStack::stack() {
  return tl_stk;
}

std::mutex *ScopeTimerPolicy::ThreadLocalStack::mutex() { return nullptr; }

template <typename StackPolicy> struct ScopeTimerBase<StackPolicy>::Internal {
  struct Snapshot {
    std::chrono::steady_clock::time_point wall_time;
    double cpu_time;
    long long VmSize, VmRSS;
  };
  struct Increase {
    double wall_time, cpu_time;
    long long VmSize, VmRSS;
  };
  inline static Snapshot get_snapshot();
  inline static Increase get_increase(Snapshot start, Snapshot end) {
    return Increase{
        std::chrono::duration<double>(end.wall_time - start.wall_time).count(),
        end.cpu_time - start.cpu_time, end.VmSize - start.VmSize,
        end.VmRSS - start.VmRSS};
  }
  inline static std::string get_stacked_name();
  inline static decltype(auto) lock_call(auto &&func) {
    std::unique_lock<std::mutex> stack_lock;
    if (auto mtx = StackPolicy::mutex()) {
      stack_lock = std::unique_lock<std::mutex>(*mtx);
    }
    return func();
  }
  inline static void log(const std::string &msg) {
    std::lock_guard lock(log_mtx);
    std::cout << msg << std::endl;
  }

  Snapshot start = get_snapshot();
  const std::string *name;
};

template <typename StackPolicy>
typename ScopeTimerBase<StackPolicy>::Internal::Snapshot
ScopeTimerBase<StackPolicy>::Internal::get_snapshot() {
  ProcessMemoryInfo pmi = GetProcessMemory();
  return Snapshot{std::chrono::steady_clock::now(), get_cpu_time(),
                  pmi.virtualMem, pmi.resident};
}

template <typename StackPolicy>
std::string ScopeTimerBase<StackPolicy>::Internal::get_stacked_name() {
  auto &stk = StackPolicy::stack();
  std::string ans = {};
  for (size_t i = 0; i < stk.size(); ++i) {
    if (i != 0)
      ans += ':';
    ans += '[' + stk[i] + ']';
  }
  return ans;
}

template <typename StackPolicy>
ScopeTimerBase<StackPolicy>::ScopeTimerBase(const std::string &name)
    : internal(std::make_unique<Internal>()) {
  internal->name = Internal::lock_call([&]() -> auto * {
    auto &stk = StackPolicy::stack();
    auto &res = stk.emplace_back(name);
    std::string msg = "ScopeTimer " + Internal::get_stacked_name() + " start";
    Internal::log(msg);
    return &res;
  });
}

template <typename StackPolicy> ScopeTimerBase<StackPolicy>::~ScopeTimerBase() {
  Internal::lock_call([&]() {
    auto &stk = StackPolicy::stack();
    assert(internal->name == &stk.back());
    auto end = Internal::get_snapshot();
    auto incr = Internal::get_increase(internal->start, end);

    auto sign = [](auto val) { return val < 0 ? '-' : (val > 0 ? '+' : '='); };
    auto to_Mb = [](auto val) { return val / 1024 / 1024; };

    std::string msg = std::format(
        "ScopeTimer {} end - Elapsed time {:.3f}({:.3f}c) sec, VmSize "
        "{}({}{}) "
        "Mb, VmRSS {}({}{}) Mb",
        Internal::get_stacked_name(), incr.wall_time, incr.cpu_time,
        to_Mb(end.VmSize), sign(incr.VmSize), to_Mb(incr.VmSize),
        to_Mb(end.VmRSS), sign(incr.VmRSS), to_Mb(incr.VmRSS));
    Internal::log(msg);
    stk.pop_back();
  });
}

template class ScopeTimerBase<ScopeTimerPolicy::GlobalStack>;
template class ScopeTimerBase<ScopeTimerPolicy::ThreadLocalStack>;
