#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/scripts/benchmark_common.sh"

TIMEOUT_S=5000
REPEATS=3

usage() {
  cat <<'EOF'
Usage:
  ./benchmark.sh <file.cnf|path/to/file.cnf>

Behavior:
  - rebuilds MiniSat (release) and GPU instinct service
  - runs 3x CPU-only (MiniSat)
  - runs 3x CPU+GPU instinct-gpu
  - timeout is fixed to 5000s per run
  - writes .md and .tsv reports in ./benchmarks
EOF
}

die() {
  echo "Error: $*" >&2
  exit 1
}

resolve_cnf_path() {
  local input="$1"
  if [[ -f "$input" ]]; then
    readlink -f "$input"
    return 0
  fi
  if [[ -f "$ROOT_DIR/$input" ]]; then
    readlink -f "$ROOT_DIR/$input"
    return 0
  fi
  if [[ -f "/tmp/vf-proiect/script/tests/$input" ]]; then
    readlink -f "/tmp/vf-proiect/script/tests/$input"
    return 0
  fi

  local found
  found="$(find "$ROOT_DIR" -type f -name "$input" | head -n1 || true)"
  if [[ -n "$found" ]]; then
    readlink -f "$found"
    return 0
  fi
  return 1
}

extract_nth_int_for_prefix() {
  local log_file="$1"
  local prefix="$2"
  local nth="$3"
  awk -v p="$prefix" -v want="$nth" '
    index($0, p) == 1 {
      c = 0;
      for (i = 1; i <= NF; i++) {
        tok = $i;
        gsub(/^[^0-9]+/, "", tok);
        gsub(/[^0-9]+$/, "", tok);
        if (tok ~ /^[0-9]+$/) {
          c++;
          if (c == want) {
            print tok;
            exit;
          }
        }
      }
    }
  ' "$log_file"
}

choose_walks() {
  local clauses="$1"
  if (( clauses > 400000 )); then
    echo 1
  elif (( clauses > 200000 )); then
    echo 2
  elif (( clauses > 50000 )); then
    echo 4
  elif (( clauses > 10000 )); then
    echo 6
  else
    echo 8
  fi
}

