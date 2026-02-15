# MiniSat + Instinct GPU

Implementare hibridă CPU+GPU pentru SAT, construită peste MiniSat.

- CPU: MiniSat (CDCL)
- GPU: serviciu separat `instinct_gpu_service`
- Comunicare: shared memory (`mmap`) + ring buffer
- Integrare în solver: **strategic VSIDS injection** (fără tactical picks în profilul de benchmark curent)

## Ce face proiectul

Proiectul rulează MiniSat normal, iar în paralel trimite snapshot-uri de stare către un serviciu GPU.
Serviciul calculează scoruri de risc pe variabile (Tensor Cores, path `mma.sync...b1`) și scrie hint-uri în shared memory.
Solverul citește hint-urile și ajustează activitatea variabilelor (`varBumpActivity`), adică influențează euristica VSIDS fără a modifica corectitudinea CDCL.

## Cerințe

### Hardware
- GPU NVIDIA (recomandat Ampere/Hopper pentru path-ul `b1`)

### Software
- Linux (testat pe Ubuntu)
- `g++`, `make`
- CUDA Toolkit (cu `nvcc`)
- NVIDIA driver + `nvidia-smi`
- `zlib` dev headers pentru build MiniSat (`zlib1g-dev`)
- `timeout` și `/usr/bin/time` (folosite de benchmark script)

Exemplu instalare pachete de bază (Ubuntu):

```bash
sudo apt update
sudo apt install -y build-essential make cmake zlib1g-dev coreutils
```

## Structură proiect (relevantă)

- `third_party/minisat/` - MiniSat modificat (integrare Instinct GPU)
- `src/instinct_gpu_service.cu` - serviciul GPU
- `third_party/minisat/minisat/core/InstinctGpuShared.h` - layout shared memory
- `benchmark.sh` - runner benchmark CPU vs CPU+GPU
- `tests/` - fișiere CNF de test
- `benchmarks/` - rezultate generate (`.tsv`, `.md`, loguri)

## Build

### 1) Build serviciu GPU

```bash
make -j"$(nproc)" instinct-gpu-service
```

Rezultat: `build/instinct_gpu_service`

### 2) Build MiniSat (release)

```bash
make -C third_party/minisat -j"$(nproc)" r \
  CXXFLAGS='-fpermissive' \
  MINISAT_LDFLAGS='-Wall -lz'
```

Rezultat: `third_party/minisat/build/release/bin/minisat`

### 3) Clean build (opțional)

```bash
make clean
make -C third_party/minisat clean
```

## Rulare manuală (2 terminale)

### Terminal 1: serviciul GPU

```bash
./build/instinct_gpu_service \
  --cnf tests/8e62c5d47920ffe36052f86177403e70-SC25_Timetable_C_393_E_45_Cl_26_D_7_T_50.normalised.cnf \
  --shm /tmp/minisat_instinct_gpu_shm.bin \
  --walks 8 \
  --engine tensor \
  --tensor-require-b1
```

### Terminal 2: MiniSat + Instinct GPU (profil strategic VSIDS)

```bash
third_party/minisat/build/release/bin/minisat \
  -verb=1 \
  -instinct-gpu \
  -instinct-gpu-shm-path=/tmp/minisat_instinct_gpu_shm.bin \
  -instinct-gpu-threshold=0.05 \
  -instinct-gpu-request-every=8 \
  -instinct-gpu-submit-every-conflicts=1024 \
  -instinct-gpu-targets=128 \
  -instinct-gpu-walks=8 \
  -instinct-gpu-candidate-scan-limit=2048 \
  -instinct-gpu-max-hint-lag=256 \
  -instinct-gpu-max-level-drift=4096 \
  -instinct-gpu-max-assign-drift=65536 \
  -no-instinct-gpu-require-top-match \
  -no-instinct-gpu-submit-on-restart \
  -no-instinct-gpu-tactical-pick \
  -no-instinct-gpu-aggressive-pick \
  -no-instinct-gpu-preempt \
  -no-instinct-gpu-hard-stop-pick \
  -no-instinct-gpu-adaptive-threshold \
  -no-instinct-gpu-phase-inject \
  -no-instinct-gpu-vsids-phase-overwrite \
  -instinct-gpu-vsids-inject \
  -instinct-gpu-vsids-every-conflicts=2048 \
  -instinct-gpu-vsids-topk=128 \
  -instinct-gpu-vsids-conf-floor=0.001 \
  -instinct-gpu-vsids-min-gap=0.0 \
  -instinct-gpu-vsids-bump-scale=0.50 \
  tests/8e62c5d47920ffe36052f86177403e70-SC25_Timetable_C_393_E_45_Cl_26_D_7_T_50.normalised.cnf
```

## Benchmark automat

Rulează benchmark CPU și CPU+GPU pe un fișier CNF:

```bash
./benchmark.sh tests/8e62c5d47920ffe36052f86177403e70-SC25_Timetable_C_393_E_45_Cl_26_D_7_T_50.normalised.cnf
```

Output:
- `benchmarks/<nume>_5000s_3x_<timestamp>.tsv`
- `benchmarks/<nume>_5000s_3x_<timestamp>.md`
- loguri detaliate în `benchmarks/runs/<nume>_<timestamp>/`

Notă: scriptul are timeout fix `5000s`/run; profilul este strategic VSIDS-only.
În versiunea curentă, `benchmark.sh` rulează implicit 1x CPU + 1x CPU+GPU (`REPEATS=1`), iar sufixul `3x` din numele fișierului este păstrat dintr-o convenție mai veche.

## Metrici utile în rezultate

Din TSV/MD:
- `total_s`, `cpu_s`
- `restarts`, `conflicts`, `decisions`, `propagations`
- `instinct_reqs`, `instinct_dropped`
- `instinct_hints_applied` (în profilul curent poate rămâne `0`)
- `instinct_vsids_bumped` (metrica principală pentru profil strategic)
- `gpu_peak_util_pct`, `gpu_peak_mem_mib`

## Troubleshooting

### 1) `failed to parse CNF header`
Ai descărcat pagina HTML GitHub în loc de fișierul raw CNF.
Folosește linkul raw (`raw.githubusercontent.com`) sau pune fișierul CNF valid în `tests/`.

### 2) GPU service nu pornește
Verifică:
- existența `build/instinct_gpu_service`
- driver/CUDA (`nvidia-smi`, `nvcc --version`)
- acces la GPU pe VM/container

### 3) `hints_applied = 0`
În profilul strategic curent este normal: tactical path este dezactivat.
Semnalul util se vede în `instinct_vsids_bumped` și în reducerea conflictelor/timpului.

### 4) Vreau rebuild complet

```bash
make clean
make -C third_party/minisat clean
make -j"$(nproc)" instinct-gpu-service
make -C third_party/minisat -j"$(nproc)" r CXXFLAGS='-fpermissive' MINISAT_LDFLAGS='-Wall -lz'
```

## Documentație internă

- `Documentatie/rezumat.md` - stare tehnică curentă
- `Documentatie/ideea de proiect.md` - blueprint/obiective
- `Documentatie/raport_final.md` - raport proiect
