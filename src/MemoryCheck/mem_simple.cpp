/**
 * 進階版 Malloc Hook 教學範例 — 加入記憶體紀錄追蹤
 *   - 每次 malloc/new 時記錄 (ptr, size, time)
 *   - 每次 free/delete 時移除紀錄
 *   - 程式結束前 Dump 出所有「尚未釋放」的記憶體 → 找出 Memory Leak
 *
 * 新概念：
 *   1. unordered_set 做 O(1) 查找
 *   2. mutex 保護多執行緒存取
 *   3. Snapshot 策略：Dump 時先複製再輸出，避免持鎖做 I/O
 *   4. g_records 用指標，手動控制生命週期（避免 exit 階段 crash）
 *
 * 編譯：g++ -std=c++17 -o test_records test_records.cpp -ldl -pthread
 * 執行：./test_records
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

// ============================================================
// 第一部分：原始函式指標
// ============================================================
using MallocFunc = void *(size_t);
using FreeFunc = void(void *);
using CallocFunc = void *(size_t, size_t);
using ReallocFunc = void *(void *, size_t);

static MallocFunc *real_malloc = nullptr;
static FreeFunc *real_free = nullptr;
static CallocFunc *real_calloc = nullptr;
static ReallocFunc *real_realloc = nullptr;

static bool g_initialized = false;
static bool g_initializing = false;

// ============================================================
// 第二部分：靜態緩衝區
// ============================================================
static char g_static_buf[1024 * 1024]; // 1 MB
static char *g_static_ptr = g_static_buf;

static bool IsStaticMemory(void *ptr) {
  return ptr >= g_static_buf && ptr < g_static_buf + sizeof(g_static_buf);
}

static void *StaticAlloc(size_t size) {
  size_t aligned = (size + 15) & ~15UL;
  if (g_static_ptr + aligned > g_static_buf + sizeof(g_static_buf)) {
    const char *msg = "*** static buffer exhausted ***\n";
    write(STDERR_FILENO, msg, strlen(msg));
    _exit(1);
  }
  void *ptr = g_static_ptr;
  g_static_ptr += aligned;
  return ptr;
}

// ============================================================
// 第三部分：遞迴防護
// ============================================================
static thread_local bool g_inside_hook = false;
struct HookGuard {
  bool prev_;
  HookGuard() : prev_(g_inside_hook) { g_inside_hook = true; }
  ~HookGuard() { g_inside_hook = prev_; }
};

// ============================================================
// 第四部分：記憶體紀錄 (MemoryRecords)
// ============================================================

/// 單筆配置紀錄：指標、大小、時間
struct MemoryRec {
  void *ptr_;
  size_t size_;
  std::chrono::system_clock::time_point time_;

  MemoryRec(void *p, size_t s)
      : ptr_(p), size_(s), time_(std::chrono::system_clock::now()) {}
  explicit MemoryRec(void *p) : ptr_(p), size_(0) {} // 查詢用
};

/// Hash / Equal 仿函式：以 ptr_ 做 key
struct MemoryRecOp {
  size_t operator()(const MemoryRec *r) const {
    return std::hash<void *>()(r->ptr_);
  }
  bool operator()(const MemoryRec *a, const MemoryRec *b) const {
    return a->ptr_ == b->ptr_;
  }
};

/**
 * 管理所有配置紀錄。
 *
 * 設計重點：
 *   - Add/Remove 都在 mutex 保護下操作 unordered_set
 *   - Dump 採 Snapshot 策略：持鎖期間只複製，釋鎖後再做 I/O
 *     → 避免 fprintf 觸發 malloc → 再拿鎖 → Deadlock
 */
class MemoryRecords {
  std::unordered_set<MemoryRec *, MemoryRecOp, MemoryRecOp> records_;
  std::mutex lock_;

public:
  void Add(void *ptr, size_t size) {
    std::lock_guard<std::mutex> g(lock_);
    records_.insert(new MemoryRec(ptr, size));
  }
  void Remove(void *ptr) {
    std::lock_guard<std::mutex> g(lock_);
    MemoryRec key(ptr);
    if (auto it = records_.find(&key); it != records_.end()) {
      delete *it; records_.erase(it);
    }
  }
  /// Dump 尚未釋放的紀錄（= Memory Leak 候選）
  void Dump(FILE *stream); // 用到前再實作
};

