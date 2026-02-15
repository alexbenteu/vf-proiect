# MiniSat + Instinct GPU

Repository pentru lucrarea privind integrarea hibridă CPU–GPU într-un solver SAT de tip CDCL.

Acest proiect extinde MiniSat printr-un modul GPU extern care realizează evaluare matricială paralelă pe Tensor Cores și influențează euristica de selecție a variabilelor, fără a modifica mecanismul logic CDCL.

---

## Overview

Arhitectura este separată în două componente:

- **CPU** – MiniSat (CDCL complet, nemodificat logic)
- **GPU** – serviciu extern `instinct_gpu_service`
- **Comunicare** – shared memory (`mmap`) organizată ca ring buffer

GPU-ul primește snapshot-uri ale stării parțiale, evaluează scenarii candidate folosind operații matriciale binare (`mma.sync...b1`) și scrie scoruri per variabilă. Solverul ajustează activitatea variabilelor (VSIDS) pe baza acestor scoruri.

Integrarea este asincronă și nu afectează corectitudinea solverului.

---

## Requirements

### Hardware
- NVIDIA GPU cu suport Tensor Cores (Ampere sau mai nou)

### Software
- Linux (testat pe Ubuntu)
- CUDA Toolkit (`nvcc`)
- NVIDIA driver (`nvidia-smi`)
- `g++`, `make`
- `zlib1g-dev`

Instalare minimă (Ubuntu):

```bash
sudo apt update
sudo apt install -y build-essential make zlib1g-dev
```

---

## Build

### 1. Build serviciu GPU

```bash
make -j"$(nproc)" instinct-gpu-service
```

### 2. Build MiniSat

```bash
make -C third_party/minisat -j"$(nproc)" r \
  CXXFLAGS='-fpermissive' \
  MINISAT_LDFLAGS='-Wall -lz'
```

---

## Run (manual)

### Terminal 1 – GPU service

```bash
./build/instinct_gpu_service \
  --cnf tests/<file>.cnf \
  --shm /tmp/minisat_instinct_gpu_shm.bin \
  --engine tensor \
  --tensor-require-b1
```

### Terminal 2 – MiniSat + GPU

```bash
third_party/minisat/build/release/bin/minisat \
  -instinct-gpu \
  -instinct-gpu-shm-path=/tmp/minisat_instinct_gpu_shm.bin \
  -instinct-gpu-vsids-inject \
  tests/<file>.cnf
```

---

## Benchmark

Script pentru rulare comparativă CPU vs CPU+GPU:

```bash
./benchmark.sh tests/<file>.cnf
```

Output:
- fișiere `.tsv` cu metrici agregate
- loguri detaliate per rulare

Timeout implicit: `5000s`.

---

## Notes

- Integrarea GPU este strict euristică (strategic VSIDS injection).
- Profilul de benchmark dezactivează tactical picks.
- `instinct_vsids_bumped` este metrica principală pentru impactul GPU.
- Performanța este dependentă de instanță; proiectul este un prototip de cercetare.
