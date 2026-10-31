/**
 * @file lser.hpp
 * @brief LSer - Clone C++23 multithreaded di ls / tree ad alte prestazioni.
 *
 * Implementa un motore Data-Oriented per l'esplorazione e l'ordinamento di
 * directory. Progettato per massimizzare la cache locality tramite flat vectors
 * e zero-copy.
 *
 * @author Mattia Pirazzi
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef LSER_HAS_TBB
  #include <execution>
#endif

// ----------------------------------------------------------------------------
// Astrazione di Sistema (OS Abstraction Layer)
// ----------------------------------------------------------------------------
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>

  // MinGW ha già mode_t, MSVC no
  #ifndef __MINGW32__
using mode_t = unsigned int;
  #endif

  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
#else
  // Sistemi POSIX (Linux, macOS, ecc.)
  #include <dirent.h>
  #include <fcntl.h>
  #include <grp.h>
  #include <pwd.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lser {

/**
 * @brief Criteri di ordinamento disponibili.
 */
enum class SortBy { Name, Size, Time };

/**
 * @struct ListFlags
 * @brief Struttura di configurazione dei parametri operativi.
 *
 * Contiene tutti i flag booleani e numerici passati da linea di comando che
 * alterano il comportamento della formattazione e della scansione del motore
 * LSer.
 */
struct ListFlags {
  bool show_hidden{false}; ///< Se true, include i file che iniziano con '.'.
  bool long_format{false}; ///< Formatta output simile a `ls -l` recuperando
                           ///< dettagli di permessi, owner e time.
  bool human_readable{
      false}; ///< Converte i bytes (size) in potenze del 1024 (KB, MB, GB).
  bool recursive{false}; ///< Attiva la scansione in profondita per tutte le
                         ///< sotto cartelle.
  bool tree_view{false}; ///< Output formattato ad albero (implica recursive).
  bool reverse_sort{false}; ///< Inverte il criterio di ordinamento prescelto.
  bool dirs_only{false};    ///< Ignora tutti i file regolari/symlink,
                            ///< collezionando solo le directory.
  bool ignore_case{false};  ///< Ignora la distinzione maiuscole/minuscole
                            ///< durante il sorting alfabetico.
  bool color{false}; ///< Utilizza i codici ANSI escape per colorare i path
                     ///< nell'output.
  bool summary{
      false}; ///< Mostra un report finale sul totale degli elementi trovati.
  bool time_exec{false}; ///< Esegue il timing del motore e lo stampa su stderr.
  bool force_headers{false}; ///< Forza la stampa del blocco "path:" ad inizio
                             ///< dir anche non in recursive.
  bool git_ignore{false}; ///< Applica le regole dei file `.gitignore` trovati
                          ///< durante lo scan.
  SortBy sort_by{SortBy::Name}; ///< Criterio principale per l'ordinamento.
  int max_depth{-1};   ///< Limita la profondita di scansione. -1 significa
                       ///< nessuna limitazione.
  unsigned threads{0}; ///< Imposta il numero di worker threads. 0 = numero
                       ///< logico di core del sistema.
};

/**
 * @brief Elenco tipizzato dei file per un parsing semplificato.
 */
enum class FileType : std::uint8_t {
  Unknown,
  Regular,
  Directory,
  Symlink,
  Other
};

/**
 * @struct FileEntry
 * @brief Struttura dati piatta, memoria allineata per l'uso cache-friendly del
 * Data-Oriented Design.
 *
 * Contiene tutti i metadati estratti per la stampa di un singolo elemento del
 * File System. I path radice non vengono inseriti qui ma nel container genitore
 * (DirectoryResult) per evitare allocazioni stringa superflue e RAM footprint
 * elevato.
 */
struct FileEntry {
  std::string name{};     ///< Nome base del file, senza percorso.
  std::uintmax_t size{0}; ///< Dimensione in bytes (valida per Regular files).
  std::int64_t mtime_sec{
      0}; ///< Data di ultima modifica in timestamp Unix (secondi).
  std::uint32_t mtime_nsec{0}; ///< Parte nanosecondi del timestamp.
  std::uint32_t mode{0};       ///< Maschera di bit dei permessi POSIX (su Win32
                               ///< contiene mapping fittizio).
  std::uint32_t uid{0};        ///< ID dell'utente proprietario. (Su win un hash
                               ///< univoco o string length).
  std::uint32_t gid{0};        ///< ID del gruppo proprietario.
  std::uint32_t nlink{0};      ///< Contatore per hard-link.
  std::int32_t link_idx{
      -1}; ///< Indice stringa del percorso risolto (se Symlink), referenziato
           ///< nel vettore Links padre.
  FileType type{
      FileType::Unknown};  ///< Enum che classifica il tipo di file trovato.
  bool has_details{false}; ///< Segnala se i dati (size, date, user) sono stati
                           ///< popolati tramite stat/winAPI.

#ifdef _WIN32
  // Estensione per Windows in quanto User/Group sono SID strings lunghe, non ID
  // numerici.
  std::string win_user{};
  std::string win_group{};
#endif

