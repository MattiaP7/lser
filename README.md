<div align="center">

# LSer

**Un clone di `ls` e `tree` in C++23, costruito attorno al parallelismo e alla località dei dati.**

![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.22%2B-064F8C?logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux-informational)
![License](https://img.shields.io/badge/license-MIT-green)

</div>

---

LSer esplora un albero di directory con una pool di thread, raccoglie i risultati in **vettori piatti**
(niente `std::map`, niente nodi sparsi nell'heap) e produce l'output in blocchi da 64 KiB.
Su `/usr/include` (30573 entry) il motore completo (scansione, `stat`, ordinamento) impiega
**~12 ms con 8 thread** contro ~48 ms con un thread, con un picco di heap di **~5,8 MB**.

> I numeri si riferiscono al *motore* (scansione + ordinamento in memoria, page cache calda).
> La stampa su terminale è un costo separato: vedi [BENCHMARKS.md](BENCHMARKS.md).

## Feature

- **Multithreading**: pool di worker con coda condivisa e terminazione deterministica; il thread
  chiamante lavora come gli altri. Numero di thread configurabile (`-j N`).
- **Una sola `stat` per file**: `opendir`/`readdir` + `fstatat(AT_SYMLINK_NOFOLLOW)`. Quando bastano nome e tipo
  (listing semplice, tree) non viene fatta nessuna `stat`: il tipo arriva da `d_type`.
- **Dati piatti, poche allocazioni**: `FileEntry` (80 byte) contiene solo il nome; il path della cartella è salvato una
  volta sola. Nomi corti restano nella small-string optimization.
- **Ordinamento avanzato**: per nome (bytewise o case-insensitive), dimensione, data; ordine inverso; spareggi
  deterministici. Ogni worker ordina la cartella che ha appena letto.
- **Tree view**: la gerarchia viene ricostruita dalla lista piatta con un indice hash costruito una sola volta
  (lookup O(1) per cartella, costruzione complessiva O(n)).
- **Sicuro con i symlink**: non vengono mai seguiti, quindi niente loop infiniti. In `-l` e nel tree si vede il `-> target`.
- **Errori come `ls`**: permessi negati e simili vanno su stderr, l'esecuzione continua; exit code `0/1/2`.
- **Output a blocchi**: formato lungo con permessi (setuid/setgid/sticky), link, utente, gruppo, ora locale, colori ANSI.
- **Testato**: suite di correttezza (`ctest`), eseguita anche con ThreadSanitizer e ASan/UBSan.

## Requisiti

| Cosa | Note |
|---|---|
| Linux | usa API POSIX (`fstatat`, `readlinkat`, ...) |
| Compilatore C++23 | testato con GCC 13.3 (serve `<format>`; **non** serve `<print>`) |
| CMake ≥ 3.22 | |
| TBB | *opzionale*: ordinamento parallelo dell'elenco cartelle quando sono più di 20 000 |
| Google Benchmark | *opzionale*: solo per `bench_lser` |

```sh
# Arch
sudo pacman -S base-devel cmake onetbb benchmark
# Debian/Ubuntu
sudo apt install build-essential cmake libtbb-dev libbenchmark-dev
```

## Build

```sh
git clone https://github.com/<tuo-utente>/lser.git
cd lser
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/lser --help
```

Opzioni CMake: `LSER_USE_TBB` (ON), `LSER_BUILD_TESTS` (ON), `LSER_BUILD_BENCHMARKS` (ON),
`LSER_NATIVE` (OFF, `-march=native`), `LSER_SANITIZE` (OFF, ASan+UBSan), `LSER_TSAN` (OFF, ThreadSanitizer).

```sh
# Sanitizer
cmake -S . -B build-tsan -DLSER_TSAN=ON && cmake --build build-tsan -j && ctest --test-dir build-tsan
```

## Uso

```
lser [OPZIONI] [PERCORSO...]
```

| Flag | Significato |
|---|---|
| `-a` | mostra i nascosti |
| `-l` | formato lungo |
| `-h` | size leggibili (con `-l`) |
| `-r` | ricorsivo |
| `-T` | albero (implica `-r`) |
| `-L N` | profondità massima (con `-r`/`-T`) |
| `-D` | solo directory |
| `-S` / `-t` | ordina per dimensione / data |
| `-R` | ordine inverso |
| `-i` | nomi case-insensitive |
| `-j N` | numero di thread |
| `-E` | tempo del motore su stderr |
| `--color[=auto\|always\|never]` | colori |
| `--summary` | totale cartelle/file |

```sh
lser -la /usr/include          # dettagli e nascosti
lser -aT -L 2 ~/progetti       # albero a 2 livelli, con nascosti
lser -lSr /var/log             # ricorsivo, i file più grandi prima
lser -lhtr ~/progetti          # più recenti prima, size leggibili
lser -DT -L 3 .                # solo la struttura delle cartelle
lser -E -j 4 -r /usr/include > /dev/null   # tempo del motore con 4 thread
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

Codici di uscita: `0` ok, `1` errori parziali (es. permessi), `2` percorso non accessibile.

## Limitazioni note

- Solo Linux/POSIX.
- L'ordinamento per nome è bytewise (nessuna collation di locale): usa `-i` per ignorare le maiuscole.
- `.` e `..` non vengono mai mostrati. Nel tree i symlink contano come file.
- A differenza di `ls`, `-r` è "ricorsivo" e `-R` è "reverse".

## Documentazione

- [ARCHITECTURE.md](ARCHITECTURE.md): scelte di design e compromessi
- [BENCHMARKS.md](BENCHMARKS.md): come riprodurre e interpretare i benchmark

## Licenza

[MIT](LICENSE)
