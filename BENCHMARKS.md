# Benchmark

I benchmark usano [Google Benchmark](https://github.com/google/benchmark) e misurano il **motore** di LSer:
scansione della directory, `stat` (se richieste), ordinamento e raccolta in memoria. **Non** misurano la stampa.

## Riprodurre

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

```sh
# Tempi (1, 2, 4, 8 thread e "auto"): tracciamento memoria SPENTO
./build/bench_lser /usr/include

# Memoria: churn, picco di heap, numero di allocazioni
./build/bench_lser --track-memory /usr/include

# Solo la modalità automatica, più veloce
./build/bench_lser /usr/include --benchmark_filter='threads:0'

# Per risultati più stabili: ripetizioni e statistiche
./build/bench_lser /usr/include --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
```

All'avvio il programma confronta il numero di entry con `std::filesystem::recursive_directory_iterator`
(`Verifica : OK (30573 entry, ...)`), così un risultato veloce non può nascondere un lavoro incompleto.

Accorgimenti per ridurre il rumore: governor `performance` (`cpupower frequency-set -g performance`), nessun altro
carico, e `setarch -R ./build/bench_lser ...` per disattivare l'ASLR (il benchmark lo segnala).

### Suite

| Benchmark | Equivalente | Cosa esercita |
|---|---|---|
| `Recursive_Simple` | `ls -R` | solo `readdir` + `d_type`, nessuna `stat` |
| `Recursive_LongFormat` | `ls -laR` | una `fstatat` per entry |
| `Recursive_SortSize` | `ls -laRS` | come sopra, ordinando per dimensione |
| `TreeView` | `tree -a` | scansione ricorsiva dei dati del tree (**solo motore**, vedi sotto) |

Ogni suite gira con `threads:` = 1, 2, 4, 8 e 0 (= `hardware_concurrency()`).

## Risultati

Macchina: AMD Ryzen, 8 core (dati dell'autore). Target `/usr/include`, **30 573 entry**, page cache calda.
Tempi in ms di **tempo reale** (wall clock), tracciamento memoria spento, una singola run.

| Suite | 1 thread | 2 | 4 | 8 | auto (0) |
|---|---|---|---|---|---|
| `Recursive_Simple` | 48,1 | 28,7 | 18,1 | **11,9** | 12,9 |
| `Recursive_LongFormat` | 107 | 59,5 | 37,6 | **28,1** | 27,8 |
| `Recursive_SortSize` | 107 | 59,3 | 39,3 | **28,9** | 27,9 |
| `TreeView` | 48,5 | 27,4 | 17,6 | **11,7** | 12,0 |

Speedup 1 → 8 thread: `Simple` **4,04×**, `LongFormat` **3,81×**. Throughput a 8 thread: ~2,57 M entry/s (`Simple`),
~1,09 M entry/s (`LongFormat`).

Memoria (`--track-memory`, stesse 30 573 entry):

| Metrica | Valore |
|---|---|
| Allocazioni per run | ~17 000 (≈ 0,56 per entry) |
| Churn | ~8,3 MiB |
| Picco di heap C++ | ~5,84 MiB |

## Come leggere i numeri

**1. Il "CPU time" non è il tempo totale di CPU.** La colonna `CPU` di Google Benchmark misura di default il tempo CPU
del solo thread principale. Con 1 thread coincide con il tempo reale (47,8 vs 48,1 ms); con 8 thread (10,3 vs 11,9 ms)
non include il lavoro degli altri 7 worker. La cifra confrontabile è il **tempo reale**: ~12 ms con 8 thread. Per
avere la CPU totale del processo aggiungi `->MeasureProcessCPUTime()` alla registrazione dei benchmark.

**2. Costo delle `stat`.** Da `Simple` a `LongFormat` a 1 thread la differenza è ~59 ms su 30 573 entry, cioè
**~1,9 µs per `fstatat`** (stima grezza: le due suite differiscono anche per altri flag, ma su `/usr/include` non ci
sono nascosti e pochissimi symlink). È il motivo per cui il listing semplice non chiama mai `stat`.

**3. L'ordinamento per dimensione è gratis.** `SortSize` ≈ `LongFormat` entro il rumore: ordinare piccole liste
per cartella nel worker costa molto meno delle syscall.

**4. `TreeView` ≈ `Simple` perché il benchmark misura solo il motore.** La costruzione dell'indice e la stampa
dell'albero (`print_tree`) non sono incluse in nessuna suite. Non va letto come "il tree costa quanto la scansione".

**5. Il tracciamento memoria costa poco, ma non zero.** A parità di suite i tempi con tracciamento sono entro ~8% di
quelli senza (es. `Simple` a 8 thread: 12,9 vs 11,9 ms), ma la stessa suite, ripetuta senza tracciamento, è oscillata
anche del ~15% tra una run e l'altra (12,9 → 14,9 ms): il rumore è dello stesso ordine. Usa comunque due run separate.

**6. Peak ≠ RSS.** `Peak_MB` è il picco dell'heap C++ allocato dal motore. La RSS del processo include anche
eseguibile, librerie, stack dei thread e arene di glibc, quindi è più alta. Per misurarla:

```sh
/usr/bin/time -v ./build/lser -ra /usr/include > /dev/null 2>time.txt; grep "Maximum resident" time.txt
```

## Motore vs terminale

Il numero in evidenza (~12 ms) è il tempo per **avere i dati pronti in memoria**. Mostrare 30 000 righe in un
emulatore di terminale è un lavoro diverso, e tipicamente più lento, perché dipende dal throughput del terminale
(rendering, scrolling, pty), non dalla CPU di LSer. Per separare i due contributi:

```sh
./build/lser -E -ra /usr/include > /dev/null    # motore + formattazione, nessun terminale (-E: tempo del motore su stderr)
./build/lser -ra /usr/include                   # con il terminale reale: ripeti con `time`
```

Confronta i tempi: la differenza è il costo del terminale. (Non riporto cifre qui perché dipendono dall'emulatore.)

## Limiti di questi risultati

- **Cache calda**: la page cache e i dentry sono già in RAM. Non sono numeri di accesso a disco freddo.
- **Una macchina, una run** per cella, senza deviazione standard (usa `--benchmark_repetitions`).
- **Nessun confronto** con `ls -R`, `find`, `fd`, `tree` in questa tabella. Suggerimento:
  ```sh
  hyperfine --warmup 3 'ls -R /usr/include > /dev/null' 'find /usr/include > /dev/null' \
            'fd -H -I . /usr/include > /dev/null' './build/lser -ra /usr/include > /dev/null'
  ```
- Il numero di entry dipende dal sistema: `Verifica: OK (N entry)` indica il tuo.
