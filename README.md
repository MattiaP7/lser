<div align="center">

# LSer

**Un clone di `ls` e `tree` in C++23, costruito attorno al parallelismo e alla località dei dati.**

![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.22%2B-064F8C?logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-informational)
![License](https://img.shields.io/badge/license-MIT-green)

</div>

---

LSer esplora un albero di directory con una pool di thread, raccoglie i risultati in **vettori piatti**
(niente `std::map`, niente nodi sparsi nell'heap) e stampa l'output in blocchi da 64 KiB.

| Piattaforma | Target                                  | Entry  | 1 thread | 8 thread    | Speedup |
| ----------- | --------------------------------------- | ------ | -------- | ----------- | ------- |
| Linux       | `/usr/include` (listing semplice)       | 30 573 | 48,1 ms  | **11,9 ms** | 4,04×   |
| Windows     | cartella di progetti (listing semplice) | 5 425  | 32,6 ms  | **7,8 ms**  | 4,18×   |

> Tempi del _motore_ (scansione + ordinamento in memoria, page cache calda), non della stampa.
> Metodologia, limiti e tutti i numeri sono in [BENCHMARKS.md](BENCHMARKS.md).

## Feature

- **Multithreading**: pool di worker con pila condivisa e terminazione deterministica; il thread chiamante lavora
  come gli altri. Numero di thread configurabile (`-j N`).
- **Pochi syscall**: su Linux `opendir`/`readdir` + `fstatat(AT_SYMLINK_NOFOLLOW)` solo quando servono dettagli
  (`-l`, `-S`, `-t`); altrimenti il tipo arriva da `d_type`. Su Windows `FindFirstFileW` restituisce già i metadati.
- **Dati piatti, poche allocazioni**: `FileEntry` contiene solo il nome; il path della cartella è salvato una volta
  sola. I nomi corti restano nella small-string optimization.
- **Ordinamento**: per nome (bytewise o case-insensitive), dimensione, data; ordine inverso; spareggi deterministici.
  Ogni worker ordina la cartella che ha appena letto.
- **Tree view**: la gerarchia viene ricostruita dalla lista piatta con un indice hash costruito una sola volta.
- **Supporto `.gitignore`** (`-g`): le regole si ereditano dalle cartelle padre; `.git` è sempre escluso.
  Vedi [limiti](#limitazioni-note) per i pattern supportati.
- **Filtri**: solo directory (`-D`), profondità massima (`-L N`), file nascosti (`-a`).
- **Symlink sicuri**: non vengono mai seguiti, quindi niente loop. In `-l` e nel tree si vede il `-> target`.
- **Errori come `ls`**: permessi negati e simili vanno su stderr, la scansione continua; exit code `0/1/2`.
- **Output curato**: formato lungo con permessi (setuid/setgid/sticky), link, utente, gruppo, ora locale, colori ANSI.
- **Testato**: suite di correttezza (`ctest`), eseguibile anche con ThreadSanitizer e ASan/UBSan.

## Requisiti

| Cosa              | Note                                                                                  |
| ----------------- | ------------------------------------------------------------------------------------- |
| Sistema           | Linux (POSIX) o Windows (MSVC / MinGW-w64)                                            |
| Compilatore C++23 | con supporto a `<print>` e `<format>`: GCC 14+, MSVC 2022 17.7+, Clang con libc++ 17+ |
| CMake ≥ 3.22      |                                                                                       |
| TBB               | _opzionale_: ordina in parallelo l'elenco cartelle quando sono più di 20 000          |
| Google Benchmark  | _opzionale_: solo per `bench_lser`                                                    |

```sh
# Arch
sudo pacman -S base-devel cmake onetbb benchmark
# Debian/Ubuntu
sudo apt install build-essential cmake libtbb-dev libbenchmark-dev
```

Con GCC il progetto linka automaticamente `stdc++exp` (necessaria per `std::print`).

## Build

```sh
git clone https://github.com/MattiaP7/lser.git
cd lser
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/lser --help

# Installazione (opzionale)
# Di default su Linux installa in /usr/local/bin. Puoi cambiare prefisso con --prefix <path>
sudo cmake --install build
```

Il build type di default è `Release` (`-O3` su GCC/Clang).

| Opzione CMake           | Default | Effetto                                                       |
| ----------------------- | ------- | ------------------------------------------------------------- |
| `LSER_USE_TBB`          | ON      | ordinamento parallelo dell'elenco cartelle (se TBB è trovato) |
| `LSER_BUILD_TESTS`      | ON      | compila `lser_tests`                                          |
| `LSER_BUILD_BENCHMARKS` | ON      | compila `bench_lser` (richiede Google Benchmark)              |
| `LSER_NATIVE`           | OFF     | `-march=native` / `/arch:AVX2` (binario non portabile)        |
| `LSER_SANITIZE`         | OFF     | ASan + UBSan                                                  |
| `LSER_TSAN`             | OFF     | ThreadSanitizer (non combinabile con `LSER_SANITIZE`)         |

```sh
cmake -S . -B build-tsan -DLSER_TSAN=ON && cmake --build build-tsan -j && ctest --test-dir build-tsan
```

## Uso

```
lser [OPZIONI] [PERCORSO...]
```

Le opzioni brevi si possono combinare (`-lSr`). `--` termina le opzioni. Con più percorsi, ognuno ha la sua intestazione.

| Flag                            | Significato                                                    |
| ------------------------------- | -------------------------------------------------------------- |
| `-a`                            | mostra i nascosti                                              |
| `-l`                            | formato lungo                                                  |
| `-h`                            | size leggibili (con `-l`)                                      |
| `-r`                            | ricorsivo (i symlink non vengono seguiti)                      |
| `-T`                            | albero (implica `-r`)                                          |
| `-L N`                          | profondità massima, `1` = solo i figli diretti (con `-r`/`-T`) |
| `-D`                            | solo directory                                                 |
| `-g`, `--git-ignore`            | salta ciò che è escluso dai `.gitignore`                       |
| `-S` / `-t`                     | ordina per dimensione / data (più recenti prima)               |
| `-R`                            | ordine inverso                                                 |
| `-i`                            | nomi case-insensitive                                          |
| `-j N`                          | numero di thread (default: tutti i core)                       |
| `-E`                            | tempo del motore su stderr                                     |
| `--color[=auto\|always\|never]` | colori (`auto` rispetta TTY e `NO_COLOR`)                      |
| `--summary`                     | totale cartelle/file (il tree lo stampa sempre)                |
| `--help`                        | mostra l'aiuto                                                 |

```sh
lser -la /usr/include                       # dettagli e nascosti
lser -aT -L 2 ~/progetti                    # albero a 2 livelli, con nascosti
lser -aT -g ~/progetti/app                  # albero che rispetta .gitignore (niente node_modules, build...)
lser -lSr /var/log                          # ricorsivo, i file più grandi prima
lser -lhtr ~/progetti                       # più recenti prima, size leggibili
lser -DT -L 3 .                             # solo la struttura delle cartelle
lser -E -j 4 -r /usr/include > /dev/null    # tempo del motore con 4 thread
lser --color=always -T . | less -R
```

Esempio di output (`lser -aT`):

```
/tmp/t1
├── .hidden
├── a.txt
├── empty
├── link_a -> a.txt
├── loop -> /tmp/t1
└── sub
    ├── b.txt
    └── deep

3 cartelle, 5 file
```

**Codici di uscita**: `0` ok · `1` errori parziali (es. permessi) · `2` opzione non valida o percorso non accessibile.

## Limitazioni note

- **Windows è un backend più giovane**: owner e gruppo compaiono come `Unknown` e i permessi sono un mapping fittizio
  (`----------`) perché Windows non ha uid/gid/mode POSIX.
- **`.gitignore` semplificato**: si confronta solo il _nome_ del file/cartella con pattern esatti (`foo`), con
  prefisso/suffisso jolly (`*.o`, `tmp*`) e con `/` finale (solo directory). Non sono supportati negazioni (`!`),
  `**`, pattern ancorati a un percorso (`build/out`) e jolly in mezzo al nome. Non è un sostituto di `git status`.
- L'ordinamento per nome è bytewise (nessuna collation di locale): usa `-i` per ignorare le maiuscole.
- `.` e `..` non vengono mai mostrati. Nel tree i symlink contano come file.
- A differenza di `ls`, `-r` è "ricorsivo" e `-R` è "reverse".

## Documentazione

- [ARCHITECTURE.md](ARCHITECTURE.md): scelte di design e compromessi
- [BENCHMARKS.md](BENCHMARKS.md): come riprodurre e interpretare i benchmark

## Licenza

[MIT](LICENSE)