  /**
   * @brief Helper per verificare velocemente se l'entry punta a una cartella.
   * @return true se il tipo corrisponde a FileType::Directory.
   */
  [[nodiscard]] bool is_directory() const noexcept {
    return type == FileType::Directory;
  }

  /**
   * @brief Helper per verificare velocemente se l'entry punta a un collegamento
   * simbolico.
   * @return true se il tipo corrisponde a FileType::Symlink.
   */
  [[nodiscard]] bool is_symlink() const noexcept {
    return type == FileType::Symlink;
  }

  /**
   * @brief Helper POSIX per stabilire se si tratta di un file nascosto.
   * @return true se il nome file comincia con un carattere 'punto' (.).
   */
  [[nodiscard]] bool is_hidden() const noexcept {
    return !name.empty() && name[0] == '.';
  }
};

/**
 * @struct DirectoryResult
 * @brief Raccoglitore per i risultati della scansione di un intero livello di
 * cartella.
 *
 * Utilizzato dai worker per accumulare batch di file thread-locally per poi
 * pusharli ai risultati.
 */
struct DirectoryResult {
  std::string path{}; ///< Path del ramo elaborato.
  std::vector<FileEntry>
      entries{}; ///< Array piatto continuo (DOD) per la L1/L2 cache.
  std::vector<std::string>
      links{}; ///< Targets effettivi prelevati durante readlink (se Symlinks)

  /**
   * @brief Ritorna il puntatore stringa se quest'entry contiene un symlink,
   * altrimenti nullptr.
   * @param e La entry da interrogare.
   */
  [[nodiscard]] const std::string *
  link_target(const FileEntry &e) const noexcept {
    return e.link_idx >= 0 ? &links[static_cast<std::size_t>(e.link_idx)]
                           : nullptr;
  }
};

/**
 * @namespace detail
 * @brief Implementazioni interne per l'astrazione OS, formattazioni stringa e
 * buffers (non API utente).
 */
namespace detail {
inline constexpr std::string_view kReset = "\033[0m";
inline constexpr std::string_view kBlue = "\033[1;34m";
inline constexpr std::string_view kCyan = "\033[1;36m";
inline constexpr std::string_view kGreen = "\033[1;32m";

#ifndef _WIN32
/**
 * @brief Mappa d_type della struct dirent al nostro FileType Enum. (OS POSIX)
 */
[[nodiscard]] inline FileType type_from_dtype(unsigned char d) noexcept {
  switch (d) {
  case DT_REG:
    return FileType::Regular;
  case DT_DIR:
    return FileType::Directory;
  case DT_LNK:
    return FileType::Symlink;
  case DT_UNKNOWN:
    return FileType::Unknown;
  default:
    return FileType::Other;
  }
}

/**
 * @brief Esegue mapping di default con struct stat. (OS POSIX)
 */
[[nodiscard]] inline FileType type_from_mode(mode_t m) noexcept {
  if (S_ISREG(m))
    return FileType::Regular;
  if (S_ISDIR(m))
    return FileType::Directory;
  if (S_ISLNK(m))
    return FileType::Symlink;
  return FileType::Other;
}

/**
 * @brief Copia i metadata POSIX di `fstatat` nella entry di sistema. (OS POSIX)
 */
inline void fill_from_stat(FileEntry &e, const struct stat &st) noexcept {
  e.size = static_cast<std::uintmax_t>(st.st_size);
  e.mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
  e.mtime_nsec = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
  e.mode = static_cast<std::uint32_t>(st.st_mode);
  e.uid = static_cast<std::uint32_t>(st.st_uid);
  e.gid = static_cast<std::uint32_t>(st.st_gid);
  e.nlink = static_cast<std::uint32_t>(st.st_nlink);
  e.type = type_from_mode(st.st_mode);
  e.has_details = true;
}
#else
/**
 * @brief Copia i metadata Windows di `WIN32_FIND_DATAW` nella entry di sistema.
 * (OS Win32)
 */
inline void fill_from_find_data(FileEntry &e,
                                const WIN32_FIND_DATAW &data) noexcept {
  ULARGE_INTEGER sz, time;
  sz.LowPart = data.nFileSizeLow;
  sz.HighPart = data.nFileSizeHigh;
  e.size = static_cast<std::uintmax_t>(sz.QuadPart);

  // Windows usa epoch dal 1601 in intervalli da 100ns
  time.LowPart = data.ftLastWriteTime.dwLowDateTime;
  time.HighPart = data.ftLastWriteTime.dwHighDateTime;

  // Conversione approssimata da filetime Win32 a timestamp UNIX
  e.mtime_sec = (time.QuadPart / 10000000ULL) - 11644473600ULL;
  e.mtime_nsec = 0; // tralasciamo la parte micro

  if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    e.type = FileType::Directory;
  } else if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    e.type = FileType::Symlink;
  } else {
    e.type = FileType::Regular;
  }

  e.has_details = true;
}

/**
 * @brief Helper per convertire il formato String Wide in String u8/ASCII (OS
 * Win32)
 */