choose_targets() {
  local clauses="$1"
  if (( clauses > 400000 )); then
    echo 32
  elif (( clauses > 200000 )); then
    echo 64
  else
    echo 128
  fi
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || $# -lt 1 ]]; then
  usage
  exit 0
fi

CNF_INPUT="$1"
CNF_PATH="$(resolve_cnf_path "$CNF_INPUT" || true)"
[[ -n "${CNF_PATH:-}" ]] || die "CNF not found: $CNF_INPUT"

CNF_BASE="$(basename "$CNF_PATH")"
CNF_STEM="${CNF_BASE%.cnf}"
TS="$(date +%Y%m%d_%H%M%S)"

mkdir -p "$ROOT_DIR/benchmarks/runs"
RUN_DIR="$ROOT_DIR/benchmarks/runs/${CNF_STEM}_${TS}"
mkdir -p "$RUN_DIR"

REPORT_TSV="$ROOT_DIR/benchmarks/${CNF_STEM}_5000s_3x_${TS}.tsv"
REPORT_MD="$ROOT_DIR/benchmarks/${CNF_STEM}_5000s_3x_${TS}.md"

NPROC="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "[build] Building MiniSat release..."
make -C "$ROOT_DIR/third_party/minisat" -j"$NPROC" r CXXFLAGS='-fpermissive' MINISAT_LDFLAGS='-Wall -lz' >/dev/null
echo "[build] Building GPU instinct service..."
make -C "$ROOT_DIR" -j"$NPROC" instinct-gpu-service >/dev/null

MINISAT_BIN="$ROOT_DIR/third_party/minisat/build/release/bin/minisat"
SERVICE_BIN="$ROOT_DIR/build/instinct_gpu_service"

[[ -x "$MINISAT_BIN" ]] || die "missing MiniSat binary: $MINISAT_BIN"
[[ -x "$SERVICE_BIN" ]] || die "missing service binary: $SERVICE_BIN"

VARS="$(awk '/^p cnf/{print $3; exit}' "$CNF_PATH")"
CLAUSES="$(awk '/^p cnf/{print $4; exit}' "$CNF_PATH")"
[[ -n "${VARS:-}" && -n "${CLAUSES:-}" ]] || die "failed to parse CNF header from $CNF_PATH"

WALKS="$(choose_walks "$CLAUSES")"
TARGETS="$(choose_targets "$CLAUSES")"

echo -e "mode\trun\tstatus\trc\ttotal_s\tcpu_s\trestarts\tconflicts\tdecisions\tpropagations\tmemory_mb\tinstinct_reqs\tinstinct_dropped\tinstinct_hints_applied\tinstinct_vsids_bumped\tgpu_peak_util_pct\tgpu_peak_mem_mib\tnote\tsolver_log\tservice_log\tgpu_csv" > "$REPORT_TSV"

run_cpu_once() {
  local i="$1"
  local solver_log="$RUN_DIR/cpu_run${i}.log"
  local result rc total_s status
  local cpu_s restarts conflicts decisions propagations memory_mb

  result="$(run_timed "$TIMEOUT_S" "$solver_log" "$MINISAT_BIN" -verb=1 "$CNF_PATH")"
  rc="${result%%|*}"
  total_s="${result##*|}"
  status="$(sat_status_from_log "$solver_log" "$rc")"

  cpu_s="$(awk '/^CPU time/{print $4; exit}' "$solver_log")"; cpu_s="${cpu_s:-}"
  restarts="$(awk '/^restarts/{print $3; exit}' "$solver_log")"; restarts="${restarts:-}"
  conflicts="$(awk '/^conflicts/{print $3; exit}' "$solver_log")"; conflicts="${conflicts:-}"
  decisions="$(awk '/^decisions/{print $3; exit}' "$solver_log")"; decisions="${decisions:-}"
  propagations="$(awk '/^propagations/{print $3; exit}' "$solver_log")"; propagations="${propagations:-}"
  memory_mb="$(awk '/^Memory used/{print $4; exit}' "$solver_log")"; memory_mb="${memory_mb:-}"

  echo -e "cpu\t${i}\t${status}\t${rc}\t${total_s}\t${cpu_s}\t${restarts}\t${conflicts}\t${decisions}\t${propagations}\t${memory_mb}\t\t\t\t\t\t\tok\t${solver_log}\t\t" >> "$REPORT_TSV"
}

run_gpu_once() {
  local i="$1"
  local solver_log="$RUN_DIR/gpu_solver_run${i}.log"
  local service_log="$RUN_DIR/gpu_service_run${i}.log"
  local gpu_csv="$RUN_DIR/gpu_mon_run${i}.csv"
  local shm_path="$RUN_DIR/gpu_run${i}.shm.bin"
  local service_pid=""
  local note="ok"

  local result rc total_s peak_util peak_mem status
  local cpu_s restarts conflicts decisions propagations memory_mb
  local instinct_reqs instinct_dropped instinct_hints_applied instinct_vsids_bumped

  rm -f "$solver_log" "$service_log" "$gpu_csv" "$shm_path"

  "$SERVICE_BIN" \
    --cnf "$CNF_PATH" \
    --shm "$shm_path" \
    --walks "$WALKS" \
    --engine tensor \
    --tensor-require-b1 \
    >"$service_log" 2>&1 &
  service_pid=$!
  sleep 0.8
  if ! kill -0 "$service_pid" >/dev/null 2>&1; then
    note="service_start_failed"
    : > "$solver_log"
    rc="2"
    total_s=""
    peak_util="0"
    peak_mem="0"
    status="UNKNOWN"
    cpu_s=""
    restarts=""
    conflicts=""
    decisions=""
    propagations=""
    memory_mb=""
    instinct_reqs=""
    instinct_dropped=""
    instinct_hints_applied=""
    instinct_vsids_bumped=""
  else
    result="$(
      run_timed_with_gpu_monitor "$TIMEOUT_S" "$solver_log" "$gpu_csv" \
        "$MINISAT_BIN" -verb=1 \
        -instinct-gpu \
        -instinct-gpu-shm-path="$shm_path" \
        -instinct-gpu-threshold=0.05 \
        -instinct-gpu-request-every=8 \
        -instinct-gpu-submit-every-conflicts=1024 \
        -instinct-gpu-targets="$TARGETS" \
        -instinct-gpu-walks="$WALKS" \
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
        "$CNF_PATH"
    )"

    rc="${result%%|*}"
    total_s="$(echo "$result" | cut -d'|' -f2)"
    peak_util="$(echo "$result" | cut -d'|' -f3)"
    peak_mem="$(echo "$result" | cut -d'|' -f4)"
    status="$(sat_status_from_log "$solver_log" "$rc")"

    cpu_s="$(awk '/^CPU time/{print $4; exit}' "$solver_log")"; cpu_s="${cpu_s:-}"
    restarts="$(awk '/^restarts/{print $3; exit}' "$solver_log")"; restarts="${restarts:-}"
    conflicts="$(awk '/^conflicts/{print $3; exit}' "$solver_log")"; conflicts="${conflicts:-}"
    decisions="$(awk '/^decisions/{print $3; exit}' "$solver_log")"; decisions="${decisions:-}"
    propagations="$(awk '/^propagations/{print $3; exit}' "$solver_log")"; propagations="${propagations:-}"
    memory_mb="$(awk '/^Memory used/{print $4; exit}' "$solver_log")"; memory_mb="${memory_mb:-}"

    instinct_reqs="$(extract_nth_int_for_prefix "$solver_log" "instinct-gpu reqs" 1)"; instinct_reqs="${instinct_reqs:-0}"
    instinct_dropped="$(extract_nth_int_for_prefix "$solver_log" "instinct-gpu reqs" 2)"; instinct_dropped="${instinct_dropped:-0}"
    instinct_hints_applied="$(sed -n 's/^instinct-gpu usage.*hint-applied: \([0-9][0-9]*\).*/\1/p' "$solver_log" | head -n1)"; instinct_hints_applied="${instinct_hints_applied:-0}"
    instinct_vsids_bumped="$(sed -n 's/^instinct-gpu vsids.*bumped=\([0-9][0-9]*\).*/\1/p' "$solver_log" | head -n1)"; instinct_vsids_bumped="${instinct_vsids_bumped:-0}"
  fi

  if [[ -n "$service_pid" ]] && kill -0 "$service_pid" >/dev/null 2>&1; then
    kill "$service_pid" >/dev/null 2>&1 || true
    wait "$service_pid" 2>/dev/null || true
  fi

  echo -e "gpu\t${i}\t${status}\t${rc}\t${total_s}\t${cpu_s}\t${restarts}\t${conflicts}\t${decisions}\t${propagations}\t${memory_mb}\t${instinct_reqs}\t${instinct_dropped}\t${instinct_hints_applied}\t${instinct_vsids_bumped}\t${peak_util}\t${peak_mem}\t${note}\t${solver_log}\t${service_log}\t${gpu_csv}" >> "$REPORT_TSV"
}

