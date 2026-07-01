#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace ScopeTimerPolicy {
struct GlobalStack {
  static std::deque<std::string> &stack();
  static std::mutex *mutex();
};

struct ThreadLocalStack {
  static std::deque<std::string> &stack();
  static std::mutex *mutex();
};
} // namespace ScopeTimerPolicy

template <typename StackPolicy> class ScopeTimerBase {
  struct Internal;
  std::unique_ptr<Internal> internal;

public:
  explicit ScopeTimerBase(const std::string &name);
  ~ScopeTimerBase();

  ScopeTimerBase(const ScopeTimerBase &) = delete;
  ScopeTimerBase &operator=(const ScopeTimerBase &) = delete;
  ScopeTimerBase(ScopeTimerBase &&) = delete;
  ScopeTimerBase &operator=(ScopeTimerBase &&) = delete;
};

using ScopeTimer = ScopeTimerBase<ScopeTimerPolicy::GlobalStack>;

using ThreadLocalScopeTimer =
    ScopeTimerBase<ScopeTimerPolicy::ThreadLocalStack>;