inline std::string utf16_to_utf8(const std::wstring &wstr) {
  if (wstr.empty())
    return std::string();
  int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(),
                                        NULL, 0, NULL, NULL);
  std::string strTo(static_cast<size_t>(size_needed), 0);
  WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0],
                      size_needed, NULL, NULL);
  return strTo;
}

inline std::wstring utf8_to_utf16(const std::string &str) {
  if (str.empty())
    return std::wstring();
  int size_needed =
      MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
  std::wstring wstrTo(static_cast<size_t>(size_needed), 0);
  MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0],
                      size_needed);
  return wstrTo;
}
#endif

/**
 * @brief Helper portabile che concatena due branch del percorso verificando gli
 * Slash path corretti.
 */
[[nodiscard]] inline std::string join_path(std::string_view base,
                                           std::string_view name) {
  std::string r;
  r.reserve(base.size() + 1 + name.size());
  r.append(base);
#ifdef _WIN32
  if (!r.empty() && r.back() != '\\' && r.back() != '/')
    r.push_back('/'); // normalizziamo a forward slash
#else
  if (!r.empty() && r.back() != '/')
    r.push_back('/');
#endif
  r.append(name);
  return r;
}

/**
 * @brief Pulisce l'estremita terminale del root target (elimina gli slash
 * eccedenti a coda).
 */
[[nodiscard]] inline std::string normalize_root(std::string p) {
#ifdef _WIN32
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
#else
  while (p.size() > 1 && p.back() == '/')
#endif
    p.pop_back();
  return p;
}

/**
 * @brief Comparatore interno per ordinare DirectoryPaths.
 *
 * L'ordine è una visita in profondità DFS, per simulare la stampata sequenziale
 * logica. Un padre viene sempre stampato prima del discendente (a prescindere
 * da `reverse`).
 */
[[nodiscard]] inline bool path_less(const std::string &a, const std::string &b,
                                    bool reverse) noexcept {
  const std::size_t n = std::min(a.size(), b.size());
  std::size_t i = 0;
  while (i < n && a[i] == b[i])
    ++i;

  if (i == n) {
    if (a.size() == b.size())
      return false;
    const bool a_shorter = a.size() < b.size();
    const std::string &shorter = a_shorter ? a : b;
    const std::string &longer = a_shorter ? b : a;
    if (longer[n] == '/' || shorter.back() == '/')
      return a_shorter;
    return reverse ? !a_shorter : a_shorter;
  }

  const auto map = [](char c) noexcept -> int {
    return c == '/' ? 0 : static_cast<unsigned char>(c);
  };
  const int ca = map(a[i]);
  const int cb = map(b[i]);
  return reverse ? ca > cb : ca < cb;
}

/**
 * @brief Confronta i nomi per il sorting alfabetico.
 */
[[nodiscard]] inline int
compare_names(const std::string &a, const std::string &b, bool icase) noexcept {
  if (!icase)
    return a.compare(b);
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const int ca = std::tolower(static_cast<unsigned char>(a[i]));
    const int cb = std::tolower(static_cast<unsigned char>(b[i]));
    if (ca != cb)
      return ca < cb ? -1 : 1;
  }
  if (a.size() != b.size())
    return a.size() < b.size() ? -1 : 1;
  return a.compare(b);
}

/**
 * @struct EntryLess
 * @brief Functor di smistamento per std::sort iniettato sui batch. Supporta
 * Name, Size, Time + Reverse.
 */
struct EntryLess {
  SortBy by;
  bool reverse;
  bool icase;

  [[nodiscard]] bool natural(const FileEntry &a,
                             const FileEntry &b) const noexcept {
    if (by == SortBy::Size && a.size != b.size)
      return a.size > b.size;
    if (by == SortBy::Time) {
      if (a.mtime_sec != b.mtime_sec)
        return a.mtime_sec > b.mtime_sec;
      if (a.mtime_nsec != b.mtime_nsec)
        return a.mtime_nsec > b.mtime_nsec;
    }
    return compare_names(a.name, b.name, icase) < 0;
  }

  [[nodiscard]] bool operator()(const FileEntry &a,
                                const FileEntry &b) const noexcept {
    return reverse ? natural(b, a) : natural(a, b);
  }
};

/**
 * @brief Formatta i byte in K, M, G, T.
 */
[[nodiscard]] inline std::string format_size(std::uintmax_t bytes) {
  constexpr std::string_view units[] = {"B", "K", "M", "G", "T", "P"};
  if (bytes < 1024)
    return std::to_string(bytes);
  double size = static_cast<double>(bytes);
  int unit_idx = 0;
  while (size >= 1024.0 && unit_idx < 5) {
    size /= 1024.0;
    ++unit_idx;
  }
  return std::format("{:.1f}{}", size, units[unit_idx]);
}

/**
 * @brief Costruisce la stringa dei permessi (es. "drwxr-xr-x").
 */