echo "[run] CNF: $CNF_PATH"
echo "[run] vars=$VARS clauses=$CLAUSES timeout=${TIMEOUT_S}s repeats=$REPEATS"
echo "[run] CPU-only runs..."
for i in $(seq 1 "$REPEATS"); do
  echo "  - CPU run $i/$REPEATS"
  run_cpu_once "$i"
done

echo "[run] CPU+GPU runs..."
for i in $(seq 1 "$REPEATS"); do
  echo "  - GPU run $i/$REPEATS"
  run_gpu_once "$i"
done

CPU_MODEL="$(lscpu | awk -F: '/Model name/{gsub(/^ +/, "", $2); print $2; exit}')"
RAM_TOTAL="$(free -h | awk '/^Mem:/{print $2}')"
GPU_NAME="$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -n1 || true)"
GPU_DRIVER="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -n1 || true)"

emit_summary_row() {
  local mode="$1"
  awk -F'\t' -v mode="$mode" '
    NR>1 && $1==mode && $5!="" {
      n++;
      t=$5+0; c=$6+0; f=$8+0;
      if (n==1 || t<tmin) tmin=t;
      if (n==1 || t>tmax) tmax=t;
      tsum+=t;
      if (n==1 || c<cmin) cmin=c;
      if (n==1 || c>cmax) cmax=c;
      csum+=c;
      if (n==1 || f<fmin) fmin=f;
      if (n==1 || f>fmax) fmax=f;
      fsum+=f;
    }
    END{
      if (n==0) {
        printf("| %s | 0 | - | - | - | - | - | - | - |\n", mode);
      } else {
        printf("| %s | %d | %.3f | %.3f | %.3f | %.6f | %.6f | %.6f | %.0f |\n",
               mode, n, tsum/n, tmin, tmax, csum/n, cmin, cmax, fsum/n);
      }
    }' "$REPORT_TSV"
}

