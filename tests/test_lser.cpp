// Test di correttezza per LSer. Nessuna dipendenza esterna: micro-framework in
// stile GTest.
#include "lser.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using lser::LSer;

// ============================================================================
// Micro-Framework di Testing
// ============================================================================
namespace testing {
int failed_checks = 0;
int passed_tests = 0;
int total_tests = 0;
bool current_test_failed = false;

void fail_check(const char *expr, const char *file, int line) {
  std::println(stderr, "\033[1;31m  -> CHECK FALLITO:\033[0m {} a {}:{}", expr,
               file, line);
  current_test_failed = true;
  failed_checks++;
}

void run(std::string_view name, const std::function<void()> &test_body) {
  std::print("\033[1;36m[ RUN      ]\033[0m {}\n", name);
  current_test_failed = false;

  auto start = std::chrono::high_resolution_clock::now();
  try {
    test_body();
  } catch (const std::exception &e) {
    std::println(stderr, "\033[1;31m  -> ECCEZIONE:\033[0m {}", e.what());
    current_test_failed = true;
    failed_checks++;
  }
  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration<double, std::milli>(end - start).count();

  if (current_test_failed) {
    std::println("\033[1;31m[  FAILED  ]\033[0m {} ({:.3f} ms)", name, ms);
  } else {
    std::println("\033[1;32m[       OK ]\033[0m {} ({:.3f} ms)", name, ms);
    passed_tests++;
  }
  total_tests++;
}
} // namespace testing

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      testing::fail_check(#cond, __FILE__, __LINE__);                          \
    }                                                                          \
  } while (0)

// ============================================================================
// Helper Functions
// ============================================================================
static void write_file(const fs::path &p, std::size_t bytes = 0) {
  std::ofstream f(p, std::ios::binary);
  f << std::string(bytes, 'x');
}

static std::string capture(lser::LSer &l, int *rc = nullptr) {
  std::FILE *out = std::tmpfile();
  std::FILE *err = std::tmpfile();
  const int r = l.execute(out, err);
  if (rc)
    *rc = r;
  std::fflush(out);
  std::rewind(out);
  std::string s;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, out)) > 0) {
    s.append(buf, n);
  }
  std::fclose(out);
  std::fclose(err);
  return s;
}

static std::vector<std::string> names_in(const lser::DirectoryResult &d) {
  std::vector<std::string> v;
  v.reserve(d.entries.size());
  for (const auto &e : d.entries)
    v.push_back(e.name);
  return v;
}

static const lser::DirectoryResult *find_dir(const lser::LSer &l,
                                             const fs::path &p) {
  for (const auto &d : l.directories()) {
    if (d.path == p.string())
      return &d;
  }
  return nullptr;
}

