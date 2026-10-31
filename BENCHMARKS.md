# Benchmark

I benchmark usano [Google Benchmark](https://github.com/google/benchmark) e misurano il **motore** di LSer:
scansione della directory, metadati (se richiesti), ordinamento e raccolta in memoria. **Non** misurano la stampa.

## Riprodurre

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

```sh
# Tempi (1, 2, 4, 8, ... thread e "auto"): tracciamento memoria SPENTO
./build/bench_lser /usr/include

# Memoria: churn, picco di heap, numero di allocazioni (tempi falsati)
./build/bench_lser --track-memory /usr/include

# Solo la modalità automatica, più veloce
./build/bench_lser /usr/include --benchmark_filter='threads:0'

# Risultati più stabili: ripetizioni e statistiche
./build/bench_lser /usr/include --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
```

All'avvio il programma confronta il numero di entry con `std::filesystem::recursive_directory_iterator`
(`Verifica : OK (N entry, ...)`), così un risultato veloce non può nascondere un lavoro incompleto. Se la cartella
cambia durante la scansione un piccolo scarto è normale.

I thread provati sono le potenze di 2 fino a `hardware_concurrency()`, più il valore esatto se non è una potenza di 2,
più `0` (= automatico).

Per ridurre il rumore: governor `performance` (`cpupower frequency-set -g performance`), nessun altro carico, e
`setarch -R ./build/bench_lser ...` per disattivare l'ASLR (su Linux). Su Windows: profilo energetico "prestazioni
elevate" e antivirus che non scansiona la cartella di test.

## Suite

| Benchmark | Equivalente | Flag | Cosa esercita |
|---|---|---|---|
| `Recursive_Simple` | `ls -R` | `-r` | `readdir` + tipo, **senza nascosti** |
| `Recursive_LongFormat` | `ls -laR` | `-r -l -a` | metadati per ogni entry, **con nascosti** |
| `Recursive_SortSize` | `ls -laRS` | `-r -l -a -S` | come sopra, ordinando per dimensione |
| `TreeView` | `tree -a` | `-T -a` | scansione ricorsiva per il tree (**solo motore**) |

> ⚠️ `Recursive_Simple` non passa `-a`, le altre suite sì. Se il target contiene file nascosti (`.git`, `.cache`,
> `node_modules/.bin`...) le suite **non vedono lo stesso numero di entry** e i tempi non sono confrontabili 1:1.
> Guarda sempre la colonna `Entries`.

## Risultati

### Windows (run più recente)

Target: cartella di progetti C++ (`Verifica: OK, 9919 entry` con nascosti), `hardware_concurrency() = 12`.
Una singola run per cella, tracciamento memoria spento. Tempo = **real time**.

| Suite | Entry | 1 thread | 2 | 4 | 8 | 12 | auto (0) |
|---|---|---|---|---|---|---|---|
| `Recursive_Simple` | 5 425 | 32,6 | 17,2 | 12,0 | **7,79** | 8,32 | 8,56 |
| `Recursive_LongFormat` | 9 919 | 48,1 | 29,9 | 19,3 | **11,7** | 13,2 | 13,1 |
| `Recursive_SortSize` | 9 919 | 46,6 | 29,9 | 19,3 | **11,5** | 13,4 | 13,0 |
| `TreeView` | 9 919 | 47,9 | 30,3 | 19,1 | **11,3** | 13,2 | 13,0 |

Speedup 1 → 8 thread: `Simple` **4,18×**, `LongFormat` **4,11×**, `SortSize` **4,05×**, `TreeView` **4,24×**
(efficienza ≈ 51–53 %). Throughput a 8 thread: ~0,70 M entry/s (`Simple`), ~0,85–0,88 M entry/s (le altre).

### Linux (`/usr/include`, 30 573 entry, macchina AMD Ryzen a 8 core)

| Suite | 1 thread | 2 | 4 | 8 | auto (0) |
|---|---|---|---|---|---|
| `Recursive_Simple` | 48,1 | 28,7 | 18,1 | **11,9** | 12,9 |
| `Recursive_LongFormat` | 107 | 59,5 | 37,6 | **28,1** | 27,8 |
| `Recursive_SortSize` | 107 | 59,3 | 39,3 | **28,9** | 27,9 |
| `TreeView` | 48,5 | 27,4 | 17,6 | **11,7** | 12,0 |

Speedup 1 → 8 thread: `Simple` **4,04×**, `LongFormat` **3,81×**. Throughput a 8 thread: ~2,57 M entry/s
(`Simple`), ~1,09 M entry/s (`LongFormat`). Qui tutte le suite vedono lo stesso insieme di entry
(`/usr/include` non ha nascosti).

### Memoria (Linux, `--track-memory`, stesse 30 573 entry)

| Metrica | Valore | Per entry |
|---|---|---|
| Allocazioni per run | ~17 000 | ~0,56 |
| Churn | ~8,3 MiB | ~285 B |
| Picco di heap C++ | ~5,84 MiB | ~200 B |

## Come leggere i numeri

**1. Lo scaling è reale ma si ferma agli 8 thread.** Su entrambe le piattaforme il guadagno è ~4× con 8 thread. Sulla
macchina Windows con 12 thread logici, passare da 8 a 12 (e quindi `auto`) **peggiora** i tempi del 7–13 %: oltre i
core fisici i thread extra condividono le stesse unità di esecuzione e aumentano solo la contesa sul mutex della pila
e sul kernel. Per questo carico `-j` pari ai core fisici è spesso meglio del default.

**2. Il "CPU time" non è il tempo totale di CPU.** La colonna `CPU` di Google Benchmark misura di default il tempo CPU
del solo thread principale. Con 1 thread coincide con il tempo reale (32,0 vs 32,6 ms); con 8 thread (6,29 vs 7,79 ms)
non include il lavoro degli altri 7 worker. La cifra confrontabile è il **tempo reale**. Per la CPU totale del
processo aggiungi `->MeasureProcessCPUTime()` alla registrazione dei benchmark.

**3. Il costo dei metadati dipende dal backend.**
- Su **Linux** il listing semplice non chiama mai `stat` (il tipo arriva da `d_type`). Da `Simple` a `LongFormat` a
  1 thread la differenza è ~59 ms su 30 573 entry, cioè **~1,9 µs per `fstatat`** (stima grezza, ma qui le due suite
  vedono le stesse entry).
- Su **Windows** `FindFirstFileW` restituisce già dimensione, date e attributi: la scansione costa uguale con o senza
  `-l`. La differenza tra `Simple` e `LongFormat` nella tabella è dovuta quasi tutta ai **nascosti** (5 425 vs 9 919
  entry), non a chiamate extra: per entry `Simple` costa ~6,0 µs e `LongFormat` ~4,8 µs. Non va letta come "il long
  format è più veloce".

**4. L'ordinamento per dimensione è praticamente gratis.** `SortSize` ≈ `LongFormat` entro il rumore su entrambe le
piattaforme: ordinare piccole liste per cartella dentro il worker costa molto meno delle chiamate di sistema.

**5. `TreeView` ≈ `Simple`/`LongFormat` perché il benchmark misura solo il motore.** La costruzione dell'indice e la
stampa dell'albero (`print_tree`) non sono incluse. Non significa che "il tree costi quanto la scansione".

**6. Il tracciamento memoria costa poco, ma non zero.** I tempi con tracciamento sono entro ~8 % di quelli senza
(Linux, `Simple` a 8 thread: 12,9 vs 11,9 ms), ma la stessa suite ripetuta senza tracciamento è oscillata anche del
~15 % tra una run e l'altra: il rumore è dello stesso ordine. Usa due run separate.

**7. Peak ≠ RSS.** `Peak_MB` è il picco dell'heap C++ allocato dal motore. La RSS del processo include anche
eseguibile, librerie, stack dei thread e arene di glibc. Per misurarla su Linux:

```sh
/usr/bin/time -v ./build/lser -ra /usr/include > /dev/null 2>time.txt; grep "Maximum resident" time.txt
```

## Motore vs terminale

Il numero in evidenza è il tempo per **avere i dati pronti in memoria**. Mostrare decine di migliaia di righe in un
emulatore di terminale è un lavoro diverso, e tipicamente più lento, perché dipende dal throughput del terminale
(rendering, scrolling, pty/conpty), non dalla CPU di LSer. Per separare i due contributi:

```sh
./build/lser -E -ra /usr/include > /dev/null    # motore + formattazione, nessun terminale
time ./build/lser -ra /usr/include              # con il terminale reale
```

La differenza è il costo del terminale. (Non riporto cifre perché dipendono dall'emulatore.)

## Limiti di questi risultati

- **Cache calda**: page cache e dentry sono già in RAM. Non sono numeri di accesso a disco freddo.
- **Una macchina per sistema operativo, una run per cella**, senza deviazione standard (usa `--benchmark_repetitions`).
- **Target diversi tra Linux e Windows**: i due set di numeri mostrano lo scaling, non un confronto tra sistemi.
- **Nessun confronto** con `ls -R`, `find`, `fd`, `tree`. Per farlo:
  ```sh
  hyperfine --warmup 3 'ls -R /usr/include > /dev/null' 'find /usr/include > /dev/null' \
            'fd -H -I . /usr/include > /dev/null' './build/lser -ra /usr/include > /dev/null'
  ```
- Il numero di entry dipende dal sistema: `Verifica: OK (N entry)` indica il tuo.