void MemoryRecords::Dump(FILE *stream) {
  // Step 1: 在 Critical Section 內複製（快照）
  std::vector<MemoryRec> snapshot;
  {
    std::lock_guard<std::mutex> g(lock_);
    snapshot.reserve(records_.size());
    for (auto *rec : records_)
      snapshot.push_back(*rec);
  }
  // Step 2: 釋鎖後排序 + 輸出（不會阻塞其他執行緒）
  std::ranges::sort(
      snapshot, [](const auto &a, const auto &b) { return a.time_ < b.time_; });

  fprintf(stream, "\n=== Unreleased Memory ===\n");
  size_t total = 0;
  for (const auto &rec : snapshot) {
    auto t = std::chrono::system_clock::to_time_t(rec.time_);
    char ts[26];
    ctime_r(&t, ts); // 執行緒安全版 ctime
    ts[24] = '\0';   // 去掉尾端換行
    fprintf(stream, "  [%s] ptr=%p  size=%zu\n", ts, rec.ptr_, rec.size_);
    total += rec.size_;
  }
  fprintf(stream, "Total: %zu bytes in %zu blocks\n", total, snapshot.size());
  fprintf(stream, "=========================\n");
}

/// 用指標而非靜態物件，手動控制生命週期。
/// 若用靜態物件，程式 exit 階段其他解構子呼叫 free 時，物件可能已被解構 →
/// Crash。
static MemoryRecords *g_records = nullptr;

// ============================================================
// 第五部分：初始化
// ============================================================
static void InitReal() {
  if (g_initialized)
    return;
  g_initializing = true;
  real_malloc = (MallocFunc *)dlsym(RTLD_NEXT, "malloc");
  real_free = (FreeFunc *)dlsym(RTLD_NEXT, "free");
  real_calloc = (CallocFunc *)dlsym(RTLD_NEXT, "calloc");
  real_realloc = (ReallocFunc *)dlsym(RTLD_NEXT, "realloc");
  // 建立 g_records 時必須設 g_inside_hook，
  // 因為 new MemoryRecords 內部的 unordered_set 建構會呼叫 malloc
  if (!g_records) {
    HookGuard guard;
    g_records = new MemoryRecords();
  }
  g_initialized = true;
  g_initializing = false;
}

static inline void EnsureInit() {
  if (!g_initialized && !g_initializing)
    InitReal();
}

// ============================================================
// 第六部分：日誌輸出
// ============================================================

// ============================================================
// 使用 write 這種底層函數，避免 fprintf 可能觸發 malloc → 再拿鎖 → Deadlock
// ============================================================
static void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char *fmt, ...) {
  char buf[256];
  int len = snprintf(buf, sizeof(buf), "[TID:%ld] ", syscall(SYS_gettid));
  va_list ap;
  va_start(ap, fmt);
  len += vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
  va_end(ap);
  write(STDERR_FILENO, buf, len);
}

// ============================================================
// 第七部分：Template 共用邏輯
// ============================================================
//
// 觀察：malloc, calloc, new, new[] 的 Hook 結構幾乎一樣：
//   1. 初始化中 → StaticAlloc
//   2. 遞迴呼叫 → 直接轉發
//   3. 正常路徑 → HookGuard + 呼叫 real + 記錄 + Log
//
// free, delete, delete[] 也是如此。
// 用 template 抽出共同邏輯，各 Hook 只需寫差異的部分。
//

/// 配置類 Hook 的共用邏輯
/// @tparam AllocFn  原始配置函式型別（如 MallocFunc*）
/// @param tag       Log 標籤（如 "malloc", "new"）
/// @param recSize   要記錄的大小（calloc 是 nmemb*size）
/// @param fn        原始函式指標
/// @param args      傳給原始函式的參數
template <typename AllocFn, typename... Args>
static void *HookedAlloc(const char *tag, size_t recSize, AllocFn fn,
                         Args... args) {
  if (g_inside_hook)
    return fn(args...);

  HookGuard guard;
  void *ptr = fn(args...);
  if (ptr && g_records)
    g_records->Add(ptr, recSize);
  Log("[%-7s] size=%-8zu → ptr=%p\n", tag, recSize, ptr);
  return ptr;
}

/// 釋放類 Hook 的共用邏輯
static void HookedFree(const char *tag, void *ptr) {
  if (g_inside_hook) {
    real_free(ptr);
    return;
  }

  HookGuard guard;
  if (g_records)
    g_records->Remove(ptr);
  Log("[%-7s] ptr=%p\n", tag, ptr);
  real_free(ptr);
}