[[nodiscard]] inline std::string format_mode(std::uint32_t m) {
#ifdef _WIN32
  // Implementazione stub o approssimazione di base su Windows.
  // Poichè non usa i chmod POSIX, non riporteremo stringhe perfette.
  std::string r(10, '-');
  if (m == 0)
    return r; // File Entry generata tramite FindNext, usiamo defaults Win.
#else
  std::string r(10, '-');
  if (S_ISDIR(m))
    r[0] = 'd';
  else if (S_ISLNK(m))
    r[0] = 'l';
  else if (S_ISCHR(m))
    r[0] = 'c';
  else if (S_ISBLK(m))
    r[0] = 'b';
  else if (S_ISFIFO(m))
    r[0] = 'p';
  else if (S_ISSOCK(m))
    r[0] = 's';

  constexpr std::uint32_t bits[9] = {S_IRUSR, S_IWUSR, S_IXUSR,
                                     S_IRGRP, S_IWGRP, S_IXGRP,
                                     S_IROTH, S_IWOTH, S_IXOTH};
  constexpr char sym[3] = {'r', 'w', 'x'};
  for (int i = 0; i < 9; ++i)
    if (m & bits[i])
      r[static_cast<std::size_t>(i) + 1] = sym[i % 3];

  if (m & S_ISUID)
    r[3] = (m & S_IXUSR) ? 's' : 'S';
  if (m & S_ISGID)
    r[6] = (m & S_IXGRP) ? 's' : 'S';
  if (m & S_ISVTX)
    r[9] = (m & S_IXOTH) ? 't' : 'T';
  return r;
#endif
  return std::string(10, '-');
}

/**
 * @brief Formatta un unix epoch TimeStamp in human string come nel tool `ls`.
 */
[[nodiscard]] inline std::string format_time(std::int64_t sec,
                                             std::int64_t now) {
  const std::time_t t = static_cast<std::time_t>(sec);
  std::tm tm{};
#ifdef _WIN32
  if (localtime_s(&tm, &t) != 0)
    return "?";
#else
  if (localtime_r(&t, &tm) == nullptr)
    return "?";
#endif
  constexpr std::int64_t half_year = 182LL * 24 * 3600;
  const bool recent = sec >= now - half_year && sec <= now + 3600;
  char buf[48];
  const std::size_t n =
      std::strftime(buf, sizeof buf, recent ? "%b %e %H:%M" : "%b %e  %Y", &tm);
  return std::string(buf, n);
}

/**
 * @class OutBuffer
 * @brief I/O Stream personalizzato, limita l'overhead di IO flush. (64 KiB
 * Blocks)
 */
class OutBuffer {
public:
  explicit OutBuffer(std::FILE *f) : f_(f) { buf_.reserve(kFlushAt + 4096); }
  OutBuffer(const OutBuffer &) = delete;
  OutBuffer &operator=(const OutBuffer &) = delete;
  ~OutBuffer() { flush(); }

  void put(std::string_view s) {
    buf_.append(s);
    maybe_flush();
  }

  template <typename... Args>
  void print(std::format_string<Args...> fmt, Args &&...args) {
    std::format_to(std::back_inserter(buf_), fmt, std::forward<Args>(args)...);
    maybe_flush();
  }

  void flush() {
    if (!buf_.empty()) {
      std::fwrite(buf_.data(), 1, buf_.size(), f_);
      buf_.clear();
    }
  }

private:
  static constexpr std::size_t kFlushAt = 64 * 1024;
  void maybe_flush() {
    if (buf_.size() >= kFlushAt)
      flush();
  }
  std::FILE *f_;
  std::string buf_{};
};

/**
 * @struct ErrorSink
 * @brief Interfaccia thread-safe per accumulare diagnostici non bloccanti del
 * motore senza bloccare i thread.
 */
struct ErrorSink {
  std::mutex m{};
  std::vector<std::string> messages{};
  void add(std::string s) {
    std::lock_guard<std::mutex> lk(m);
    messages.push_back(std::move(s));
  }
};

/**
 * @class IdNames
 * @brief Cache Resolver Single-thread. Trasforma ID numerici (o hash SID Win32)
 * in alias stringa utente/gruppo.
 */
class IdNames {
public:
  const std::string &user(std::uint32_t id, const std::string &winName = "") {
    (void)id;
#ifdef _WIN32
    return winName.empty() ? fallback_ : winName;
#else
    auto it = users_.find(id);
    if (it != users_.end())
      return it->second;
    const passwd *pw = getpwuid(id);
    return users_
        .emplace(id, pw ? std::string(pw->pw_name) : std::to_string(id))
        .first->second;
#endif
  }
  const std::string &group(std::uint32_t id, const std::string &winGroup = "") {
    (void)id;
#ifdef _WIN32
    return winGroup.empty() ? fallback_ : winGroup;
#else
    auto it = groups_.find(id);
    if (it != groups_.end())
      return it->second;
    const struct group *gr = getgrgid(id);
    return groups_
        .emplace(id, gr ? std::string(gr->gr_name) : std::to_string(id))
        .first->second;
#endif
  }

private:
  std::unordered_map<std::uint32_t, std::string> users_{};
  std::unordered_map<std::uint32_t, std::string> groups_{};
  std::string fallback_{"Unknown"};
};

/**
 * @struct GitIgnoreNode
 * @brief Node tree adibito a calcolare l'ereditarietà di direttive ignore
 * innestate in subfolder.
 */
struct GitIgnoreNode {
  std::vector<std::string> patterns;
  std::shared_ptr<GitIgnoreNode> parent;

