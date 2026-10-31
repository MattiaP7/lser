#include "lser.hpp"

#include <benchmark/benchmark.h>
#include <malloc.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <print>
#include <string>

static std::string g_target = "/usr/include";

inline std::size_t get_alloc_size(void *p) noexcept {
#ifdef _WIN32
  return _msize(p);
#else
  return malloc_usable_size(p);
#endif
}
namespace {
std::atomic<bool> g_track{false};
std::atomic<std::int64_t> g_current{0};
std::atomic<std::int64_t> g_peak{0};
std::atomic<std::int64_t> g_churn{0};
std::atomic<std::int64_t> g_allocs{0};

void account_alloc(std::int64_t n) noexcept {
  g_churn.fetch_add(n, std::memory_order_relaxed);
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  const std::int64_t cur =
      g_current.fetch_add(n, std::memory_order_relaxed) + n;
  std::int64_t peak = g_peak.load(std::memory_order_relaxed);
  while (cur > peak &&
         !g_peak.compare_exchange_weak(peak, cur, std::memory_order_relaxed)) {
  }
}
} // namespace

void *operator new(std::size_t size) {
  void *p = std::malloc(size != 0 ? size : 1);
  if (p == nullptr)
    throw std::bad_alloc();
  if (g_track.load(std::memory_order_relaxed))
    account_alloc(static_cast<std::int64_t>(get_alloc_size(p)));
  return p;
}

void operator delete(void *p) noexcept {
  if (p == nullptr)
    return;
  if (g_track.load(std::memory_order_relaxed))
    g_current.fetch_sub(static_cast<std::int64_t>(get_alloc_size(p)),
                        std::memory_order_relaxed);
  std::free(p);
}

void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }

static void RunEngine(benchmark::State &state, lser::ListFlags flags) {
  flags.threads = static_cast<unsigned>(state.range(0));

  std::int64_t churn_total = 0;
  std::int64_t allocs_total = 0;
  std::int64_t peak_max = 0;
  std::size_t entries = 0;

  for (auto _ : state) {
    const std::int64_t base = g_current.load(std::memory_order_relaxed);
    const std::int64_t churn0 = g_churn.load(std::memory_order_relaxed);
    const std::int64_t allocs0 = g_allocs.load(std::memory_order_relaxed);
    g_peak.store(base, std::memory_order_relaxed);

    lser::LSer engine(g_target, flags);
    engine.run_engine_only();

    entries = engine.count();
    benchmark::DoNotOptimize(entries);
    benchmark::ClobberMemory();

    churn_total += g_churn.load(std::memory_order_relaxed) - churn0;
    allocs_total += g_allocs.load(std::memory_order_relaxed) - allocs0;
    peak_max =
        std::max(peak_max, g_peak.load(std::memory_order_relaxed) - base);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(entries) *
                          static_cast<std::int64_t>(state.iterations()));
  state.counters["Entries"] = static_cast<double>(entries);

  if (g_track.load(std::memory_order_relaxed)) {
    constexpr double MB = 1024.0 * 1024.0;
    state.counters["Churn_MB"] =
        benchmark::Counter(static_cast<double>(churn_total) / MB,
                           benchmark::Counter::kAvgIterations);
    state.counters["Allocs"] = benchmark::Counter(
        static_cast<double>(allocs_total), benchmark::Counter::kAvgIterations);
    state.counters["Peak_MB"] = static_cast<double>(peak_max) / MB;
  }
}

static void BM_LSer_Recursive_Simple(benchmark::State &state) {
  lser::ListFlags flags;
  flags.recursive = true;
  RunEngine(state, flags);
}

static void BM_LSer_Recursive_LongFormat(benchmark::State &state) {
  lser::ListFlags flags;
  flags.recursive = true;
  flags.long_format = true;
  flags.show_hidden = true;
  RunEngine(state, flags);
}

static void BM_LSer_Recursive_SortSize(benchmark::State &state) {
  lser::ListFlags flags;
  flags.recursive = true;
  flags.long_format = true;
  flags.show_hidden = true;
  flags.sort_by = lser::SortBy::Size;
  RunEngine(state, flags);
}

static void BM_LSer_TreeView(benchmark::State &state) {
  lser::ListFlags flags;
  flags.tree_view = true;
  flags.show_hidden = true;
  RunEngine(state, flags);
}

static void ThreadArguments(benchmark::internal::Benchmark *b) {
  // aggiungiamo potenze di 2: 1, 2, 4, ... fino a superare i core massimi
  unsigned int max_cores = std::thread::hardware_concurrency();
  for (unsigned int threads = 1; threads <= max_cores; threads *= 2) {
    b->Args({threads});
  }

  // se non e' potenza di 2 aggiungiamo il max
  if (max_cores > 0 && (max_cores & (max_cores - 1)) != 0) {
    b->Args({max_cores});
  }

  // comportamento default
  b->Args({0});
}

#define LSER_BENCH(fn)                                                         \
  BENCHMARK(fn)                                                                \
      ->ArgName("threads")                                                     \
      ->Apply(ThreadArguments)                                                 \
      ->Unit(benchmark::kMillisecond)                                          \
      ->UseRealTime()

LSER_BENCH(BM_LSer_Recursive_Simple);
LSER_BENCH(BM_LSer_Recursive_LongFormat);
LSER_BENCH(BM_LSer_Recursive_SortSize);
LSER_BENCH(BM_LSer_TreeView);

static bool verify_against_std_filesystem(const std::string &path) {
  std::size_t expected = 0;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator
           it(path, std::filesystem::directory_options::skip_permission_denied,
              ec),
       end;
       it != end; it.increment(ec)) {
    ++expected;
  }

  lser::ListFlags flags;
  flags.recursive = true;
  flags.show_hidden = true;
  lser::LSer engine(path, flags);
  engine.run_engine_only();
  const std::size_t got = engine.count();

  if (got == expected) {
    std::println(" Verifica         : OK ({} entry, uguale a std::filesystem)",
                 got);
    return true;
  }
  std::println(stderr,
               " Verifica         : MISMATCH! LSer={} std::filesystem={} (se "
               "la cartella cambia mentre si scansiona e' normale)",
               got, expected);
  return false;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc;) {
    if (std::strcmp(argv[i], "--track-memory") == 0) {
      g_track.store(true, std::memory_order_relaxed);
      for (int j = i; j < argc - 1; ++j)
        argv[j] = argv[j + 1];
      --argc;
    } else {
      ++i;
    }
  }

  benchmark::Initialize(&argc, argv);

  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {
      g_target = argv[i];
      break;
    }
  }

  std::println("\n======================================================");
  std::println(" LSer Benchmark");
  std::println(" Target Directory : {}", g_target);
  std::println(" Tracciamento mem : {}",
               (g_track ? "ATTIVO (tempi falsati)" : "spento"));

  std::error_code ec;
  if (!std::filesystem::exists(g_target, ec)) {
    std::println(stderr, "ERRORE: Il percorso specificato non esiste: {}",
                 g_target);
    return 1;
  }
  verify_against_std_filesystem(g_target);
  std::println("======================================================\n");

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