// ============================================================
// 第八部分：malloc / free / calloc / realloc Hook
// ============================================================
extern "C" {

void *malloc(size_t size) {
  if (g_initializing)
    return StaticAlloc(size);
  EnsureInit();
  return HookedAlloc("malloc", size, real_malloc, size);
}

void free(void *ptr) {
  if (!ptr)
    return;
  if (IsStaticMemory(ptr))
    return;
  if (g_initializing && !real_free)
    return;
  EnsureInit();
  HookedFree("free", ptr);
}

void *calloc(size_t nmemb, size_t size) {
  if (g_initializing) {
    if (!real_calloc) {
      void *p = StaticAlloc(nmemb * size);
      memset(p, 0, nmemb * size);
      return p;
    }
    return real_calloc(nmemb, size);
  }
  EnsureInit();
  return HookedAlloc("calloc", nmemb * size, real_calloc, nmemb, size);
}

void *realloc(void *old_ptr, size_t size) {
  // realloc 邏輯較特殊（Remove old + Add new），不套 template
  if (IsStaticMemory(old_ptr)) {
    void *p = malloc(size);
    if (old_ptr && p) {
      size_t old_avail =
          (g_static_buf + sizeof(g_static_buf)) - (char *)old_ptr;
      memcpy(p, old_ptr, old_avail < size ? old_avail : size);
    }
    return p;
  }
  if (g_initializing && !real_realloc)
    return StaticAlloc(size);
  EnsureInit();
  if (g_inside_hook)
    return real_realloc(old_ptr, size);

  HookGuard guard;
  void *new_ptr = real_realloc(old_ptr, size);
  if (g_records) {
    if (old_ptr)
      g_records->Remove(old_ptr);
    if (new_ptr)
      g_records->Add(new_ptr, size);
  }
  Log("[realloc] old=%p size=%-8zu → new=%p\n", old_ptr, size, new_ptr);
  return new_ptr;
}

void dump_memory_records(FILE *stream) {
  if (g_records) {
    HookGuard guard;
    g_records->Dump(stream);
  }
}

} // extern "C"

// ============================================================
// 第九部分：C++ new / delete
// ============================================================
//
// new/new[] 完全相同，delete/delete[] 也是，只差 Log 標籤。
// 再抽出 NewImpl / DeleteImpl，各 operator 只剩一行。
//
static void *NewImpl(const char *tag, size_t size) {
  if (g_initializing)
    return StaticAlloc(size);
  EnsureInit();
  void *ptr = HookedAlloc(tag, size, real_malloc, size);
  if (!ptr)
    throw std::bad_alloc();
  return ptr;
}

static void DeleteImpl(const char *tag, void *ptr) noexcept {
  if (!ptr)
    return;
  if (IsStaticMemory(ptr))
    return;
  if (g_initializing)
    return;
  EnsureInit();
  HookedFree(tag, ptr);
}

void *operator new(size_t size) { return NewImpl("new", size); }
void *operator new[](size_t size) { return NewImpl("new[]", size); }
void operator delete(void *ptr) noexcept { DeleteImpl("delete", ptr); }
void operator delete[](void *ptr) noexcept { DeleteImpl("del[]", ptr); }
void operator delete(void *ptr, size_t) noexcept { DeleteImpl("delete", ptr); }
void operator delete[](void *ptr, size_t) noexcept { DeleteImpl("del[]", ptr); }

// ============================================================
// 第十部分：測試 — 故意留下 Memory Leak
// ============================================================
int main() {
  fprintf(stderr, "===== 測試開始 =====\n");

  // 正常配對
  fprintf(stderr, "\n--- 正常 malloc/free ---\n");
  char *p1 = (char *)malloc(100);
  free(p1);

  // 正常配對
  fprintf(stderr, "\n--- 正常 new/delete ---\n");
  int *p2 = new int(42);
  delete p2;

  // *** 故意 Leak ***
  fprintf(stderr, "\n--- 故意 Leak ---\n");
  char *leak1 = (char *)malloc(256); // Leak!
  int *leak2 = new int[100];         // Leak!
  (void)leak1;
  (void)leak2;

  // realloc 測試
  fprintf(stderr, "\n--- realloc ---\n");
  char *p3 = (char *)malloc(32);
  p3 = (char *)realloc(p3, 128); // Leak! (沒有 free)
  (void)p3;

  // Dump：應該看到 3 筆未釋放的紀錄
  dump_memory_records(stderr);

  return 0;
}