  bool matches(std::string_view name, bool is_dir) const {
    if (name == ".git")
      return true; // Ignora sempre la cartella .git

    for (const auto &p : patterns) {
      if (p.empty())
        continue;
      bool dir_only = (p.back() == '/');
      if (dir_only && !is_dir)
        continue;

      std::string_view rule =
          dir_only ? std::string_view(p).substr(0, p.size() - 1) : p;
      if (rule.starts_with('*')) {
        if (name.ends_with(rule.substr(1)))
          return true;
      } else if (rule.ends_with('*')) {
        if (name.starts_with(rule.substr(0, rule.size() - 1)))
          return true;
      } else {
        if (name == rule)
          return true;
      }
    }
    if (parent)
      return parent->matches(name, is_dir);
    return false;
  }

  static std::shared_ptr<GitIgnoreNode>
  load(const std::string &path, std::shared_ptr<GitIgnoreNode> parent) {
    auto node = std::make_shared<GitIgnoreNode>();
    node->parent = parent;
    std::ifstream file(path + "/.gitignore");
    if (!file)
      return node;

    std::string line;
    while (std::getline(file, line)) {
      if (line.empty() || line[0] == '#')
        continue;
      // Trim right spaces
      line.erase(
          std::find_if(line.rbegin(), line.rend(),
                       [](unsigned char ch) { return !std::isspace(ch); })
              .base(),
          line.end());
      if (!line.empty())
        node->patterns.push_back(line);
    }
    return node;
  }
};

} // namespace detail

/**
 * @class LSer
 * @brief Motore principale per la scansione multithread del file system.
 */
class LSer {
public:
  /**
   * @brief Costruisce il motore di scansione istanziando path target e flag
   * base.
   * @param path Il percorso radice da analizzare.
   * @param flags Struttura con i parametri di configurazione.
   */
  explicit LSer(std::string_view path = ".", ListFlags flags = {})
      : flags_(flags), target_(path.empty() ? std::string_view{"."} : path) {}

  // Fluent API per configurazione run-time (Chainable)
  LSer &with_hidden(bool v = true) & noexcept {
    flags_.show_hidden = v;
    return *this;
  }
  LSer &with_long_format(bool v = true) & noexcept {
    flags_.long_format = v;
    return *this;
  }
  LSer &with_human_readable(bool v = true) & noexcept {
    flags_.human_readable = v;
    return *this;
  }
  LSer &with_recursive(bool v = true) & noexcept {
    flags_.recursive = v;
    return *this;
  }
  LSer &with_tree_view(bool v = true) & noexcept {
    flags_.tree_view = v;
    return *this;
  }
  LSer &with_sort(SortBy s, bool reverse = false) & noexcept {
    flags_.sort_by = s;
    flags_.reverse_sort = reverse;
    return *this;
  }
  LSer &with_max_depth(int d) & noexcept {
    flags_.max_depth = d;
    return *this;
  }
  LSer &with_threads(unsigned n) & noexcept {
    flags_.threads = n;
    return *this;
  }
  LSer &with_time_exec(bool v = true) & noexcept {
    flags_.time_exec = v;
    return *this;
  }
  LSer &set_path(std::string_view path) & {
    target_.assign(path.empty() ? std::string_view{"."} : path);
    return *this;
  }

  /**
   * @brief Esegue esclusivamente il caricamento e lo smistamento dati in
   * memoria senza renderli a IOStream. Ideale per fare Stress Testing con
   * Google Benchmark.
   */
  void run_engine_only() { load_entries(); }

  /**
   * @brief Esegue la scansione completa e stampa l'output bufferizzato sui
   * flussi specificati.
   * @param out File handler target su cui eseguire fwrite Standard-Out. (es.
   * `stdout`)
   * @param err File handler target su cui eseguire l'imissione diagnostica di
   * warnings e tempo. (es. `stderr`)
   * @return 0 in caso di successo totale, 1 se ci sono stati errori parziali
   * (Permission denied), 2 per path root invalido.
   */
  int execute(std::FILE *out = stdout, std::FILE *err = stderr) {
    const auto t0 = std::chrono::steady_clock::now();
    load_entries();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();

    for (const auto &msg : errors_)
      std::println(err, "{}", msg);
    if (root_failed_)
      return 2;

    {
      detail::OutBuffer buf(out);
      if (flags_.tree_view)
        print_tree(buf);
      else if (flags_.long_format)
        print_long(buf);
      else
        print_simple(buf);

      if (flags_.summary && !flags_.tree_view)
        buf.print("\nNumero totale cartelle/file: {}\n", count());
    }

    if (flags_.time_exec)
      std::println(err, "[Time elapsed: {:.3f} ms]", elapsed_ms);
    return errors_.empty() ? 0 : 1;
  }

  [[nodiscard]] const std::vector<DirectoryResult> &
  directories() const noexcept {
    return directories_;
  }

  [[nodiscard]] const std::vector<std::string> &errors() const noexcept {
    return errors_;
  }