{
  echo "# Benchmark Report"
  echo
  echo "- Benchmark: \`$CNF_BASE\`"
  echo "- CNF path: \`$CNF_PATH\`"
  echo "- Date: \`$(date -Iseconds)\`"
  echo "- Timeout per run: \`${TIMEOUT_S}s\`"
  echo "- Repetitions: \`${REPEATS}\` CPU + \`${REPEATS}\` CPU+GPU"
  echo "- MiniSat: \`$MINISAT_BIN\`"
  echo "- GPU service: \`$SERVICE_BIN\`"
  echo "- Instinct GPU profile: strategic VSIDS-only (\`request-every=8\`, \`submit-conf=1024\`, \`walks=$WALKS\`, \`targets=$TARGETS\`)"
  echo
  echo "## Hardware"
  echo
  echo "- CPU: \`${CPU_MODEL:-unknown}\`"
  echo "- RAM: \`${RAM_TOTAL:-unknown}\`"
  echo "- GPU: \`${GPU_NAME:-unknown}\`"
  echo "- NVIDIA driver: \`${GPU_DRIVER:-unknown}\`"
  echo
  echo "## Per-Run Metrics"
  echo
  echo "| mode | run | status | rc | total_s | cpu_s | restarts | conflicts | decisions | propagations | mem_mb | instinct_reqs | instinct_dropped | hints_applied | vsids_bumped | gpu_util_peak | gpu_mem_peak | note |"
  echo "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"
  awk -F'\t' 'NR>1{
    printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
      $1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18)
  }' "$REPORT_TSV"
  echo
  echo "## Aggregate (Mean/Min/Max)"
  echo
  echo "| mode | n | mean_total_s | min_total_s | max_total_s | mean_cpu_s | min_cpu_s | max_cpu_s | mean_conflicts |"
  echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|"
  emit_summary_row "cpu"
  emit_summary_row "gpu"
  echo
  echo "## Artifacts"
  echo
  echo "- Raw TSV: \`$REPORT_TSV\`"
  echo "- Run directory: \`$RUN_DIR\`"
  echo "- Solver/service/gpu logs: under \`$RUN_DIR\`"
} > "$REPORT_MD"

echo
echo "[done] Raw metrics: $REPORT_TSV"
echo "[done] Markdown report: $REPORT_MD"
echo "[done] Logs: $RUN_DIR"
