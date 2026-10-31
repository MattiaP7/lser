#include "lser.hpp"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

enum class ColorMode { Auto, Always, Never };

void print_help() {
  std::print(
      R"(Uso: lser [OPZIONI] [PERCORSO...]
LSer - clone C++23 multithreaded ad alte prestazioni di ls/tree (Linux).

Opzioni base:
  -a         Mostra i file nascosti (iniziano con '.')
  -l         Formato lungo (permessi, link, utente, gruppo, size, data, nome)
  -h         Size leggibili (K, M, G). Richiede -l
  -r         Ricorsivo: esplora tutte le sottocartelle (i symlink non vengono seguiti)
  -T         Mostra a forma di albero gerarchico (implica -r)
  -L N       Profondita' massima (1 = solo i figli diretti). Vale con -r e -T
  -D         Mostra solo le directory
  -g         Parsa i .gitignore per saltare le directory ignorate (es. node_modules, build)

Ordinamento:
  -S         Ordina per dimensione (decrescente)
  -t         Ordina per data di ultima modifica (i piu' recenti prima)
  -R         Inverte l'ordine (Reverse)
  -i         Ordinamento per nome case-insensitive

Output e prestazioni:
  --color[=quando]  Colori: auto (default), always, never
  --summary         Stampa il totale di cartelle/file (il tree lo stampa sempre)
  -j N              Numero di thread (default: tutti i core)
  -E                Stampa su stderr il tempo di esecuzione del motore
  --help            Mostra questo messaggio

Note: '.' e '..' non vengono mai mostrati. Con '--' finiscono le opzioni.
Codici di uscita: 0 ok, 1 errori parziali (es. permessi), 2 percorso non accessibile.

Esempi:
  lser -la /usr/include       (Dettagli e nascosti)
  lser -aT -g /home/user      (Albero rispettando le regole Git)
  lser -lSr /var/log          (File piu' grandi, ricorsivamente)
)");
}

[[nodiscard]] bool parse_uint(std::string_view s, unsigned &out) {
  if (s.empty())
    return false;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && ptr == s.data() + s.size();
}

} // namespace

int main(int argc, char **argv) {
  lser::ListFlags flags;
  ColorMode color = ColorMode::Auto;
  std::vector<std::string> paths;
  bool options_done = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    if (options_done || arg.size() < 2 || arg[0] != '-') {
      paths.emplace_back(arg);
      continue;
    }

    if (arg == "--") {
      options_done = true;
      continue;
    }

    if (arg.starts_with("--")) {
      if (arg == "--help") {
        print_help();
        return 0;
      } else if (arg == "--summary") {
        flags.summary = true;
      } else if (arg == "--git-ignore") {
        flags.git_ignore = true;
      } else if (arg == "--color" || arg == "--color=always") {
        color = ColorMode::Always;
      } else if (arg == "--color=auto") {
        color = ColorMode::Auto;
      } else if (arg == "--color=never") {
        color = ColorMode::Never;
      } else {
        std::println(stderr, "Errore: opzione sconosciuta '{}'", arg);
        std::println(stderr, "Usa 'lser --help' per la lista dei comandi.");
        return 2;
      }
      continue;
    }

    for (std::size_t k = 1; k < arg.size(); ++k) {
      const char c = arg[k];
      switch (c) {
      case 'a':
        flags.show_hidden = true;
        break;
      case 'l':
        flags.long_format = true;
        break;
      case 'h':
        flags.human_readable = true;
        break;
      case 'r':
        flags.recursive = true;
        break;
      case 'T':
        flags.tree_view = true;
        break;
      case 'D':
        flags.dirs_only = true;
        break;
      case 'S':
        flags.sort_by = lser::SortBy::Size;
        break;
      case 't':
        flags.sort_by = lser::SortBy::Time;
        break;
      case 'R':
        flags.reverse_sort = true;
        break;
      case 'i':
        flags.ignore_case = true;
        break;
      case 'E':
        flags.time_exec = true;
        break;
      case 'g':
        flags.git_ignore = true;
        break;
      case 'L':
      case 'j': {
        std::string_view value;
        if (k + 1 < arg.size())
          value = arg.substr(k + 1);
        else if (i + 1 < argc)
          value = argv[++i];
        else {
          std::println(stderr, "Errore: l'opzione '-{}' richiede un numero", c);
          return 2;
        }
        unsigned n = 0;
        if (!parse_uint(value, n) || n == 0 || n > 4096) {
          std::println(stderr, "Errore: valore non valido per '-{}': '{}'", c,
                       value);
          return 2;
        }
        if (c == 'L')
          flags.max_depth = static_cast<int>(n);
        else
          flags.threads = n;
        k = arg.size();
        break;
      }
      default:
        std::println(stderr, "Errore: opzione sconosciuta '-{}'", c);
        std::println(stderr, "Usa 'lser --help' per la lista dei comandi.");
        return 2;
      }
    }
  }

  if (paths.empty())
    paths.emplace_back(".");

  switch (color) {
  case ColorMode::Always:
    flags.color = true;
    break;
  case ColorMode::Never:
    flags.color = false;
    break;
  case ColorMode::Auto:
    flags.color =
        ::isatty(STDOUT_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr;
    break;
  }
  flags.force_headers = paths.size() > 1;

  int rc = 0;
  try {
    for (std::size_t i = 0; i < paths.size(); ++i) {
      if (i > 0)
        std::print("\n");
      lser::LSer engine(paths[i], flags);
      rc = std::max(rc, engine.execute());
    }
  } catch (const std::exception &e) {
    std::println(stderr, "lser: errore fatale: {}", e.what());
    return 2;
  }
  return rc;
}