  /**
   * @brief Ritorna il count esatto di quanti file fisici e symlinks ha parsato
   * l'engine nel filesystem.
   */
  [[nodiscard]] std::size_t count() const noexcept {
    std::size_t total = 0;
    for (const auto &dir : directories_)
      total += dir.entries.size();
    return total;
  }

  [[nodiscard]] std::vector<fs::path> all_paths() const {
    std::vector<fs::path> all;
    all.reserve(count());
    for (const auto &dir : directories_) {
      for (const auto &e : dir.entries) {
        all.emplace_back(root_is_file_ ? e.name
                                       : detail::join_path(dir.path, e.name));
      }
    }
    return all;
  }

private:
  struct Task {
    std::string path{};
    int depth{0};
    std::shared_ptr<detail::GitIgnoreNode> git_node{nullptr};
  };

  using DirIndex =
      std::unordered_map<std::string_view, const DirectoryResult *>;

  ListFlags flags_{};
  std::string target_{};
  std::vector<DirectoryResult> directories_{};
  std::vector<std::string> errors_{};
  bool root_failed_{false};
  bool root_is_file_{false};

  [[nodiscard]] bool recursive() const noexcept {
    return flags_.recursive || flags_.tree_view;
  }

  [[nodiscard]] bool need_details() const noexcept {
    return flags_.long_format || flags_.sort_by != SortBy::Name;
  }

  /**
   * @brief Scanner atomico eseguito dai Worker su Singolo ramo per non bloccare
   * il mutex genitore. Utilizza API POSIX o Win32 a seconda del build System.
   */
  void scan_directory(Task task, std::vector<Task> &subdirs,
                      std::vector<DirectoryResult> &sink,
                      detail::ErrorSink &errors) const {
    using namespace detail;

    // Processamento del .gitignore
    if (flags_.git_ignore) {
      if (fs::exists(task.path + "/.gitignore")) {
        task.git_node = GitIgnoreNode::load(task.path, task.git_node);
      }
    }

#ifdef _WIN32
    // ---- IMPLEMENTAZIONE WIN32 (FindFirstFile API) ----
    std::string search_path = join_path(task.path, "*");
    std::wstring wsearch = utf8_to_utf16(search_path);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(wsearch.c_str(), &fd);

    if (hFind == INVALID_HANDLE_VALUE) {
      errors.add(
          std::format("lser: impossibile aprire la cartella '{}'", task.path));
      return;
    }

    // const bool details = need_details();
    // const bool want_links = flags_.long_format || flags_.tree_view;
    const bool can_descend = recursive() && (flags_.max_depth < 0 ||
                                             task.depth + 1 < flags_.max_depth);

    DirectoryResult res;
    res.path = std::move(task.path);
    res.entries.reserve(16);

    do {
      std::string nm = utf16_to_utf8(fd.cFileName);

      if (nm == "." || nm == "..")
        continue;
      if (!flags_.show_hidden && nm[0] == '.')
        continue;

      FileEntry e;
      bool is_dir_fast = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

      if (flags_.git_ignore && task.git_node) {
        if (task.git_node->matches(nm, is_dir_fast))
          continue;
      }

      if (flags_.dirs_only && !is_dir_fast)
        continue;

      e.name = nm;

      // La Win32 struct ci regala già quasi tutti i metadata dello Stat
      fill_from_find_data(e, fd);

      if (can_descend && is_dir_fast) {
        subdirs.push_back(
            Task{join_path(res.path, e.name), task.depth + 1, task.git_node});
      }

      res.entries.push_back(std::move(e));
    } while (FindNextFileW(hFind, &fd) != 0);

    FindClose(hFind);

#else
    // ---- IMPLEMENTAZIONE ORIGINALE POSIX ----
    DIR *raw = ::opendir(task.path.c_str());
    if (raw == nullptr) {
      errors.add(std::format("lser: impossibile aprire la cartella '{}': {}",
                             task.path, std::strerror(errno)));
      return;
    }
    const std::unique_ptr<DIR, int (*)(DIR *)> dir(raw, &::closedir);
    const int dfd = ::dirfd(raw);

    const bool details = need_details();
    const bool want_links = flags_.long_format || flags_.tree_view;
    const bool can_descend = recursive() && (flags_.max_depth < 0 ||
                                             task.depth + 1 < flags_.max_depth);

    DirectoryResult res;
    res.path = std::move(task.path);
    res.entries.reserve(16);

    for (;;) {
      errno = 0;
      const dirent *de = ::readdir(raw);
      if (de == nullptr)
        break;

      const char *nm = de->d_name;
      if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
        continue;
      if (!flags_.show_hidden && nm[0] == '.')
        continue;

      FileEntry e;
      e.type = type_from_dtype(de->d_type);
      bool is_dir_fast = (e.type == FileType::Directory);

      // Check git ignore in tempo reale
      if (flags_.git_ignore && task.git_node) {
        if (task.git_node->matches(nm, is_dir_fast))
          continue;
      }

      if (details || e.type == FileType::Unknown) {
        struct stat st{};
        if (::fstatat(dfd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
          errors.add(std::format("lser: impossibile leggere '{}': {}",
                                 join_path(res.path, nm),
                                 std::strerror(errno)));
          continue;
        }
        fill_from_stat(e, st);
      }

      if (flags_.dirs_only && e.type != FileType::Directory)
        continue;

      e.name.assign(nm);

      if (e.type == FileType::Symlink && want_links) {
        char buf[PATH_MAX];
        const ssize_t n = ::readlinkat(dfd, nm, buf, sizeof buf);
        if (n >= 0) {
          res.links.emplace_back(buf, static_cast<std::size_t>(n));
          e.link_idx = static_cast<std::int32_t>(res.links.size() - 1);
        }
      }

      if (can_descend && e.type == FileType::Directory)
        subdirs.push_back(
            Task{join_path(res.path, e.name), task.depth + 1, task.git_node});

      res.entries.push_back(std::move(e));
    }
    if (errno != 0) {
      errors.add(std::format("lser: errore leggendo '{}': {}", res.path,
                             std::strerror(errno)));
    }
#endif // _WIN32

    std::sort(
        res.entries.begin(), res.entries.end(),
        EntryLess{flags_.sort_by, flags_.reverse_sort, flags_.ignore_case});
    sink.push_back(std::move(res));
  }

  void run_workers(Task root, detail::ErrorSink &errors,
                   std::vector<DirectoryResult> &out) const {
    struct Shared {
      std::mutex m{};
      std::condition_variable cv{};
      std::vector<Task> stack{};
      std::size_t pending{0};
    } sh;
    sh.stack.push_back(std::move(root));
    sh.pending = 1;

    const unsigned n = flags_.threads != 0
                           ? flags_.threads
                           : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::vector<DirectoryResult>> results(n);

    auto worker = [&](unsigned id) {
      std::vector<Task> subdirs;
      for (;;) {
        Task task;
        {
          std::unique_lock<std::mutex> lk(sh.m);
          sh.cv.wait(lk, [&] { return !sh.stack.empty() || sh.pending == 0; });
          if (sh.stack.empty())
            return;
          task = std::move(sh.stack.back());
          sh.stack.pop_back();
        }

        subdirs.clear();
        try {
          scan_directory(std::move(task), subdirs, results[id], errors);
        } catch (const std::exception &ex) {
          errors.add(std::format("lser: errore interno: {}", ex.what()));
          subdirs.clear();
        }

        const std::size_t spawned = subdirs.size();
        bool all_done = false;
        {
          std::lock_guard<std::mutex> lk(sh.m);
          for (auto &s : subdirs)
            sh.stack.push_back(std::move(s));
          sh.pending += spawned;
          --sh.pending;
          all_done = sh.pending == 0;
        }
        if (all_done || spawned > 1)
          sh.cv.notify_all();
        else if (spawned == 1)
          sh.cv.notify_one();
      }
    };

    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned i = 1; i < n; ++i) {
      try {
        pool.emplace_back(worker, i);
      } catch (const std::system_error &) {
        break;
      }
    }
    worker(0);
    for (auto &t : pool)
      t.join();

    std::size_t total = 0;
    for (const auto &r : results)
      total += r.size();
    out.reserve(total);
    for (auto &r : results)
      std::move(r.begin(), r.end(), std::back_inserter(out));
  }

