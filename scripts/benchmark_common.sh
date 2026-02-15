#!/usr/bin/env bash

run_timed() {
  local timeout_s="$1"
  local log_file="$2"
  shift 2

  local time_file="${log_file}.time"
  rm -f "$time_file"

  set +e
  /usr/bin/time -q -f "%e" -o "$time_file" timeout "${timeout_s}s" "$@" >"$log_file" 2>&1
  local rc=$?
  set -e

  local elapsed=""
  if [[ -f "$time_file" ]]; then
    elapsed="$(awk '/^[0-9]+([.][0-9]+)?$/{v=$0} END{print v}' "$time_file")"
  fi
  echo "${rc}|${elapsed}"
}

run_timed_with_gpu_monitor() {
  local timeout_s="$1"
  local log_file="$2"
  local gpu_file="$3"
  shift 3

  local time_file="${log_file}.time"
  rm -f "$time_file" "$gpu_file"

  local nvidia_smi_bin="${NVIDIA_SMI_BIN:-$(command -v nvidia-smi || true)}"
  local poll_interval="${GPU_POLL_INTERVAL_S:-1}"
  local mon_pid=""
  if [[ -n "$nvidia_smi_bin" ]]; then
    (
      while true; do
        "$nvidia_smi_bin" --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>/dev/null | head -n 1 >> "$gpu_file"
        sleep "$poll_interval"
      done
    ) &
    mon_pid=$!
  fi

  set +e
  /usr/bin/time -q -f "%e" -o "$time_file" timeout "${timeout_s}s" "$@" >"$log_file" 2>&1
  local rc=$?
  set -e

  if [[ -n "$mon_pid" ]]; then
    kill "$mon_pid" >/dev/null 2>&1 || true
    wait "$mon_pid" 2>/dev/null || true
  fi

  local elapsed=""
  local peak_util="0"
  local peak_mem="0"

  if [[ -f "$time_file" ]]; then
    elapsed="$(awk '/^[0-9]+([.][0-9]+)?$/{v=$0} END{print v}' "$time_file")"
  fi
  if [[ -f "$gpu_file" ]]; then
    peak_util="$(awk -F',' '
      BEGIN{m=0}
      {
        gsub(/[[:space:]]/,"",$1);
        if ($1 ~ /^[0-9]+$/ && $1 > m) m=$1;
      }
      END{print m}
    ' "$gpu_file")"
    peak_mem="$(awk -F',' '
      BEGIN{m=0}
      {
        gsub(/[[:space:]]/,"",$2);
        if ($2 ~ /^[0-9]+$/ && $2 > m) m=$2;
      }
      END{print m}
    ' "$gpu_file")"
  fi

  echo "${rc}|${elapsed}|${peak_util}|${peak_mem}"
}

sat_status_from_log() {
  local log_file="$1"
  local rc="$2"
  if grep -q "UNSATISFIABLE" "$log_file"; then
    echo "UNSATISFIABLE"
  elif grep -q "SATISFIABLE" "$log_file"; then
    echo "SATISFIABLE"
  elif [[ "$rc" == "124" ]]; then
    echo "TIMEOUT"
  else
    echo "UNKNOWN"
  fi
}
