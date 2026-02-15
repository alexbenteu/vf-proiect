# Benchmark Report

- Benchmark: `de8c42d50f1dbf43a731f1916dc6bf01-16_16_booth_dadda_mapped_and_and_wallace_origin_bit28.cnf`
- CNF path: `/home/bent/proiect-minisat/tests/de8c42d50f1dbf43a731f1916dc6bf01-16_16_booth_dadda_mapped_and_and_wallace_origin_bit28.cnf`
- Date: `2026-02-12T14:37:14+00:00`
- Timeout per run: `5000s`
- Repetitions: `1` CPU + `1` CPU+GPU
- MiniSat: `/home/bent/proiect-minisat/third_party/minisat/build/release/bin/minisat`
- GPU service: `/home/bent/proiect-minisat/build/cp3_oracle_online_service`
- Oracle profile: strategic VSIDS-only (`request-every=8`, `submit-conf=1024`, `walks=8`, `targets=128`)

## Hardware

- CPU: `Intel(R) Xeon(R) CPU E5-2697A v4 @ 2.60GHz`
- RAM: `19Gi`
- GPU: `NVIDIA RTX 2000 Ada Generation`
- NVIDIA driver: `590.48.01`

## Per-Run Metrics

| mode | run | status | rc | total_s | cpu_s | restarts | conflicts | decisions | propagations | mem_mb | oracle_reqs | oracle_dropped | hints_applied | vsids_bumped | gpu_util_peak | gpu_mem_peak | note |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| cpu | 1 | TIMEOUT | 124 | 5000.00 | 4999.83 | 83452 | 64696847 | 81650563 | 14352398627 | 52.49 |  |  |  |  |  |  | ok |
| gpu | 1 | TIMEOUT | 124 | 5000.00 | 4999.57 | 83965 | 65043594 | 82216729 | 14476331469 | 91.84 | 63413 | 107 | 0 | 2880206 | 34 | 137 | ok |

## Aggregate (Mean/Min/Max)

| mode | n | mean_total_s | min_total_s | max_total_s | mean_cpu_s | min_cpu_s | max_cpu_s | mean_conflicts |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cpu | 1 | 5000.000 | 5000.000 | 5000.000 | 4999.830000 | 4999.830000 | 4999.830000 | 64696847 |
| gpu | 1 | 5000.000 | 5000.000 | 5000.000 | 4999.570000 | 4999.570000 | 4999.570000 | 65043594 |

## Artifacts

- Raw TSV: `/home/bent/proiect-minisat/benchmarks/de8c42d50f1dbf43a731f1916dc6bf01-16_16_booth_dadda_mapped_and_and_wallace_origin_bit28_5000s_3x_20260212_115029.tsv`
- Run directory: `/home/bent/proiect-minisat/benchmarks/runs/de8c42d50f1dbf43a731f1916dc6bf01-16_16_booth_dadda_mapped_and_and_wallace_origin_bit28_20260212_115029`
- Solver/service/gpu logs: under `/home/bent/proiect-minisat/benchmarks/runs/de8c42d50f1dbf43a731f1916dc6bf01-16_16_booth_dadda_mapped_and_and_wallace_origin_bit28_20260212_115029`