  void sort_directories() {
    const auto less = [rev = flags_.reverse_sort](const DirectoryResult &a,
                                                  const DirectoryResult &b) {
      return detail::path_less(a.path, b.path, rev);
    };
#ifdef LSER_HAS_TBB
    if (directories_.size() > 20000) {
      std::sort(std::execution::par, directories_.begin(), directories_.end(),
                less);
      return;
    }
#endif
    std::sort(directories_.begin(), directories_.end(), less);
  }

  void load_entries() {
    directories_.clear();
    errors_.clear();
    root_failed_ = false;
    root_is_file_ = false;

    // Stat / File Checks
#ifdef _WIN32
    std::wstring wtarget = detail::utf8_to_utf16(target_);
    DWORD dwAttrib = GetFileAttributesW(wtarget.c_str());

    if (dwAttrib == INVALID_FILE_ATTRIBUTES) {
      errors_.push_back(
          std::format("lser: impossibile accedere a '{}'", target_));
      root_failed_ = true;
      return;
    }

    if (!(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
      FileEntry e;
      e.name = target_;
      e.size = 0; // Se occorre si prende tramite handle. (Benchmark ignora
                  // questa casistica root-file)
      DirectoryResult r;
      r.path = target_;
      r.entries.push_back(std::move(e));
      directories_.push_back(std::move(r));
      root_is_file_ = true;
      return;
    }
#else
    struct stat st{};
    if (::stat(target_.c_str(), &st) != 0) {
      errors_.push_back(std::format("lser: impossibile accedere a '{}': {}",
                                    target_, std::strerror(errno)));
      root_failed_ = true;
      return;
    }

    if (!S_ISDIR(st.st_mode)) {
      FileEntry e;
      e.name = target_;
      detail::fill_from_stat(e, st);
      DirectoryResult r;
      r.path = target_;
      r.entries.push_back(std::move(e));
      directories_.push_back(std::move(r));
      root_is_file_ = true;
      return;
    }
#endif

    Task root{detail::normalize_root(target_), 0, nullptr};
    detail::ErrorSink sink;

    if (!recursive()) {
      std::vector<Task> unused;
      scan_directory(std::move(root), unused, directories_, sink);
    } else {
      run_workers(std::move(root), sink, directories_);
    }

    errors_ = std::move(sink.messages);
    std::sort(errors_.begin(), errors_.end());
    sort_directories();
  }

  void put_name(detail::OutBuffer &out, const DirectoryResult &dir,
                const FileEntry &e) const {
    using namespace detail;
    std::string_view color{};
    if (flags_.color) {
      if (e.type == FileType::Directory)
        color = kBlue;
      else if (e.type == FileType::Symlink)
        color = kCyan;
      else if (e.type == FileType::Regular && e.has_details && (e.mode & 0111))
        color = kGreen;
    }
    if (color.empty()) {
      out.put(e.name);
    } else {
      out.put(color);
      out.put(e.name);
      out.put(kReset);
    }
    if (const std::string *target = dir.link_target(e))
      out.print(" -> {}", *target);
  }

  void print_simple(detail::OutBuffer &out) const {
    const bool headers =
        !root_is_file_ && (recursive() || flags_.force_headers);
    bool first = true;
    for (const auto &dir : directories_) {
      if (headers) {
        if (!first)
          out.put("\n");
        out.print("{}:\n", dir.path);
      }
      first = false;
      for (const auto &e : dir.entries) {
        put_name(out, dir, e);
        out.put("\n");
      }
    }
  }

  [[nodiscard]] std::string size_string(const FileEntry &e) const {
    return flags_.human_readable ? detail::format_size(e.size)
                                 : std::to_string(e.size);
  }

  void print_long(detail::OutBuffer &out) const {
    using namespace detail;
    ::tzset();
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    IdNames names;

    const bool headers =
        !root_is_file_ && (recursive() || flags_.force_headers);
    bool first = true;

    for (const auto &dir : directories_) {
      if (headers) {
        if (!first)
          out.put("\n");
        out.print("{}:\n", dir.path);
      }
      first = false;

      int w_nlink = 1, w_user = 1, w_group = 1, w_size = 1;
      for (const auto &e : dir.entries) {
        w_nlink =
            std::max(w_nlink, static_cast<int>(std::to_string(e.nlink).size()));
#ifdef _WIN32
        w_user = std::max(
            w_user, static_cast<int>(names.user(e.uid, e.win_user).size()));
        w_group = std::max(
            w_group, static_cast<int>(names.group(e.gid, e.win_group).size()));
#else
        w_user = std::max(w_user, static_cast<int>(names.user(e.uid).size()));
        w_group =
            std::max(w_group, static_cast<int>(names.group(e.gid).size()));
#endif
        w_size = std::max(w_size, static_cast<int>(size_string(e).size()));
      }

      for (const auto &e : dir.entries) {
#ifdef _WIN32
        out.print("{} {:>{}} {:<{}} {:<{}} {:>{}} {} ", format_mode(e.mode),
                  e.nlink, w_nlink, names.user(e.uid, e.win_user), w_user,
                  names.group(e.gid, e.win_group), w_group, size_string(e),
                  w_size, format_time(e.mtime_sec, now));
#else
        out.print("{} {:>{}} {:<{}} {:<{}} {:>{}} {} ", format_mode(e.mode),
                  e.nlink, w_nlink, names.user(e.uid), w_user,
                  names.group(e.gid), w_group, size_string(e), w_size,
                  format_time(e.mtime_sec, now));
#endif
        put_name(out, dir, e);
        out.put("\n");
      }
    }
  }

  void print_tree_node(detail::OutBuffer &out, const DirectoryResult &dir,
                       std::string &prefix, const DirIndex &index,
                       std::size_t &dirs, std::size_t &files) const {
    for (std::size_t i = 0; i < dir.entries.size(); ++i) {
      const FileEntry &e = dir.entries[i];
      const bool last = i + 1 == dir.entries.size();

      out.put(prefix);
      out.put(last ? "└── " : "├── ");
      put_name(out, dir, e);
      out.put("\n");

      if (e.is_directory()) {
        ++dirs;
        const std::string child = detail::join_path(dir.path, e.name);
        if (auto it = index.find(child); it != index.end()) {
          const std::size_t old = prefix.size();
          prefix += last ? "    " : "│   ";
          print_tree_node(out, *it->second, prefix, index, dirs, files);
          prefix.resize(old);
        }
      } else {
        ++files;
      }
    }
  }

  void print_tree(detail::OutBuffer &out) const {
    out.print("{}\n", target_);
    if (root_is_file_)
      return;

    DirIndex index;
    index.reserve(directories_.size() * 2);
    for (const auto &d : directories_)
      index.emplace(d.path, &d);

    const std::string root = detail::normalize_root(target_);
    std::size_t dirs = 0, files = 0;
    if (auto it = index.find(root); it != index.end()) {
      std::string prefix;
      print_tree_node(out, *it->second, prefix, index, dirs, files);
    }
    out.print("\n{} cartelle, {} file\n", dirs, files);
  }
};

} // namespace lser