// ============================================================================
// Main & Test Suites
// ============================================================================
int main() {
  auto global_start = std::chrono::high_resolution_clock::now();

  const fs::path root =
      fs::temp_directory_path() /
      std::format("lser_test_{}", static_cast<int>(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "sub" / "deep");
  fs::create_directories(root / "empty");
  write_file(root / "a.txt", 10);
  write_file(root / ".hidden", 1);
  write_file(root / "sub" / "b.txt", 200);
  write_file(root / "sub" / "deep" / "c.txt", 3000);
  fs::create_directory_symlink(root, root / "loop");
  fs::create_symlink("a.txt", root / "link_a");

  std::println(
      "\033[1;35m==================================================\033[0m");
  std::println("\033[1;37m Esecuzione Test Suite: LSer\033[0m");
  std::println(
      "\033[1;35m==================================================\033[0m");

  testing::run("1. Lettura base (Non ricorsivo)", [&]() {
    lser::LSer l(root.string());
    l.run_engine_only();
    CHECK(l.count() == 5);
    CHECK(l.directories().size() == 1);
    CHECK(l.errors().empty());
    l.with_hidden();
    l.run_engine_only();
    CHECK(l.count() == 6);
  });

  testing::run("2. Esplorazione ricorsiva & Symlink Loop", [&]() {
    lser::ListFlags f;
    f.recursive = true;
    f.show_hidden = true;
    lser::LSer l(root.string(), f);
    l.run_engine_only();
    CHECK(l.count() == 9);
    CHECK(l.directories().size() == 4);
    CHECK(l.errors().empty());

    const auto *top = find_dir(l, root);
    CHECK(top != nullptr);
    if (top) {
      bool found = false;
      for (const auto &e : top->entries) {
        if (e.name == "loop") {
          found = true;
          CHECK(e.is_symlink());
          CHECK(!e.is_directory());
        }
      }
      CHECK(found);
    }
    CHECK(l.directories().front().path == root.string());
  });

  testing::run("3. Rispetto della profondita' massima (-L)", [&]() {
    lser::ListFlags f;
    f.recursive = true;
    f.max_depth = 1;
    lser::LSer l1(root.string(), f);
    l1.run_engine_only();
    CHECK(l1.directories().size() == 1);

    f.max_depth = 2;
    lser::LSer l2(root.string(), f);
    l2.run_engine_only();
    CHECK(l2.directories().size() == 3);

    f.max_depth = -1;
    lser::LSer l3(root.string(), f);
    l3.run_engine_only();
    CHECK(l3.directories().size() == 4);
  });

  testing::run("4. Filtro solo directory (-D)", [&]() {
    lser::ListFlags f;
    f.recursive = true;
    f.dirs_only = true;
    lser::LSer l(root.string(), f);
    l.run_engine_only();
    CHECK(l.count() == 3);
  });

  testing::run("5. Ordinamento Avanzato (Size, Name, Case, Reverse)", [&]() {
    const fs::path d = root / "sorting";
    fs::create_directories(d);
    write_file(d / "f1", 1);
    write_file(d / "f2", 100);
    write_file(d / "f3", 50);
    write_file(d / "B");
    write_file(d / "a");
    write_file(d / "C");

    lser::ListFlags f;
    f.sort_by = lser::SortBy::Size;
    lser::LSer by_size(d.string(), f);
    by_size.run_engine_only();
    auto n = names_in(by_size.directories()[0]);
    CHECK(n.size() == 6);
    CHECK(n[0] == "f2" && n[1] == "f3" && n[2] == "f1");

    f.reverse_sort = true;
    lser::LSer by_size_r(d.string(), f);
    by_size_r.run_engine_only();
    n = names_in(by_size_r.directories()[0]);
    CHECK(n[n.size() - 1] == "f2");

    lser::ListFlags g;
    lser::LSer by_name(d.string(), g);
    by_name.run_engine_only();
    n = names_in(by_name.directories()[0]);
    CHECK(n[0] == "B" && n[1] == "C" && n[2] == "a");

    g.ignore_case = true;
    lser::LSer icase(d.string(), g);
    icase.run_engine_only();
    n = names_in(icase.directories()[0]);
    CHECK(n[0] == "a" && n[1] == "B" && n[2] == "C");

    g.ignore_case = false;
    g.reverse_sort = true;
    lser::LSer rev(d.string(), g);
    rev.run_engine_only();
    n = names_in(rev.directories()[0]);
    CHECK(n[0] == "f3" && n.back() == "B");
  });

  testing::run("6. Gestione errori e percorsi inesistenti", [&]() {
    int rc = -1;
    lser::LSer missing("/nonexistent_lser_path_xyz");
    capture(missing, &rc);
    CHECK(rc == 2);

    lser::LSer file((root / "a.txt").string());
    const std::string out = capture(file, &rc);
    CHECK(rc == 0);
    CHECK(out == (root / "a.txt").string() + "\n");
  });

  testing::run("7. Gestione permessi negati", [&]() {
    if (::geteuid() == 0) {
      std::println("\033[1;33m  -> SKIP:\033[0m Test permessi saltato "
                   "(eseguito come root)");
      return;
    }
    const fs::path d = root / "noperm";
    fs::create_directories(d);
    ::chmod(d.c_str(), 0000);

    lser::ListFlags f;
    f.recursive = true;
    LSer l(root.string(), f);
    int rc = -1;
    capture(l, &rc);

    CHECK(rc == 1);
    CHECK(l.errors().size() == 1);
    CHECK(!l.errors().empty() &&
          l.errors()[0].find("noperm") != std::string::npos);
    ::chmod(d.c_str(), 0755);
  });

  testing::run("8. Formattazione dell'output (Tree View & Long Format)", [&]() {
    lser::ListFlags f;
    f.tree_view = true;
    f.show_hidden = true;
    lser::LSer l(root.string(), f);
    const std::string out = capture(l);
    CHECK(out.find("├── ") != std::string::npos);
    CHECK(out.find("└── ") != std::string::npos);
    CHECK(out.find("c.txt") != std::string::npos);
    CHECK(out.find("loop -> ") != std::string::npos);
    CHECK(out.find("cartelle,") != std::string::npos);

    lser::ListFlags g;
    g.long_format = true;
    lser::LSer ll(root.string(), g);
    const std::string lo = capture(ll);
    CHECK(lo.find("link_a -> a.txt") != std::string::npos);
    CHECK(lo.find("lrwx") != std::string::npos);
  });

  testing::run("9. Thread Safety & Race Conditions (1..8 Threads)", [&]() {
    std::vector<fs::path> reference;
    for (unsigned threads : {1u, 2u, 4u, 8u}) {
      lser::ListFlags f;
      f.recursive = true;
      f.show_hidden = true;
      f.threads = threads;
      lser::LSer l(root.string(), f);

      const std::size_t expected_count = 16 + (::geteuid() != 0 ? 1 : 0);
      for (int rep = 0; rep < 100; ++rep) {
        l.run_engine_only();
        CHECK(l.count() == expected_count);
      }

      auto paths = l.all_paths();
      std::sort(paths.begin(), paths.end());
      if (reference.empty())
        reference = paths;
      CHECK(paths == reference);
    }
  });

  testing::run("10. Allineamento con std::filesystem", [&]() {
    std::size_t expected = 0;
    std::error_code e2;
    for (fs::recursive_directory_iterator
             it(root, fs::directory_options::skip_permission_denied, e2),
         end;
         it != end; it.increment(e2)) {
      ++expected;
    }

    lser::ListFlags f;
    f.recursive = true;
    f.show_hidden = true;
    LSer l(root.string(), f);
    l.run_engine_only();
    CHECK(l.count() == expected);
  });

  // Cleanup
  ::chmod((root / "noperm").c_str(), 0755);
  fs::remove_all(root, ec);

  // Summary
  auto global_end = std::chrono::high_resolution_clock::now();
  auto total_ms =
      std::chrono::duration<double, std::milli>(global_end - global_start)
          .count();

  std::println(
      "\033[1;35m==================================================\033[0m");
  std::println("\033[1;37m RIEPILOGO TEST\033[0m");
  std::println(
      "\033[1;35m==================================================\033[0m");

  if (testing::failed_checks == 0) {
    std::println(
        "\033[1;32m[  PASSED  ]\033[0m {} su {} test passati con successo.",
        testing::passed_tests, testing::total_tests);
    std::println("\033[1;32m[  TIME    ]\033[0m Tempo totale: {:.3f} ms",
                 total_ms);
    return 0;
  } else {
    std::println("\033[1;31m[  FAILED  ]\033[0m Falliti {} controlli interni.",
                 testing::failed_checks);
    std::println("\033[1;31m[  TIME    ]\033[0m Tempo totale: {:.3f} ms",
                 total_ms);
    return 1;
  }
}
