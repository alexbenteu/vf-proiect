import argparse
import csv
import json
import subprocess
import time
import sys
import shutil
import lzma
from pathlib import Path
import re

def discover_benchmarks(paths):
    files = []
    for p in paths:
        p = Path(p)
        if p.is_dir():
            files.extend(sorted(p.rglob("*.cnf.xz")))
        elif p.is_file() and p.name.endswith(".cnf.xz"):
            files.append(p)
    return files

def parse_minisat_output(output: str):
    status = "UNKNOWN"
    if "SATISFIABLE" in output:
        status = "SATISFIABLE"
    elif "UNSATISFIABLE" in output:
        status = "UNSATISFIABLE"

    stats = {}
    for line in output.splitlines():
        if "CPU time" in line:
            m = re.search(r"CPU time\s*:\s*([\d\.]+)", line)
            if m:
                stats["cpu_time_solver"] = m.group(1)
        else:
            m = re.search(r"^(conflicts|decisions|propagations)\s*:\s*(\d+)", line.strip())
            if m:
                stats[m.group(1)] = m.group(2)
    return status, stats

def run_minisat(solver_exec, cnf_file, timeout, out_dir):
    
    base_name = cnf_file.name.replace(".cnf.xz", "")
    out_file = out_dir / f"{base_name}.out"
    log_file = out_dir / f"{base_name}.log.txt"
    
    temp_cnf_path = out_dir / f"{base_name}.temp.cnf"
    
    start = time.perf_counter()
    
    try:
        try:
            with lzma.open(cnf_file, "rb") as f_in:
                with open(temp_cnf_path, "wb") as f_out:
                    f_out.write(f_in.read())
        except (lzma.LZMAError, OSError) as e:
            elapsed = time.perf_counter() - start
            err_msg = f"EROARE DE DECOMPRESIE/SCRIERE: {e}"
            log_file.write_text(err_msg, encoding="utf-8")
            return {
                "benchmark": str(cnf_file), "status": "DECOMPRESS_ERROR", "time_seconds": round(elapsed, 6),
                "cpu_time_solver": "", "stats": {}, "timeout_seconds": timeout,
                "log_path": str(log_file), "output_file": str(out_file),
            }

        cmd = [str(solver_exec), str(temp_cnf_path), str(out_file)]

        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="latin-1",
            timeout=timeout,
        )
        elapsed = time.perf_counter() - start
        combined = proc.stdout + "\n" + proc.stderr
        status, stats = parse_minisat_output(combined)

        log_file.write_text(combined, encoding="utf-8")

        return {
            "benchmark": str(cnf_file), "status": status, "time_seconds": round(elapsed, 6),
            "cpu_time_solver": stats.get("cpu_time_solver", ""), "stats": stats, "timeout_seconds": timeout,
            "log_path": str(log_file), "output_file": str(out_file),
        }

    except subprocess.TimeoutExpired:
        elapsed = time.perf_counter() - start
        log_file.write_text("TIMEOUT", encoding="utf-8")
        return {
            "benchmark": str(cnf_file), "status": "TIMEOUT", "time_seconds": round(elapsed, 6),
            "cpu_time_solver": "", "stats": {}, "timeout_seconds": timeout,
            "log_path": str(log_file), "output_file": str(out_file),
        }
    
    finally:
        if temp_cnf_path.exists():
            temp_cnf_path.unlink()


def save_results(results, outdir):
    """Salvează rezultatele în JSON, CSV și Markdown."""
    with open(outdir / "results.json", "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    
    with open(outdir / "results.csv", "w", newline="", encoding="utf-8") as f:
        fieldnames = ["benchmark", "status", "time_seconds", "cpu_time_solver", "timeout_seconds"]
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in results:
            writer.writerow({k: r.get(k, "") for k in fieldnames})
    
    with open(outdir / "SUMMARY.md", "w", encoding="utf-8") as f:
        f.write("# Rezultate MiniSat – SAT2025\n\n")
        f.write("| Benchmark | Status | Timp total [s] | CPU solver [s] |\n")
        f.write("|------------|--------|----------------|----------------|\n")
        for r in results:
            f.write(f"| {Path(r['benchmark']).name} | {r['status']} | {r['time_seconds']:.3f} | {r.get('cpu_time_solver', '')} |\n")

def main():
    parser = argparse.ArgumentParser(description="Rulează benchmark-uri SAT (.cnf.xz) folosind MiniSat.")
    parser.add_argument("--bench", nargs="+", required=True, help="Fișiere .cnf.xz sau directoare")
    parser.add_argument("--timeout", type=int, default=1800, help="Timeout per benchmark (secunde)")
    parser.add_argument("--outdir", type=Path, default=Path("results_minisat"))
    args = parser.parse_args()

    solver_name = "minisat"
    minisat_exec = shutil.which(solver_name)
    
    if minisat_exec is None:
        print(f"Eroare: Executabilul '{solver_name}' nu a fost găsit în PATH-ul sistemului.", file=sys.stderr)
        print("Vă rugăm să instalați MiniSat și să vă asigurați că este accesibil din terminal.", file=sys.stderr)
        sys.exit(1)
        
    print(f"Folosind executabilul MiniSat: {minisat_exec}")

    outdir = args.outdir.resolve()
    outdir.mkdir(parents=True, exist_ok=True)

    benchmarks = discover_benchmarks(args.bench)
    if not benchmarks:
        print(f"Nu s-au găsit fișiere .cnf.xz în: {args.bench}")
        sys.exit(1)

    print(f"Se rulează MiniSat pe {len(benchmarks)} benchmark-uri (.cnf.xz)...")

    results = []
    for cnf in benchmarks:
        print(f"→ {cnf.name} ...", end=" ", flush=True)
        res = run_minisat(minisat_exec, cnf, args.timeout, outdir)
        print(f"{res['status']} ({res['time_seconds']:.2f}s)")
        results.append(res)

    save_results(results, outdir)
    print(f"\nRezultate salvate în {outdir}/SUMMARY.md")

if __name__ == "__main__":
    main()