#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <vector>
#include "minisat/mtl/Alg.h"
#include "minisat/mtl/Sort.h"
#include "minisat/utils/System.h"
#include "minisat/core/Solver.h"
#include "minisat/core/InstinctGpuShared.h"
using namespace Minisat;
static const char* _cat = "CORE";
static DoubleOption opt_var_decay (_cat, "var-decay", "The variable activity decay factor", 0.95, DoubleRange(0, false, 1, false));
static DoubleOption opt_clause_decay (_cat, "cla-decay", "The clause activity decay factor", 0.999, DoubleRange(0, false, 1, false));
static DoubleOption opt_random_var_freq (_cat, "rnd-freq", "The frequency with which the decision heuristic tries to choose a random variable", 0, DoubleRange(0, true, 1, true));
static DoubleOption opt_random_seed (_cat, "rnd-seed", "Used by the random variable selection", 91648253, DoubleRange(0, false, HUGE_VAL, false));
static IntOption opt_ccmin_mode (_cat, "ccmin-mode", "Controls conflict clause minimization (0=none, 1=basic, 2=deep)", 2, IntRange(0, 2));
static IntOption opt_phase_saving (_cat, "phase-saving", "Controls the level of phase saving (0=none, 1=limited, 2=full)", 2, IntRange(0, 2));
static BoolOption opt_rnd_init_act (_cat, "rnd-init", "Randomize the initial activity", false);
static BoolOption opt_luby_restart (_cat, "luby", "Use the Luby restart sequence", true);
static IntOption opt_restart_first (_cat, "rfirst", "The base restart interval", 100, IntRange(1, INT32_MAX));
static DoubleOption opt_restart_inc (_cat, "rinc", "Restart interval increase factor", 2, DoubleRange(1, false, HUGE_VAL, false));
static DoubleOption opt_garbage_frac (_cat, "gc-frac", "The fraction of wasted memory allowed before a garbage collection is triggered", 0.20, DoubleRange(0, false, HUGE_VAL, false));
static IntOption opt_min_learnts_lim (_cat, "min-learnts", "Minimum learnt clause limit", 0, IntRange(0, INT32_MAX));
static DoubleOption opt_instinct_gpu_threshold (_cat, "instinct-gpu-threshold", "Apply hint when confidence >= threshold", 0.80, DoubleRange(0, true, 1, true));
static BoolOption opt_instinct_gpu (_cat, "instinct-gpu", "Enable asynchronous online GPU oracle via shared memory.", false);
static StringOption opt_instinct_gpu_shm_path (_cat, "instinct-gpu-shm-path", "Shared-memory file path for instinct gpu.", "/tmp/minisat_instinct_gpu_shm.bin");
static IntOption opt_instinct_gpu_request_every(_cat, "instinct-gpu-request-every", "Submit instinct gpu snapshot every N decisions.", 32, IntRange(1, INT32_MAX));
static IntOption opt_instinct_gpu_submit_every_conflicts(_cat, "instinct-gpu-submit-every-conflicts", "Submit async oracle snapshot every N conflicts (0=disabled, replaces per-pick submit).", 2048, IntRange(0, INT32_MAX));
static BoolOption opt_instinct_gpu_submit_on_restart(_cat, "instinct-gpu-submit-on-restart", "Submit async oracle snapshot at each restart boundary.", false);
static IntOption opt_instinct_gpu_targets(_cat, "instinct-gpu-targets", "Number of target vars per instinct gpu request.", 128, IntRange(1, (int)INSTINCT_GPU_MAX_TARGETS));
static IntOption opt_instinct_gpu_walks(_cat, "instinct-gpu-walks", "Random walks per target for instinct gpu request.", 8, IntRange(1, INT32_MAX));
static IntOption opt_instinct_gpu_max_hint_lag(_cat, "instinct-gpu-max-hint-lag", "Accept online hints that are at most this many request IDs behind the latest processed request (0=strict latest).", 64, IntRange(0, INT32_MAX));
static IntOption opt_instinct_gpu_max_level_drift(_cat, "instinct-gpu-max-level-drift", "Reject online hint when decision level drift exceeds this value.", 64, IntRange(0, INT32_MAX));
static IntOption opt_instinct_gpu_max_assign_drift(_cat, "instinct-gpu-max-assign-drift", "Reject online hint when assigned literal count drift exceeds this value.", 4096, IntRange(0, INT32_MAX));
static BoolOption opt_instinct_gpu_require_top_match(_cat, "instinct-gpu-require-top-match", "Reject online hint when top decision variable/sign no longer matches context.", false);
static BoolOption opt_instinct_gpu_aggressive_pick(_cat, "instinct-gpu-aggressive-pick", "Allow instinct gpu to override the picked variable when a stronger hint exists in a scanned candidate set.", false);
static IntOption opt_instinct_gpu_scan_limit(_cat, "instinct-gpu-scan-limit", "Maximum unassigned candidates scanned for aggressive instinct-gpu variable override.", 256, IntRange(1, INT32_MAX));
static IntOption opt_instinct_gpu_candidate_scan_limit(_cat, "instinct-gpu-candidate-scan-limit", "Maximum heap entries scanned when building candidate list for instinct-gpu requests/VSIDS (0=full heap).", 4096, IntRange(0, INT32_MAX));
static DoubleOption opt_instinct_gpu_aggressive_threshold(_cat, "instinct-gpu-aggressive-threshold", "Confidence threshold for aggressive instinct-gpu variable override.", 0.35, DoubleRange(0, true, 1, true));
static BoolOption opt_instinct_gpu_preempt(_cat, "instinct-gpu-preempt", "Enable pre-emptive backtrack on high-confidence instinct gpu disagreement.", false);
static DoubleOption opt_instinct_gpu_preempt_threshold(_cat, "instinct-gpu-preempt-threshold", "Pre-emptive backtrack when confidence >= threshold and decision sign disagrees.", 0.90, DoubleRange(0, true, 1, true));
static IntOption opt_instinct_gpu_preempt_cooldown(_cat, "instinct-gpu-preempt-cooldown", "Minimum decisions between two pre-emptive backtracks (0=disabled).", 64, IntRange(0, INT32_MAX));
static BoolOption opt_instinct_gpu_tactical_pick(_cat, "instinct-gpu-tactical-pick", "Enable tactical per-pick online hint reads/sign override in pickBranchLit (higher overhead).", false);
static BoolOption opt_instinct_gpu_hard_stop_pick(_cat, "instinct-gpu-hard-stop-pick", "Flip branch sign away from branch predicted highly conflictual (STOP rule).", false);
static DoubleOption opt_instinct_gpu_stop_prob_threshold(_cat, "instinct-gpu-stop-prob-threshold", "STOP rule threshold: predicted conflict probability for chosen sign >= threshold.", 0.90, DoubleRange(0, true, 1, true));
static DoubleOption opt_instinct_gpu_stop_prob_margin(_cat, "instinct-gpu-stop-prob-margin", "STOP rule margin: chosen branch must exceed opposite branch by at least this probability gap.", 0.02, DoubleRange(0, true, 1, true));
static BoolOption opt_instinct_gpu_adaptive_threshold(_cat, "instinct-gpu-adaptive-threshold", "Adapt instinct-gpu thresholds from recent hit/miss quality.", false);
static IntOption opt_instinct_gpu_adapt_window(_cat, "instinct-gpu-adapt-window", "Quality evaluation window (events) for one adaptive threshold update.", 4096, IntRange(64, INT32_MAX));
static DoubleOption opt_instinct_gpu_adapt_low_hit(_cat, "instinct-gpu-adapt-low-hit", "If hit-rate drops below this value, increase thresholds.", 0.30, DoubleRange(0, true, 1, true));
static DoubleOption opt_instinct_gpu_adapt_high_hit(_cat, "instinct-gpu-adapt-high-hit", "If hit-rate rises above this value, decrease thresholds.", 0.60, DoubleRange(0, true, 1, true));
static DoubleOption opt_instinct_gpu_adapt_step(_cat, "instinct-gpu-adapt-step", "Threshold step used by adaptive instinct-gpu tuning.", 0.01, DoubleRange(0, false, 1, true));
static DoubleOption opt_instinct_gpu_adapt_min_threshold(_cat, "instinct-gpu-adapt-min-threshold", "Minimum threshold allowed by adaptive instinct-gpu tuning.", 0.15, DoubleRange(0, true, 1, true));
static DoubleOption opt_instinct_gpu_adapt_max_threshold(_cat, "instinct-gpu-adapt-max-threshold", "Maximum threshold allowed by adaptive instinct-gpu tuning.", 0.98, DoubleRange(0, true, 1, true));
static StringOption opt_instinct_gpu_trace_csv(_cat, "instinct-gpu-trace-csv", "Path to CSV trace for instinct-gpu decision/preempt/conflict events (empty=disabled).");
static IntOption opt_instinct_gpu_trace_max_events(_cat, "instinct-gpu-trace-max-events", "Maximum trace events written to instinct-gpu-trace-csv (0=unlimited).", 200000, IntRange(0, INT32_MAX));
static BoolOption opt_instinct_gpu_trace_flush(_cat, "instinct-gpu-trace-flush", "Flush instinct-gpu-trace-csv on each event (debug mode, slower).", false);
static BoolOption opt_instinct_gpu_vsids_inject(_cat, "instinct-gpu-vsids-inject", "Inject async oracle guidance as VSIDS/phase updates instead of tactical per-pick micromanagement.", true);
static IntOption opt_instinct_gpu_vsids_every_conflicts(_cat, "instinct-gpu-vsids-every-conflicts", "Run oracle->VSIDS injection every N conflicts (0=disabled).", 2048, IntRange(0, INT32_MAX));
static IntOption opt_instinct_gpu_vsids_topk(_cat, "instinct-gpu-vsids-topk", "Maximum top-activity unassigned vars scanned for VSIDS injection.", 128, IntRange(1, INT32_MAX));
static DoubleOption opt_instinct_gpu_vsids_conf_floor(_cat, "instinct-gpu-vsids-conf-floor", "Minimum hint confidence accepted by VSIDS injection.", 0.001, DoubleRange(0, true, 1, true));
static DoubleOption opt_instinct_gpu_vsids_min_gap(_cat, "instinct-gpu-vsids-min-gap", "Minimum |p_true - p_false| gap accepted by VSIDS injection.", 0.0, DoubleRange(0, true, 1, true));
static DoubleOption opt_instinct_gpu_vsids_bump_scale(_cat, "instinct-gpu-vsids-bump-scale", "Scaling factor for VSIDS bumps derived from oracle confidence*gap.", 0.50, DoubleRange(0, true, 100, true));
static IntOption opt_instinct_gpu_vsids_max_bumps(_cat, "instinct-gpu-vsids-max-bumps", "Maximum variables bumped per VSIDS injection round.", 96, IntRange(1, INT32_MAX));
static BoolOption opt_instinct_gpu_vsids_allow_relaxed(_cat, "instinct-gpu-vsids-allow-relaxed", "Allow VSIDS injection fallback from raw shared-memory hints when strict freshness checks fail.", false);
static DoubleOption opt_instinct_gpu_vsids_relaxed_min_freshness(_cat, "instinct-gpu-vsids-relaxed-min-freshness", "Minimum freshness multiplier accepted for relaxed VSIDS fallback hints.", 0.25, DoubleRange(0, true, 1, true));
static BoolOption opt_instinct_gpu_vsids_phase_overwrite(_cat, "instinct-gpu-vsids-phase-overwrite", "Overwrite phase-saving polarity from strong oracle guidance during VSIDS injection.", false);
static BoolOption opt_instinct_gpu_phase_inject(_cat, "instinct-gpu-phase-inject", "Inject oracle hints directly into phase-saving polarity (strategic direct phase control).", false);
static IntOption opt_instinct_gpu_phase_every_conflicts(_cat, "instinct-gpu-phase-every-conflicts", "Run direct phase injection every N conflicts (0=disabled).", 512, IntRange(0, INT32_MAX));
static IntOption opt_instinct_gpu_phase_topk(_cat, "instinct-gpu-phase-topk", "Maximum top-activity unassigned vars scanned for direct phase injection.", 256, IntRange(1, INT32_MAX));
static DoubleOption opt_instinct_gpu_phase_conf_floor(_cat, "instinct-gpu-phase-conf-floor", "Minimum confidence accepted by direct phase injection.", 0.01, DoubleRange(0, true, 1, true));
static DoubleOption opt_instinct_gpu_phase_min_gap(_cat, "instinct-gpu-phase-min-gap", "Minimum |p_true - p_false| gap accepted by direct phase injection.", 0.002, DoubleRange(0, true, 1, true));
static IntOption opt_instinct_gpu_phase_max_updates(_cat, "instinct-gpu-phase-max-updates", "Maximum polarity updates per direct phase-injection round.", 256, IntRange(1, INT32_MAX));
namespace {
static bool instinct_gpu_initialized = false;
static bool instinct_gpu_state_prepared = false;
static int instinct_gpu_fd = -1;
static InstinctGpuSharedMemory* instinct_gpu_shm = NULL;
static uint64_t instinct_gpu_next_request_id = 1;
static bool instinct_gpu_has_submitted_request = false;
static uint64_t instinct_gpu_last_submitted_request_id = 0;
static uint64_t instinct_gpu_last_submit_decisions = 0;
static uint64_t instinct_gpu_last_submit_conflicts = 0;
static int instinct_gpu_requests_submitted = 0;
static int instinct_gpu_requests_dropped = 0;
static int instinct_gpu_hints_seen = 0;
static int instinct_gpu_hints_applied = 0;
static int instinct_gpu_hints_stale = 0;
static int instinct_gpu_hints_stale_lag = 0;
static int instinct_gpu_hints_stale_level = 0;
static int instinct_gpu_hints_stale_assign = 0;
static int instinct_gpu_hints_stale_top = 0;
static int instinct_gpu_preempt_checks = 0;
static int instinct_gpu_preempt_backtracks = 0;
static uint64_t instinct_gpu_last_preempt_decisions = 0;
static int instinct_gpu_preempt_prob_triggers = 0;
static uint64_t instinct_gpu_aggressive_scans = 0;
static int instinct_gpu_aggressive_overrides = 0;
static int instinct_gpu_aggressive_applied = 0;
static int instinct_gpu_hard_stop_flips = 0;
static bool instinct_gpu_trace_initialized = false;
static FILE* instinct_gpu_trace_file = NULL;
static uint64_t instinct_gpu_trace_events = 0;
static uint64_t instinct_gpu_trace_decision_events = 0;
static uint64_t instinct_gpu_trace_decisions_with_hint = 0;
static uint64_t instinct_gpu_trace_decisions_with_applied_hint = 0;
static uint64_t instinct_gpu_trace_decisions_with_aggressive_override = 0;
static uint64_t instinct_gpu_quality_evals = 0;
static uint64_t instinct_gpu_quality_hits = 0;
static uint64_t instinct_gpu_quality_misses = 0;
static uint64_t instinct_gpu_quality_ties = 0;
static uint64_t instinct_gpu_quality_high_conf_evals = 0;
static uint64_t instinct_gpu_quality_high_conf_hits = 0;
static uint64_t instinct_gpu_quality_high_conf_misses = 0;
static double instinct_gpu_quality_sum_chosen_p = 0.0;
static double instinct_gpu_quality_sum_opposite_p = 0.0;
static uint64_t instinct_gpu_quality_applied_evals = 0;
static uint64_t instinct_gpu_quality_applied_low_risk = 0;
static uint64_t instinct_gpu_quality_applied_high_risk = 0;
static uint64_t instinct_gpu_quality_applied_ties = 0;
static bool instinct_gpu_adapt_initialized = false;
static double instinct_gpu_dynamic_threshold = 0.0;
static double instinct_gpu_dynamic_aggressive_threshold = 0.0;
static uint64_t instinct_gpu_adapt_last_evals = 0;
static uint64_t instinct_gpu_adapt_last_hits = 0;
static uint64_t instinct_gpu_adapt_last_misses = 0;
static int instinct_gpu_adapt_updates = 0;
static uint64_t instinct_gpu_vsids_last_conflicts = 0;
static uint64_t instinct_gpu_vsids_rounds = 0;
static uint64_t instinct_gpu_vsids_scanned = 0;
static uint64_t instinct_gpu_vsids_bumped = 0;
static uint64_t instinct_gpu_vsids_phase_updates = 0;
static uint64_t instinct_gpu_vsids_relaxed_hints = 0;
static uint64_t instinct_gpu_phase_last_conflicts = 0;
static uint64_t instinct_gpu_phase_rounds = 0;
static uint64_t instinct_gpu_phase_scanned = 0;
static uint64_t instinct_gpu_phase_updates = 0;
static uint64_t instinct_gpu_phase_relaxed_hints = 0;
struct InstinctGpuHintView {
    uint64_t request_id;
    double confidence;
    double p_conflict_true;
    double p_conflict_false;
    uint32_t decision_level;
    uint32_t assigned_count;
    uint32_t top_decision_var;
    bool top_decision_sign;
    bool prefer_true;
};
struct InstinctGpuPickTrace {
    bool valid;
    Var var;
    bool chosen_sign;
    bool hint_available;
    bool hint_applied;
    bool aggressive_override;
    uint64_t request_id;
    double confidence;
    double p_conflict_true;
    double p_conflict_false;
    bool recommended_sign;
};
struct InstinctGpuDecisionRecord {
    bool valid;
    bool hint_available;
    bool hint_applied;
    bool aggressive_override;
    bool conflict_evaluated;
    Var var;
    bool chosen_sign;
    bool recommended_sign;
    uint64_t request_id;
    double confidence;
    double p_conflict_true;
    double p_conflict_false;
    uint64_t decision_counter;
};
static InstinctGpuPickTrace instinct_gpu_last_pick_trace = {false, var_Undef, false, false, false, false, 0, 0.0, 0.0, 0.0, false};
static std::vector<InstinctGpuDecisionRecord> instinct_gpu_decision_records;
static void closeInstinctGpuShm() {
    if (instinct_gpu_shm != NULL){
        munmap((void*)instinct_gpu_shm, sizeof(InstinctGpuSharedMemory));
        instinct_gpu_shm = NULL;
    }
    if (instinct_gpu_fd >= 0){
        close(instinct_gpu_fd);
        instinct_gpu_fd = -1;
    }
    instinct_gpu_state_prepared = false;
}
static void closeInstinctGpuTrace() {
    if (instinct_gpu_trace_file != NULL){
        fflush(instinct_gpu_trace_file);
        fclose(instinct_gpu_trace_file);
        instinct_gpu_trace_file = NULL;
    }
}
static void ensureInstinctGpuTraceOpened(int verbosity) {
    if (instinct_gpu_trace_initialized)
        return;
    instinct_gpu_trace_initialized = true;
    const char* path = opt_instinct_gpu_trace_csv;
    if (path == NULL || path[0] == '\0')
        return;
    FILE* file = fopen(path, "wb");
    if (file == NULL){
        if (verbosity > 0)
            printf("c [instinct-gpu-trace] could not open: %s\n", path);
        return;
    }
    instinct_gpu_trace_file = file;
    fprintf(instinct_gpu_trace_file,
            "event,decisions,conflicts,level,var,chosen_true,recommended_true,hint_available,hint_applied,aggressive_override,confidence,p_conflict_true,p_conflict_false,request_id,note\n");
    if ((bool)opt_instinct_gpu_trace_flush)
        fflush(instinct_gpu_trace_file);
    atexit(closeInstinctGpuTrace);
    if (verbosity > 0)
        printf("c [instinct-gpu-trace] writing CSV to %s\n", path);
}
static void traceInstinctGpuEvent(const char* event,
                                   uint64_t decisions,
                                   uint64_t conflicts,
                                   int level,
                                   Var var,
                                   int chosen_true,
                                   int recommended_true,
                                   bool hint_available,
                                   bool hint_applied,
                                   bool aggressive_override,
                                   double confidence,
                                   double p_conflict_true,
                                   double p_conflict_false,
                                   uint64_t request_id,
                                   const char* note)
{
    if (instinct_gpu_trace_file == NULL)
        return;
    const int max_events = (int)opt_instinct_gpu_trace_max_events;
    if (max_events > 0 && instinct_gpu_trace_events >= (uint64_t)max_events)
        return;
    fprintf(instinct_gpu_trace_file,
            "%s,%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%" PRIu64 ",%s\n",
            event,
            decisions,
            conflicts,
            level,
            var >= 0 ? (int)var + 1 : -1,
            chosen_true,
            recommended_true,
            hint_available ? 1 : 0,
            hint_applied ? 1 : 0,
            aggressive_override ? 1 : 0,
            confidence,
            p_conflict_true,
            p_conflict_false,
            request_id,
            note != NULL ? note : "");
    instinct_gpu_trace_events++;
    if ((bool)opt_instinct_gpu_trace_flush)
        fflush(instinct_gpu_trace_file);
}
static bool ensureInstinctGpuMapped(int verbosity) {
    if (!(bool)opt_instinct_gpu)
        return false;
    if (instinct_gpu_shm != NULL)
        return true;
    if (instinct_gpu_initialized)
        return false;
    instinct_gpu_initialized = true;
    const char* path = opt_instinct_gpu_shm_path;
    if (path == NULL || path[0] == '\0')
        return false;
    instinct_gpu_fd = open(path, O_RDWR | O_CREAT, 0666);
    if (instinct_gpu_fd < 0){
        if (verbosity > 0)
            printf("c [instinct-gpu] open failed: %s\n", path);
        return false;
    }
    const size_t shm_size = sizeof(InstinctGpuSharedMemory);
    if (ftruncate(instinct_gpu_fd, (off_t)shm_size) != 0){
        if (verbosity > 0)
            printf("c [instinct-gpu] ftruncate failed: %s\n", path);
        closeInstinctGpuShm();
        return false;
    }
    void* mapped = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, instinct_gpu_fd, 0);
    if (mapped == MAP_FAILED){
        if (verbosity > 0)
            printf("c [instinct-gpu] mmap failed: %s\n", path);
        closeInstinctGpuShm();
        return false;
    }
    instinct_gpu_shm = (InstinctGpuSharedMemory*)mapped;
    if (instinct_gpu_shm->magic != INSTINCT_GPU_MAGIC ||
        instinct_gpu_shm->version != INSTINCT_GPU_VERSION){
        memset((void*)instinct_gpu_shm, 0, shm_size);
        instinct_gpu_shm->magic = INSTINCT_GPU_MAGIC;
        instinct_gpu_shm->version = INSTINCT_GPU_VERSION;
        __sync_synchronize();
    }
    if (!instinct_gpu_state_prepared){
        instinctGpuResetSharedState(instinct_gpu_shm);
        instinct_gpu_shm->magic = INSTINCT_GPU_MAGIC;
        instinct_gpu_shm->version = INSTINCT_GPU_VERSION;
        instinct_gpu_has_submitted_request = false;
        instinct_gpu_last_submitted_request_id = 0;
        instinct_gpu_last_submit_decisions = 0;
        instinct_gpu_last_submit_conflicts = 0;
        instinct_gpu_vsids_last_conflicts = 0;
        instinct_gpu_vsids_rounds = 0;
        instinct_gpu_vsids_scanned = 0;
        instinct_gpu_vsids_bumped = 0;
        instinct_gpu_vsids_phase_updates = 0;
        instinct_gpu_vsids_relaxed_hints = 0;
        instinct_gpu_phase_last_conflicts = 0;
        instinct_gpu_phase_rounds = 0;
        instinct_gpu_phase_scanned = 0;
        instinct_gpu_phase_updates = 0;
        instinct_gpu_phase_relaxed_hints = 0;
        instinct_gpu_next_request_id = 1;
        __sync_synchronize();
        instinct_gpu_state_prepared = true;
    }
    atexit(closeInstinctGpuShm);
    if (verbosity > 0)
        printf("c [instinct-gpu] mapped shared memory: %s\n", path);
    return true;
}
static double clampDouble(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
static void ensureInstinctGpuAdaptiveState() {
    if (instinct_gpu_adapt_initialized)
        return;
    instinct_gpu_adapt_initialized = true;
    instinct_gpu_dynamic_threshold = (double)opt_instinct_gpu_threshold;
    instinct_gpu_dynamic_aggressive_threshold = (double)opt_instinct_gpu_aggressive_threshold;
    if (instinct_gpu_dynamic_aggressive_threshold > instinct_gpu_dynamic_threshold)
        instinct_gpu_dynamic_aggressive_threshold = instinct_gpu_dynamic_threshold;
    instinct_gpu_adapt_last_evals = instinct_gpu_quality_high_conf_evals;
    instinct_gpu_adapt_last_hits = instinct_gpu_quality_high_conf_hits;
    instinct_gpu_adapt_last_misses = instinct_gpu_quality_high_conf_misses;
}
static double instinctGpuApplyThreshold() {
    if (!(bool)opt_instinct_gpu_adaptive_threshold)
        return (double)opt_instinct_gpu_threshold;
    ensureInstinctGpuAdaptiveState();
    return instinct_gpu_dynamic_threshold;
}
static void updateInstinctGpuAdaptiveThresholdsIfNeeded() {
    if (!(bool)opt_instinct_gpu_adaptive_threshold)
        return;
    ensureInstinctGpuAdaptiveState();
    const uint64_t window = (uint64_t)(int)opt_instinct_gpu_adapt_window;
    if (window == 0)
        return;
    if (instinct_gpu_quality_high_conf_evals < instinct_gpu_adapt_last_evals + window)
        return;
    const uint64_t evals = instinct_gpu_quality_high_conf_evals - instinct_gpu_adapt_last_evals;
    const uint64_t hits = instinct_gpu_quality_high_conf_hits - instinct_gpu_adapt_last_hits;
    const uint64_t misses = instinct_gpu_quality_high_conf_misses - instinct_gpu_adapt_last_misses;
    const uint64_t informative = hits + misses;
    instinct_gpu_adapt_last_evals = instinct_gpu_quality_high_conf_evals;
    instinct_gpu_adapt_last_hits = instinct_gpu_quality_high_conf_hits;
    instinct_gpu_adapt_last_misses = instinct_gpu_quality_high_conf_misses;
    if (evals == 0 || informative == 0)
        return;
    const double low_hit = (double)opt_instinct_gpu_adapt_low_hit;
    const double high_hit = (double)opt_instinct_gpu_adapt_high_hit;
    if (low_hit > high_hit)
        return;
    const double hit_rate = (double)hits / (double)informative;
    const double step = (double)opt_instinct_gpu_adapt_step;
    const double tmin = (double)opt_instinct_gpu_adapt_min_threshold;
    const double tmax = (double)opt_instinct_gpu_adapt_max_threshold;
    bool changed = false;
    if (hit_rate + 1e-12 < low_hit) {
        instinct_gpu_dynamic_threshold = clampDouble(instinct_gpu_dynamic_threshold + step, tmin, tmax);
        instinct_gpu_dynamic_aggressive_threshold =
            clampDouble(instinct_gpu_dynamic_aggressive_threshold + 0.75 * step, tmin, tmax);
        changed = true;
    } else if (hit_rate > high_hit + 1e-12) {
        instinct_gpu_dynamic_threshold = clampDouble(instinct_gpu_dynamic_threshold - step, tmin, tmax);
        instinct_gpu_dynamic_aggressive_threshold =
            clampDouble(instinct_gpu_dynamic_aggressive_threshold - 0.75 * step, tmin, tmax);
        changed = true;
    }
    if (instinct_gpu_dynamic_aggressive_threshold > instinct_gpu_dynamic_threshold)
        instinct_gpu_dynamic_aggressive_threshold = instinct_gpu_dynamic_threshold;
    if (changed)
        instinct_gpu_adapt_updates++;
}
static bool readInstinctGpuHint(Var v,
                                 InstinctGpuHintView* out,
                                 int solver_decision_level,
                                 int solver_assigned_count,
                                 uint32_t solver_top_decision_var,
                                 bool solver_top_decision_sign,
                                 bool count_stale,
                                 bool count_seen) {
    if (instinct_gpu_shm == NULL)
        return false;
    if (v < 0 || v >= (Var)INSTINCT_GPU_MAX_VARS)
        return false;
    __sync_synchronize();
    const InstinctGpuHint hint = instinct_gpu_shm->hints[(size_t)v];
    if (!hint.has_hint)
        return false;
    if (hint.request_id == 0)
        return false;
    if (instinct_gpu_last_submitted_request_id > 0 &&
        hint.request_id > instinct_gpu_last_submitted_request_id)
        return false;
    if (count_seen)
        instinct_gpu_hints_seen++;
    const uint64_t latest_processed = instinct_gpu_shm->latest_processed_request_id;
    const uint64_t max_hint_lag = (uint64_t)(int)opt_instinct_gpu_max_hint_lag;
    if (latest_processed > 0 && hint.request_id + max_hint_lag < latest_processed){
        if (count_stale){
            instinct_gpu_hints_stale++;
            instinct_gpu_hints_stale_lag++;
        }
        return false;
    }
    if (instinct_gpu_last_submitted_request_id > 0 && latest_processed > 0 &&
        instinct_gpu_last_submitted_request_id >= latest_processed){
        const uint64_t solver_service_lag = instinct_gpu_last_submitted_request_id - latest_processed;
        if (solver_service_lag <= max_hint_lag &&
            hint.request_id + max_hint_lag < instinct_gpu_last_submitted_request_id){
            if (count_stale){
                instinct_gpu_hints_stale++;
                instinct_gpu_hints_stale_lag++;
            }
            return false;
        }
    }
    const int max_level_drift = (int)opt_instinct_gpu_max_level_drift;
    if (max_level_drift >= 0 && solver_decision_level >= 0){
        const int hint_level = (int)hint.decision_level;
        if (abs(hint_level - solver_decision_level) > max_level_drift){
            if (count_stale){
                instinct_gpu_hints_stale++;
                instinct_gpu_hints_stale_level++;
            }
            return false;
        }
    }
    const int max_assign_drift = (int)opt_instinct_gpu_max_assign_drift;
    if (max_assign_drift >= 0 && solver_assigned_count >= 0){
        const int hint_assigned = (int)hint.assigned_count;
        if (abs(hint_assigned - solver_assigned_count) > max_assign_drift){
            if (count_stale){
                instinct_gpu_hints_stale++;
                instinct_gpu_hints_stale_assign++;
            }
            return false;
        }
    }
    if ((bool)opt_instinct_gpu_require_top_match && solver_decision_level > 0){
        const uint32_t hint_top_var = hint.top_decision_var;
        if (hint_top_var > 0 && solver_top_decision_var > 0 &&
            (hint_top_var != solver_top_decision_var ||
             (hint.top_decision_sign ? true : false) != solver_top_decision_sign)){
            if (count_stale){
                instinct_gpu_hints_stale++;
                instinct_gpu_hints_stale_top++;
            }
            return false;
        }
    }
    out->request_id = hint.request_id;
    out->confidence = (double)hint.confidence;
    out->p_conflict_true = (double)hint.p_conflict_true;
    out->p_conflict_false = (double)hint.p_conflict_false;
    out->decision_level = hint.decision_level;
    out->assigned_count = hint.assigned_count;
    out->top_decision_var = hint.top_decision_var;
    out->top_decision_sign = hint.top_decision_sign ? true : false;
    out->prefer_true = hint.prefer_true ? true : false;
    return true;
}
static void resetOracleRuntimeState() {
    closeInstinctGpuTrace();
    closeInstinctGpuShm();
    instinct_gpu_initialized = false;
    instinct_gpu_state_prepared = false;
    instinct_gpu_next_request_id = 1;
    instinct_gpu_has_submitted_request = false;
    instinct_gpu_last_submitted_request_id = 0;
    instinct_gpu_last_submit_decisions = 0;
    instinct_gpu_last_submit_conflicts = 0;
    instinct_gpu_requests_submitted = 0;
    instinct_gpu_requests_dropped = 0;
    instinct_gpu_hints_seen = 0;
    instinct_gpu_hints_applied = 0;
    instinct_gpu_hints_stale = 0;
    instinct_gpu_hints_stale_lag = 0;
    instinct_gpu_hints_stale_level = 0;
    instinct_gpu_hints_stale_assign = 0;
    instinct_gpu_hints_stale_top = 0;
    instinct_gpu_preempt_checks = 0;
    instinct_gpu_preempt_backtracks = 0;
    instinct_gpu_last_preempt_decisions = 0;
    instinct_gpu_preempt_prob_triggers = 0;
    instinct_gpu_aggressive_scans = 0;
    instinct_gpu_aggressive_overrides = 0;
    instinct_gpu_aggressive_applied = 0;
    instinct_gpu_hard_stop_flips = 0;
    instinct_gpu_trace_initialized = false;
    instinct_gpu_trace_file = NULL;
    instinct_gpu_trace_events = 0;
    instinct_gpu_trace_decision_events = 0;
    instinct_gpu_trace_decisions_with_hint = 0;
    instinct_gpu_trace_decisions_with_applied_hint = 0;
    instinct_gpu_trace_decisions_with_aggressive_override = 0;
    instinct_gpu_quality_evals = 0;
    instinct_gpu_quality_hits = 0;
    instinct_gpu_quality_misses = 0;
    instinct_gpu_quality_ties = 0;
    instinct_gpu_quality_high_conf_evals = 0;
    instinct_gpu_quality_high_conf_hits = 0;
    instinct_gpu_quality_high_conf_misses = 0;
    instinct_gpu_quality_sum_chosen_p = 0.0;
    instinct_gpu_quality_sum_opposite_p = 0.0;
    instinct_gpu_quality_applied_evals = 0;
    instinct_gpu_quality_applied_low_risk = 0;
    instinct_gpu_quality_applied_high_risk = 0;
    instinct_gpu_quality_applied_ties = 0;
    instinct_gpu_adapt_initialized = false;
    instinct_gpu_dynamic_threshold = 0.0;
    instinct_gpu_dynamic_aggressive_threshold = 0.0;
    instinct_gpu_adapt_last_evals = 0;
    instinct_gpu_adapt_last_hits = 0;
    instinct_gpu_adapt_last_misses = 0;
    instinct_gpu_adapt_updates = 0;
    instinct_gpu_vsids_last_conflicts = 0;
    instinct_gpu_vsids_rounds = 0;
    instinct_gpu_vsids_scanned = 0;
    instinct_gpu_vsids_bumped = 0;
    instinct_gpu_vsids_phase_updates = 0;
    instinct_gpu_vsids_relaxed_hints = 0;
    instinct_gpu_phase_last_conflicts = 0;
    instinct_gpu_phase_rounds = 0;
    instinct_gpu_phase_scanned = 0;
    instinct_gpu_phase_updates = 0;
    instinct_gpu_phase_relaxed_hints = 0;
    instinct_gpu_last_pick_trace = {false, var_Undef, false, false, false, false, 0, 0.0, 0.0, 0.0, false};
    instinct_gpu_decision_records.clear();
}
}
Solver::Solver() :
    verbosity (0)
  , var_decay (opt_var_decay)
  , clause_decay (opt_clause_decay)
  , random_var_freq (opt_random_var_freq)
  , random_seed (opt_random_seed)
  , luby_restart (opt_luby_restart)
  , ccmin_mode (opt_ccmin_mode)
  , phase_saving (opt_phase_saving)
  , rnd_pol (false)
  , rnd_init_act (opt_rnd_init_act)
  , garbage_frac (opt_garbage_frac)
  , min_learnts_lim (opt_min_learnts_lim)
  , restart_first (opt_restart_first)
  , restart_inc (opt_restart_inc)
  , learntsize_factor((double)1/(double)3), learntsize_inc(1.1)
  , learntsize_adjust_start_confl (100)
  , learntsize_adjust_inc (1.5)
  , solves(0), starts(0), decisions(0), rnd_decisions(0), propagations(0), conflicts(0)
  , dec_vars(0), num_clauses(0), num_learnts(0), clauses_literals(0), learnts_literals(0), max_literals(0), tot_literals(0)
  , watches (WatcherDeleted(ca))
  , order_heap (VarOrderLt(activity))
  , ok (true)
  , cla_inc (1)
  , var_inc (1)
  , qhead (0)
  , simpDB_assigns (-1)
  , simpDB_props (0)
  , progress_estimate (0)
  , remove_satisfied (true)
  , next_var (0)
  , conflict_budget (-1)
  , propagation_budget (-1)
  , asynch_interrupt (false)
{
    resetOracleRuntimeState();
}
Solver::~Solver()
{
}
Var Solver::newVar(lbool upol, bool dvar)
{
    Var v;
    if (free_vars.size() > 0){
        v = free_vars.last();
        free_vars.pop();
    }else
        v = next_var++;
    watches .init(mkLit(v, false));
    watches .init(mkLit(v, true ));
    assigns .insert(v, l_Undef);
    vardata .insert(v, mkVarData(CRef_Undef, 0));
    activity .insert(v, rnd_init_act ? drand(random_seed) * 0.00001 : 0);
    seen .insert(v, 0);
    polarity .insert(v, true);
    user_pol .insert(v, upol);
    decision .reserve(v);
    trail .capacity(v+1);
    setDecisionVar(v, dvar);
    return v;
}
void Solver::releaseVar(Lit l)
{
    if (value(l) == l_Undef){
        addClause(l);
        released_vars.push(var(l));
    }
}
bool Solver::addClause_(vec<Lit>& ps)
{
    assert(decisionLevel() == 0);
    if (!ok) return false;
    sort(ps);
    Lit p; int i, j;
    for (i = j = 0, p = lit_Undef; i < ps.size(); i++)
        if (value(ps[i]) == l_True || ps[i] == ~p)
            return true;
        else if (value(ps[i]) != l_False && ps[i] != p)
            ps[j++] = p = ps[i];
    ps.shrink(i - j);
    if (ps.size() == 0)
        return ok = false;
    else if (ps.size() == 1){
        uncheckedEnqueue(ps[0]);
        return ok = (propagate() == CRef_Undef);
    }else{
        CRef cr = ca.alloc(ps, false);
        clauses.push(cr);
        attachClause(cr);
    }
    return true;
}
void Solver::attachClause(CRef cr){
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    watches[~c[0]].push(Watcher(cr, c[1]));
    watches[~c[1]].push(Watcher(cr, c[0]));
    if (c.learnt()) num_learnts++, learnts_literals += c.size();
    else num_clauses++, clauses_literals += c.size();
}
void Solver::detachClause(CRef cr, bool strict){
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    if (strict){
        remove(watches[~c[0]], Watcher(cr, c[1]));
        remove(watches[~c[1]], Watcher(cr, c[0]));
    }else{
        watches.smudge(~c[0]);
        watches.smudge(~c[1]);
    }
    if (c.learnt()) num_learnts--, learnts_literals -= c.size();
    else num_clauses--, clauses_literals -= c.size();
}
void Solver::removeClause(CRef cr) {
    Clause& c = ca[cr];
    detachClause(cr);
    if (locked(c)) vardata[var(c[0])].reason = CRef_Undef;
    c.mark(1);
    ca.free(cr);
}
bool Solver::satisfied(const Clause& c) const {
    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) == l_True)
            return true;
    return false; }
void Solver::cancelUntil(int level) {
    if (decisionLevel() > level){
        for (int c = trail.size()-1; c >= trail_lim[level]; c--){
            Var x = var(trail[c]);
            assigns [x] = l_Undef;
            if (phase_saving > 1 || (phase_saving == 1 && c > trail_lim.last()))
                polarity[x] = sign(trail[c]);
            insertVarOrder(x); }
        qhead = trail_lim[level];
        trail.shrink(trail.size() - trail_lim[level]);
        trail_lim.shrink(trail_lim.size() - level);
        const int keep_levels = level + 1;
        if (keep_levels <= 0)
            instinct_gpu_decision_records.clear();
        else if ((int)instinct_gpu_decision_records.size() > keep_levels)
            instinct_gpu_decision_records.resize((size_t)keep_levels);
    } }
void Solver::collectTopUnassignedDecisionVars(int limit, vec<Var>& out)
{
    out.clear();
    if (limit <= 0 || order_heap.empty())
        return;
    std::vector<Var> candidates;
    const int configured_scan = (int)opt_instinct_gpu_candidate_scan_limit;
    const int heap_scan_limit =
        configured_scan <= 0 ? order_heap.size() : std::min(order_heap.size(), configured_scan);
    candidates.reserve((size_t)heap_scan_limit);
    for (int i = 0; i < heap_scan_limit; i++){
        const Var v = order_heap[i];
        if (v < 0 || v >= nVars())
            continue;
        if (assigns[v] != l_Undef)
            continue;
        if (!decision[v])
            continue;
        candidates.push_back(v);
    }
    if (candidates.empty())
        return;
    const int want = std::min(limit, (int)candidates.size());
    std::partial_sort(candidates.begin(),
                      candidates.begin() + want,
                      candidates.end(),
                      [this](Var a, Var b) {
                          const double aa = activity[a];
                          const double bb = activity[b];
                          if (aa == bb)
                              return a < b;
                          return aa > bb;
                      });
    for (int i = 0; i < want; i++)
        out.push(candidates[(size_t)i]);
}
void Solver::submitInstinctGpuRequest(bool force)
{
    if (!(bool)opt_instinct_gpu)
        return;
    ensureInstinctGpuMapped(verbosity);
    if (instinct_gpu_shm == NULL)
        return;
    const int conflicts_every = (int)opt_instinct_gpu_submit_every_conflicts;
    if (!force){
        const int decisions_every = std::max(1, (int)opt_instinct_gpu_request_every);
        if (conflicts_every <= 0){
            if (instinct_gpu_has_submitted_request){
                if (decisions < instinct_gpu_last_submit_decisions + (uint64_t)decisions_every)
                    return;
            }else{
                if (decisions < (uint64_t)decisions_every)
                    return;
            }
        }else{
            const bool due_by_conflicts =
                conflicts >= instinct_gpu_last_submit_conflicts + (uint64_t)conflicts_every;
            const bool bootstrap_due =
                !instinct_gpu_has_submitted_request &&
                decisions >= (uint64_t)decisions_every;
            if (!due_by_conflicts && !bootstrap_due)
                return;
        }
    }
    const int solver_level = decisionLevel();
    const int solver_assigned = nAssigns();
    uint32_t solver_top_decision_var = 0;
    bool solver_top_decision_sign = false;
    if (solver_level > 0){
        const int top_index = trail_lim[solver_level - 1];
        if (top_index >= 0 && top_index < trail.size()){
            const Lit top_lit = trail[top_index];
            solver_top_decision_var = (uint32_t)(var(top_lit) + 1);
            solver_top_decision_sign = sign(top_lit) ? true : false;
        }
    }
    int request_walks = std::max(1, (int)opt_instinct_gpu_walks);
    const uint32_t write_seq_now = instinct_gpu_shm->write_seq;
    const uint32_t read_seq_now = instinct_gpu_shm->read_seq;
    const uint32_t backlog = write_seq_now - read_seq_now;
    const uint64_t latest_processed = instinct_gpu_shm->latest_processed_request_id;
    const uint64_t max_hint_lag = (uint64_t)std::max(1, (int)opt_instinct_gpu_max_hint_lag);
    uint64_t service_lag = 0;
    if (instinct_gpu_last_submitted_request_id > latest_processed)
        service_lag = instinct_gpu_last_submitted_request_id - latest_processed;
    if (solver_level <= 4 && request_walks < 64)
        request_walks = std::min(64, request_walks * 2);
    if (backlog >= INSTINCT_GPU_RING_CAPACITY / 4 || service_lag > 2 * max_hint_lag)
        request_walks = std::max(1, request_walks / 2);
    if (backlog >= INSTINCT_GPU_RING_CAPACITY / 2 || service_lag > 4 * max_hint_lag)
        request_walks = std::max(1, request_walks / 2);
    if (backlog >= (INSTINCT_GPU_RING_CAPACITY * 3) / 4 || service_lag > 8 * max_hint_lag)
        request_walks = std::max(1, request_walks / 2);
    request_walks = std::max(1, std::min(64, request_walks));
    if (instinct_gpu_has_submitted_request &&
        latest_processed == 0 &&
        instinct_gpu_requests_dropped > 0){
        instinct_gpu_last_submit_decisions = decisions;
        instinct_gpu_last_submit_conflicts = conflicts;
        return;
    }
    const uint32_t write_seq = instinct_gpu_shm->write_seq;
    const uint32_t read_seq = instinct_gpu_shm->read_seq;
    if (write_seq - read_seq >= INSTINCT_GPU_RING_CAPACITY){
        instinct_gpu_requests_dropped++;
        instinct_gpu_last_submit_decisions = decisions;
        instinct_gpu_last_submit_conflicts = conflicts;
        return;
    }
    const int max_targets = (int)opt_instinct_gpu_targets;
    const uint32_t slot = write_seq % INSTINCT_GPU_RING_CAPACITY;
    InstinctGpuRequest* req = &instinct_gpu_shm->requests[slot];
    const uint64_t request_id = instinct_gpu_next_request_id++;
    req->request_id = request_id;
    req->num_vars = nVars() > (int)INSTINCT_GPU_MAX_VARS ? INSTINCT_GPU_MAX_VARS : (uint32_t)nVars();
    req->walks = (uint32_t)request_walks;
    req->decision_level = (uint32_t)solver_level;
    req->assigned_count = (uint32_t)solver_assigned;
    req->top_decision_var = solver_top_decision_var;
    req->top_decision_sign = solver_top_decision_sign ? 1 : 0;
    const int target_candidate_budget = max_targets > INT32_MAX / 4 ? INT32_MAX : max_targets * 4;
    vec<Var> target_candidates;
    collectTopUnassignedDecisionVars(target_candidate_budget, target_candidates);
    int target_count = 0;
    for (int i = 0; i < target_candidates.size() && target_count < max_targets; i++){
        const Var candidate = target_candidates[i];
        if (candidate < 0 || candidate >= nVars())
            continue;
        if (candidate >= (Var)INSTINCT_GPU_MAX_VARS)
            continue;
        req->targets[target_count++] = (uint32_t)(candidate + 1);
    }
    req->num_targets = (uint32_t)target_count;
    for (int v = 0; v < (int)req->num_vars; v++){
        if (assigns[v] == l_True){
            req->assignment[v] = 1;
            req->preferred_phase[v] = 1;
        }else if (assigns[v] == l_False){
            req->assignment[v] = -1;
            req->preferred_phase[v] = -1;
        }else{
            req->assignment[v] = 0;
            if (user_pol[v] != l_Undef){
                req->preferred_phase[v] = (user_pol[v] == l_True) ? 1 : -1;
            }else{
                req->preferred_phase[v] = polarity[v] ? -1 : 1;
            }
        }
    }
    if (req->num_targets == 0){
        instinct_gpu_requests_dropped++;
        instinct_gpu_last_submit_decisions = decisions;
        instinct_gpu_last_submit_conflicts = conflicts;
        return;
    }
    __sync_synchronize();
    instinct_gpu_shm->write_seq = write_seq + 1;
    instinct_gpu_has_submitted_request = true;
    instinct_gpu_last_submit_decisions = decisions;
    instinct_gpu_last_submit_conflicts = conflicts;
    instinct_gpu_last_submitted_request_id = request_id;
    instinct_gpu_requests_submitted++;
}
void Solver::applyInstinctGpuVsidsInjection()
{
    if (!(bool)opt_instinct_gpu)
        return;
    if (!(bool)opt_instinct_gpu_vsids_inject)
        return;
    const int every_conflicts = (int)opt_instinct_gpu_vsids_every_conflicts;
    if (every_conflicts <= 0)
        return;
    if (conflicts < instinct_gpu_vsids_last_conflicts + (uint64_t)every_conflicts)
        return;
    ensureInstinctGpuMapped(verbosity);
    if (instinct_gpu_shm == NULL)
        return;
    const int topk = std::max(1, (int)opt_instinct_gpu_vsids_topk);
    const double conf_floor = (double)opt_instinct_gpu_vsids_conf_floor;
    const double min_gap = (double)opt_instinct_gpu_vsids_min_gap;
    const double bump_scale = (double)opt_instinct_gpu_vsids_bump_scale;
    const int max_bumps = std::max(1, (int)opt_instinct_gpu_vsids_max_bumps);
    const bool allow_relaxed = (bool)opt_instinct_gpu_vsids_allow_relaxed;
    const double relaxed_min_freshness = (double)opt_instinct_gpu_vsids_relaxed_min_freshness;
    const double online_threshold = instinctGpuApplyThreshold();
    int scanned = 0;
    int bumped = 0;
    int phase_updates = 0;
    const int topk_candidate_budget = topk > INT32_MAX / 4 ? INT32_MAX : topk * 4;
    vec<Var> top_candidates;
    collectTopUnassignedDecisionVars(topk_candidate_budget, top_candidates);
    for (int i = 0; i < top_candidates.size() && scanned < topk && bumped < max_bumps; i++){
        Var candidate = top_candidates[i];
        if (candidate < 0 || candidate >= nVars())
            continue;
        if (candidate >= (Var)INSTINCT_GPU_MAX_VARS)
            continue;
        scanned++;
        InstinctGpuHintView hint;
        bool hint_ok = readInstinctGpuHint(candidate,
                                            &hint,
                                            -1,
                                            -1,
                                            0,
                                            false,
                                            false,
                                            false);
        if (!hint_ok && allow_relaxed){
            __sync_synchronize();
            const InstinctGpuHint raw = instinct_gpu_shm->hints[(size_t)candidate];
            if (raw.has_hint &&
                raw.request_id > 0 &&
                (instinct_gpu_last_submitted_request_id == 0 ||
                 raw.request_id <= instinct_gpu_last_submitted_request_id)){
                hint.request_id = raw.request_id;
                hint.confidence = (double)raw.confidence;
                hint.p_conflict_true = (double)raw.p_conflict_true;
                hint.p_conflict_false = (double)raw.p_conflict_false;
                hint.decision_level = raw.decision_level;
                hint.assigned_count = raw.assigned_count;
                hint.top_decision_var = raw.top_decision_var;
                hint.top_decision_sign = raw.top_decision_sign ? true : false;
                hint.prefer_true = raw.prefer_true ? true : false;
                if (instinct_gpu_last_submitted_request_id > 0 &&
                    instinct_gpu_last_submitted_request_id > raw.request_id){
                    const uint64_t lag = instinct_gpu_last_submitted_request_id - raw.request_id;
                    double freshness = 1.0 / (1.0 + 0.02 * (double)lag);
                    if (freshness < relaxed_min_freshness)
                        continue;
                    hint.confidence *= freshness;
                }
                instinct_gpu_vsids_relaxed_hints++;
                hint_ok = true;
            }
        }
        if (!hint_ok)
            continue;
        const double confidence = hint.confidence;
        const double gap = fabs(hint.p_conflict_true - hint.p_conflict_false);
        if (confidence + 1e-12 < conf_floor)
            continue;
        if (gap + 1e-12 < min_gap)
            continue;
        const double bump = bump_scale * confidence * gap;
        if (bump > 0.0){
            varBumpActivity(candidate, bump);
            bumped++;
        }
        if ((bool)opt_instinct_gpu_vsids_phase_overwrite &&
            confidence + 1e-12 >= online_threshold){
            const bool recommended_sign = hint.prefer_true ? false : true;
            if ((bool)polarity[candidate] != recommended_sign){
                polarity[candidate] = recommended_sign;
                phase_updates++;
            }
        }
    }
    instinct_gpu_vsids_last_conflicts = conflicts;
    instinct_gpu_vsids_rounds++;
    instinct_gpu_vsids_scanned += (uint64_t)scanned;
    instinct_gpu_vsids_bumped += (uint64_t)bumped;
    instinct_gpu_vsids_phase_updates += (uint64_t)phase_updates;
}
void Solver::applyInstinctGpuPhaseInjection()
{
    if (!(bool)opt_instinct_gpu)
        return;
    if (!(bool)opt_instinct_gpu_phase_inject)
        return;
    const int every_conflicts = (int)opt_instinct_gpu_phase_every_conflicts;
    if (every_conflicts <= 0)
        return;
    if (conflicts < instinct_gpu_phase_last_conflicts + (uint64_t)every_conflicts)
        return;
    ensureInstinctGpuMapped(verbosity);
    if (instinct_gpu_shm == NULL)
        return;
    const int topk = std::max(1, (int)opt_instinct_gpu_phase_topk);
    const int max_updates = std::max(1, (int)opt_instinct_gpu_phase_max_updates);
    const double conf_floor = (double)opt_instinct_gpu_phase_conf_floor;
    const double min_gap = (double)opt_instinct_gpu_phase_min_gap;
    int scanned = 0;
    int updates = 0;
    const int topk_candidate_budget = topk > INT32_MAX / 4 ? INT32_MAX : topk * 4;
    vec<Var> top_candidates;
    collectTopUnassignedDecisionVars(topk_candidate_budget, top_candidates);
    for (int i = 0; i < top_candidates.size() && scanned < topk && updates < max_updates; i++){
        Var candidate = top_candidates[i];
        if (candidate < 0 || candidate >= nVars())
            continue;
        if (candidate >= (Var)INSTINCT_GPU_MAX_VARS)
            continue;
        scanned++;
        InstinctGpuHintView hint;
        bool hint_ok = readInstinctGpuHint(candidate,
                                            &hint,
                                            -1,
                                            -1,
                                            0,
                                            false,
                                            false,
                                            false);
        if (!hint_ok){
            __sync_synchronize();
            const InstinctGpuHint raw = instinct_gpu_shm->hints[(size_t)candidate];
            if (raw.has_hint &&
                raw.request_id > 0 &&
                (instinct_gpu_last_submitted_request_id == 0 ||
                 raw.request_id <= instinct_gpu_last_submitted_request_id)){
                hint.request_id = raw.request_id;
                hint.confidence = (double)raw.confidence;
                hint.p_conflict_true = (double)raw.p_conflict_true;
                hint.p_conflict_false = (double)raw.p_conflict_false;
                hint.decision_level = raw.decision_level;
                hint.assigned_count = raw.assigned_count;
                hint.top_decision_var = raw.top_decision_var;
                hint.top_decision_sign = raw.top_decision_sign ? true : false;
                hint.prefer_true = raw.prefer_true ? true : false;
                if (instinct_gpu_last_submitted_request_id > 0 &&
                    instinct_gpu_last_submitted_request_id > raw.request_id){
                    const uint64_t lag = instinct_gpu_last_submitted_request_id - raw.request_id;
                    double freshness = 1.0 / (1.0 + 0.02 * (double)lag);
                    if (freshness < 0.05)
                        freshness = 0.05;
                    hint.confidence *= freshness;
                }
                instinct_gpu_phase_relaxed_hints++;
                hint_ok = true;
            }
        }
        if (!hint_ok)
            continue;
        const double confidence = hint.confidence;
        const double gap = fabs(hint.p_conflict_true - hint.p_conflict_false);
        if (confidence + 1e-12 < conf_floor)
            continue;
        if (gap + 1e-12 < min_gap)
            continue;
        const bool recommended_sign = hint.prefer_true ? false : true;
        if ((bool)polarity[candidate] != recommended_sign){
            polarity[candidate] = recommended_sign;
            updates++;
        }
    }
    instinct_gpu_phase_last_conflicts = conflicts;
    instinct_gpu_phase_rounds++;
    instinct_gpu_phase_scanned += (uint64_t)scanned;
    instinct_gpu_phase_updates += (uint64_t)updates;
}
Lit Solver::pickBranchLit()
{
    instinct_gpu_last_pick_trace.valid = false;
    Var next = var_Undef;
    if (drand(random_seed) < random_var_freq && !order_heap.empty()){
        next = order_heap[irand(random_seed,order_heap.size())];
        if (value(next) == l_Undef && decision[next])
            rnd_decisions++; }
    while (next == var_Undef || value(next) != l_Undef || !decision[next])
        if (order_heap.empty()){
            next = var_Undef;
            break;
        }else
            next = order_heap.removeMin();
    if (next == var_Undef)
        return lit_Undef;
    const bool chosen_sign = (user_pol[next] != l_Undef)
                                 ? (user_pol[next] == l_True)
                                 : (rnd_pol ? (drand(random_seed) < 0.5) : polarity[next]);
    instinct_gpu_last_pick_trace.valid = true;
    instinct_gpu_last_pick_trace.var = next;
    instinct_gpu_last_pick_trace.chosen_sign = chosen_sign;
    instinct_gpu_last_pick_trace.hint_available = false;
    instinct_gpu_last_pick_trace.hint_applied = false;
    instinct_gpu_last_pick_trace.aggressive_override = false;
    instinct_gpu_last_pick_trace.request_id = 0;
    instinct_gpu_last_pick_trace.confidence = 0.0;
    instinct_gpu_last_pick_trace.p_conflict_true = 0.0;
    instinct_gpu_last_pick_trace.p_conflict_false = 0.0;
    instinct_gpu_last_pick_trace.recommended_sign = chosen_sign;
    return mkLit(next, chosen_sign);
}
void Solver::analyze(CRef confl, vec<Lit>& out_learnt, int& out_btlevel)
{
    int pathC = 0;
    Lit p = lit_Undef;
    out_learnt.push();
    int index = trail.size() - 1;
    do{
        assert(confl != CRef_Undef);
        Clause& c = ca[confl];
        if (c.learnt())
            claBumpActivity(c);
        for (int j = (p == lit_Undef) ? 0 : 1; j < c.size(); j++){
            Lit q = c[j];
            if (!seen[var(q)] && level(var(q)) > 0){
                varBumpActivity(var(q));
                seen[var(q)] = 1;
                if (level(var(q)) >= decisionLevel())
                    pathC++;
                else
                    out_learnt.push(q);
            }
        }
        while (!seen[var(trail[index--])]);
        p = trail[index+1];
        confl = reason(var(p));
        seen[var(p)] = 0;
        pathC--;
    }while (pathC > 0);
    out_learnt[0] = ~p;
    int i, j;
    out_learnt.copyTo(analyze_toclear);
    if (ccmin_mode == 2){
        for (i = j = 1; i < out_learnt.size(); i++)
            if (reason(var(out_learnt[i])) == CRef_Undef || !litRedundant(out_learnt[i]))
                out_learnt[j++] = out_learnt[i];
    }else if (ccmin_mode == 1){
        for (i = j = 1; i < out_learnt.size(); i++){
            Var x = var(out_learnt[i]);
            if (reason(x) == CRef_Undef)
                out_learnt[j++] = out_learnt[i];
            else{
                Clause& c = ca[reason(var(out_learnt[i]))];
                for (int k = 1; k < c.size(); k++)
                    if (!seen[var(c[k])] && level(var(c[k])) > 0){
                        out_learnt[j++] = out_learnt[i];
                        break; }
            }
        }
    }else
        i = j = out_learnt.size();
    max_literals += out_learnt.size();
    out_learnt.shrink(i - j);
    tot_literals += out_learnt.size();
    if (out_learnt.size() == 1)
        out_btlevel = 0;
    else{
        int max_i = 1;
        for (int i = 2; i < out_learnt.size(); i++)
            if (level(var(out_learnt[i])) > level(var(out_learnt[max_i])))
                max_i = i;
        Lit p = out_learnt[max_i];
        out_learnt[max_i] = out_learnt[1];
        out_learnt[1] = p;
        out_btlevel = level(var(p));
    }
    for (int j = 0; j < analyze_toclear.size(); j++) seen[var(analyze_toclear[j])] = 0;
}
bool Solver::litRedundant(Lit p)
{
    enum { seen_undef = 0, seen_source = 1, seen_removable = 2, seen_failed = 3 };
    assert(seen[var(p)] == seen_undef || seen[var(p)] == seen_source);
    assert(reason(var(p)) != CRef_Undef);
    Clause* c = &ca[reason(var(p))];
    vec<ShrinkStackElem>& stack = analyze_stack;
    stack.clear();
    for (uint32_t i = 1; ; i++){
        if (i < (uint32_t)c->size()){
            Lit l = (*c)[i];
            if (level(var(l)) == 0 || seen[var(l)] == seen_source || seen[var(l)] == seen_removable){
                continue; }
            if (reason(var(l)) == CRef_Undef || seen[var(l)] == seen_failed){
                stack.push(ShrinkStackElem(0, p));
                for (int i = 0; i < stack.size(); i++)
                    if (seen[var(stack[i].l)] == seen_undef){
                        seen[var(stack[i].l)] = seen_failed;
                        analyze_toclear.push(stack[i].l);
                    }
                return false;
            }
            stack.push(ShrinkStackElem(i, p));
            i = 0;
            p = l;
            c = &ca[reason(var(p))];
        }else{
            if (seen[var(p)] == seen_undef){
                seen[var(p)] = seen_removable;
                analyze_toclear.push(p);
            }
            if (stack.size() == 0) break;
            i = stack.last().i;
            p = stack.last().l;
            c = &ca[reason(var(p))];
            stack.pop();
        }
    }
    return true;
}
void Solver::analyzeFinal(Lit p, LSet& out_conflict)
{
    out_conflict.clear();
    out_conflict.insert(p);
    if (decisionLevel() == 0)
        return;
    seen[var(p)] = 1;
    for (int i = trail.size()-1; i >= trail_lim[0]; i--){
        Var x = var(trail[i]);
        if (seen[x]){
            if (reason(x) == CRef_Undef){
                assert(level(x) > 0);
                out_conflict.insert(~trail[i]);
            }else{
                Clause& c = ca[reason(x)];
                for (int j = 1; j < c.size(); j++)
                    if (level(var(c[j])) > 0)
                        seen[var(c[j])] = 1;
            }
            seen[x] = 0;
        }
    }
    seen[var(p)] = 0;
}
void Solver::uncheckedEnqueue(Lit p, CRef from)
{
    assert(value(p) == l_Undef);
    assigns[var(p)] = lbool(!sign(p));
    vardata[var(p)] = mkVarData(from, decisionLevel());
    trail.push_(p);
}
CRef Solver::propagate()
{
    CRef confl = CRef_Undef;
    int num_props = 0;
    while (qhead < trail.size()){
        Lit p = trail[qhead++];
        vec<Watcher>& ws = watches.lookup(p);
        Watcher *i, *j, *end;
        num_props++;
        for (i = j = (Watcher*)ws, end = i + ws.size(); i != end;){
            Lit blocker = i->blocker;
            if (value(blocker) == l_True){
                *j++ = *i++; continue; }
            CRef cr = i->cref;
            Clause& c = ca[cr];
            Lit false_lit = ~p;
            if (c[0] == false_lit)
                c[0] = c[1], c[1] = false_lit;
            assert(c[1] == false_lit);
            i++;
            Lit first = c[0];
            Watcher w = Watcher(cr, first);
            if (first != blocker && value(first) == l_True){
                *j++ = w; continue; }
            for (int k = 2; k < c.size(); k++)
                if (value(c[k]) != l_False){
                    c[1] = c[k]; c[k] = false_lit;
                    watches[~c[1]].push(w);
                    goto NextClause; }
            *j++ = w;
            if (value(first) == l_False){
                confl = cr;
                qhead = trail.size();
                while (i < end)
                    *j++ = *i++;
            }else
                uncheckedEnqueue(first, cr);
        NextClause:;
        }
        ws.shrink(i - j);
    }
    propagations += num_props;
    simpDB_props -= num_props;
    return confl;
}
struct reduceDB_lt {
    ClauseAllocator& ca;
    reduceDB_lt(ClauseAllocator& ca_) : ca(ca_) {}
    bool operator () (CRef x, CRef y) {
        return ca[x].size() > 2 && (ca[y].size() == 2 || ca[x].activity() < ca[y].activity()); }
};
void Solver::reduceDB()
{
    int i, j;
    double extra_lim = cla_inc / learnts.size();
    sort(learnts, reduceDB_lt(ca));
    for (i = j = 0; i < learnts.size(); i++){
        Clause& c = ca[learnts[i]];
        if (c.size() > 2 && !locked(c) && (i < learnts.size() / 2 || c.activity() < extra_lim))
            removeClause(learnts[i]);
        else
            learnts[j++] = learnts[i];
    }
    learnts.shrink(i - j);
    checkGarbage();
}
void Solver::removeSatisfied(vec<CRef>& cs)
{
    int i, j;
    for (i = j = 0; i < cs.size(); i++){
        Clause& c = ca[cs[i]];
        if (satisfied(c))
            removeClause(cs[i]);
        else{
            assert(value(c[0]) == l_Undef && value(c[1]) == l_Undef);
            for (int k = 2; k < c.size(); k++)
                if (value(c[k]) == l_False){
                    c[k--] = c[c.size()-1];
                    c.pop();
                }
            cs[j++] = cs[i];
        }
    }
    cs.shrink(i - j);
}
void Solver::rebuildOrderHeap()
{
    vec<Var> vs;
    for (Var v = 0; v < nVars(); v++)
        if (decision[v] && value(v) == l_Undef)
            vs.push(v);
    order_heap.build(vs);
}
bool Solver::simplify()
{
    assert(decisionLevel() == 0);
    if (!ok || propagate() != CRef_Undef)
        return ok = false;
    if (nAssigns() == simpDB_assigns || (simpDB_props > 0))
        return true;
    removeSatisfied(learnts);
    if (remove_satisfied){
        removeSatisfied(clauses);
        for (int i = 0; i < released_vars.size(); i++){
            assert(seen[released_vars[i]] == 0);
            seen[released_vars[i]] = 1;
        }
        int i, j;
        for (i = j = 0; i < trail.size(); i++)
            if (seen[var(trail[i])] == 0)
                trail[j++] = trail[i];
        trail.shrink(i - j);
        qhead = trail.size();
        for (int i = 0; i < released_vars.size(); i++)
            seen[released_vars[i]] = 0;
        append(released_vars, free_vars);
        released_vars.clear();
    }
    checkGarbage();
    rebuildOrderHeap();
    simpDB_assigns = nAssigns();
    simpDB_props = clauses_literals + learnts_literals;
    return true;
}
lbool Solver::search(int nof_conflicts)
{
    assert(ok);
    int backtrack_level;
    int conflictC = 0;
    vec<Lit> learnt_clause;
    starts++;
    for (;;){
        CRef confl = propagate();
        if (confl != CRef_Undef){
            conflicts++; conflictC++;
            if (decisionLevel() == 0) return l_False;
            if ((bool)opt_instinct_gpu){
                submitInstinctGpuRequest(false);
                applyInstinctGpuVsidsInjection();
                applyInstinctGpuPhaseInjection();
            }
            if ((bool)opt_instinct_gpu && decisionLevel() > 0){
                const int top_level = decisionLevel();
                if (top_level >= 0 && top_level < (int)instinct_gpu_decision_records.size()){
                    InstinctGpuDecisionRecord& rec = instinct_gpu_decision_records[(size_t)top_level];
                    if (rec.valid && rec.hint_available && !rec.conflict_evaluated){
                        rec.conflict_evaluated = true;
                        const double chosen_p = rec.chosen_sign ? rec.p_conflict_false : rec.p_conflict_true;
                        const double opposite_p = rec.chosen_sign ? rec.p_conflict_true : rec.p_conflict_false;
                        const double eps = 1e-9;
                        const bool high_conf = rec.confidence + 1e-12 >= instinctGpuApplyThreshold();
                        const bool used_for_adapt = !rec.hint_applied;
                        const char* note = "tie";
                        if (used_for_adapt){
                            instinct_gpu_quality_evals++;
                            instinct_gpu_quality_sum_chosen_p += chosen_p;
                            instinct_gpu_quality_sum_opposite_p += opposite_p;
                            if (high_conf)
                                instinct_gpu_quality_high_conf_evals++;
                            if (chosen_p > opposite_p + eps){
                                instinct_gpu_quality_hits++;
                                if (high_conf)
                                    instinct_gpu_quality_high_conf_hits++;
                                note = "hit_predicted_bad";
                            }else if (chosen_p + eps < opposite_p){
                                instinct_gpu_quality_misses++;
                                if (high_conf)
                                    instinct_gpu_quality_high_conf_misses++;
                                note = "miss_predicted_good";
                            }else{
                                instinct_gpu_quality_ties++;
                                note = "tie";
                            }
                        }else if (rec.hint_applied){
                            instinct_gpu_quality_applied_evals++;
                            if (chosen_p + eps < opposite_p){
                                instinct_gpu_quality_applied_low_risk++;
                                note = "applied_low_risk";
                            }else if (chosen_p > opposite_p + eps){
                                instinct_gpu_quality_applied_high_risk++;
                                note = "applied_high_risk";
                            }else{
                                instinct_gpu_quality_applied_ties++;
                                note = "applied_tie";
                            }
                        }
                        updateInstinctGpuAdaptiveThresholdsIfNeeded();
                        traceInstinctGpuEvent("conflict_eval",
                                               decisions,
                                               conflicts,
                                               top_level,
                                               rec.var,
                                               rec.chosen_sign ? 0 : 1,
                                               rec.recommended_sign ? 0 : 1,
                                               rec.hint_available,
                                               rec.hint_applied,
                                               rec.aggressive_override,
                                               rec.confidence,
                                               rec.p_conflict_true,
                                               rec.p_conflict_false,
                                               rec.request_id,
                                               note);
                    }
                }
            }
            learnt_clause.clear();
            analyze(confl, learnt_clause, backtrack_level);
            cancelUntil(backtrack_level);
            if (learnt_clause.size() == 1){
                uncheckedEnqueue(learnt_clause[0]);
            }else{
                CRef cr = ca.alloc(learnt_clause, true);
                learnts.push(cr);
                attachClause(cr);
                claBumpActivity(ca[cr]);
                uncheckedEnqueue(learnt_clause[0], cr);
            }
            varDecayActivity();
            claDecayActivity();
            if (--learntsize_adjust_cnt == 0){
                learntsize_adjust_confl *= learntsize_adjust_inc;
                learntsize_adjust_cnt = (int)learntsize_adjust_confl;
                max_learnts *= learntsize_inc;
                if (verbosity >= 1)
                    printf("| %9d | %7d %8d %8d | %8d %8d %6.0f | %6.3f %% |\n",
                           (int)conflicts,
                           (int)dec_vars - (trail_lim.size() == 0 ? trail.size() : trail_lim[0]), nClauses(), (int)clauses_literals,
                           (int)max_learnts, nLearnts(), (double)learnts_literals/nLearnts(), progressEstimate()*100);
            }
        }else{
            if ((nof_conflicts >= 0 && conflictC >= nof_conflicts) || !withinBudget()){
                progress_estimate = progressEstimate();
                cancelUntil(0);
                return l_Undef; }
            if (decisionLevel() == 0 && !simplify())
                return l_False;
            if (learnts.size()-nAssigns() >= max_learnts)
                reduceDB();
            Lit next = lit_Undef;
            while (decisionLevel() < assumptions.size()){
                Lit p = assumptions[decisionLevel()];
                if (value(p) == l_True){
                    newDecisionLevel();
                }else if (value(p) == l_False){
                    analyzeFinal(~p, conflict);
                    return l_False;
                }else{
                    next = p;
                    break;
                }
            }
            bool picked_by_solver = false;
            if (next == lit_Undef){
                decisions++;
                next = pickBranchLit();
                picked_by_solver = true;
                if (next == lit_Undef)
                    return l_True;
            }
            newDecisionLevel();
            uncheckedEnqueue(next);
            if (picked_by_solver){
                InstinctGpuDecisionRecord rec;
                rec.valid = true;
                rec.hint_available = false;
                rec.hint_applied = false;
                rec.aggressive_override = false;
                rec.conflict_evaluated = false;
                rec.var = var(next);
                rec.chosen_sign = sign(next);
                rec.recommended_sign = sign(next);
                rec.request_id = 0;
                rec.confidence = 0.0;
                rec.p_conflict_true = 0.0;
                rec.p_conflict_false = 0.0;
                rec.decision_counter = decisions;
                if (instinct_gpu_last_pick_trace.valid &&
                    instinct_gpu_last_pick_trace.var == rec.var &&
                    instinct_gpu_last_pick_trace.chosen_sign == rec.chosen_sign){
                    rec.hint_available = instinct_gpu_last_pick_trace.hint_available;
                    rec.hint_applied = instinct_gpu_last_pick_trace.hint_applied;
                    rec.aggressive_override = instinct_gpu_last_pick_trace.aggressive_override;
                    rec.recommended_sign = instinct_gpu_last_pick_trace.recommended_sign;
                    rec.request_id = instinct_gpu_last_pick_trace.request_id;
                    rec.confidence = instinct_gpu_last_pick_trace.confidence;
                    rec.p_conflict_true = instinct_gpu_last_pick_trace.p_conflict_true;
                    rec.p_conflict_false = instinct_gpu_last_pick_trace.p_conflict_false;
                }
                const int level = decisionLevel();
                if (level >= 0){
                    if (level >= (int)instinct_gpu_decision_records.size())
                        instinct_gpu_decision_records.resize((size_t)level + 1);
                    instinct_gpu_decision_records[(size_t)level] = rec;
                }
                instinct_gpu_trace_decision_events++;
                if (rec.hint_available)
                    instinct_gpu_trace_decisions_with_hint++;
                if (rec.hint_applied)
                    instinct_gpu_trace_decisions_with_applied_hint++;
                if (rec.aggressive_override)
                    instinct_gpu_trace_decisions_with_aggressive_override++;
                const char* note = "no_hint";
                if (rec.hint_applied && rec.aggressive_override)
                    note = "instinct_applied_aggr";
                else if (rec.hint_applied)
                    note = "instinct_applied";
                else if (rec.hint_available)
                    note = "hint_available_not_applied";
                traceInstinctGpuEvent("decision",
                                       decisions,
                                       conflicts,
                                       level,
                                       rec.var,
                                       rec.chosen_sign ? 0 : 1,
                                       rec.recommended_sign ? 0 : 1,
                                       rec.hint_available,
                                       rec.hint_applied,
                                       rec.aggressive_override,
                                       rec.confidence,
                                       rec.p_conflict_true,
                                       rec.p_conflict_false,
                                       rec.request_id,
                                       note);
            }
        }
    }
}
double Solver::progressEstimate() const
{
    double progress = 0;
    double F = 1.0 / nVars();
    for (int i = 0; i <= decisionLevel(); i++){
        int beg = i == 0 ? 0 : trail_lim[i - 1];
        int end = i == decisionLevel() ? trail.size() : trail_lim[i];
        progress += pow(F, i) * (end - beg);
    }
    return progress / nVars();
}
static double luby(double y, int x){
    int size, seq;
    for (size = 1, seq = 0; size < x+1; seq++, size = 2*size+1);
    while (size-1 != x){
        size = (size-1)>>1;
        seq--;
        x = x % size;
    }
    return pow(y, seq);
}
lbool Solver::solve_()
{
    model.clear();
    conflict.clear();
    if (!ok) return l_False;
    solves++;
    instinct_gpu_decision_records.clear();
    instinct_gpu_last_pick_trace.valid = false;
    if ((bool)opt_instinct_gpu)
        ensureInstinctGpuTraceOpened(verbosity);
    max_learnts = nClauses() * learntsize_factor;
    if (max_learnts < min_learnts_lim)
        max_learnts = min_learnts_lim;
    learntsize_adjust_confl = learntsize_adjust_start_confl;
    learntsize_adjust_cnt = (int)learntsize_adjust_confl;
    lbool status = l_Undef;
    if (verbosity >= 1){
        printf("============================[ Search Statistics ]==============================\n");
        printf("| Conflicts |          ORIGINAL         |          LEARNT          | Progress |\n");
        printf("|           |    Vars  Clauses Literals |    Limit  Clauses Lit/Cl |          |\n");
        printf("===============================================================================\n");
    }
    int curr_restarts = 0;
    while (status == l_Undef){
        if ((bool)opt_instinct_gpu && (bool)opt_instinct_gpu_submit_on_restart)
            submitInstinctGpuRequest(true);
        double rest_base = luby_restart ? luby(restart_inc, curr_restarts) : pow(restart_inc, curr_restarts);
        status = search(rest_base * restart_first);
        if (!withinBudget()) break;
        curr_restarts++;
    }
    if (verbosity >= 1)
        printf("===============================================================================\n");
    if (status == l_True){
        model.growTo(nVars());
        for (int i = 0; i < nVars(); i++) model[i] = value(i);
    }else if (status == l_False && conflict.size() == 0)
        ok = false;
    cancelUntil(0);
    return status;
}
bool Solver::implies(const vec<Lit>& assumps, vec<Lit>& out)
{
    trail_lim.push(trail.size());
    for (int i = 0; i < assumps.size(); i++){
        Lit a = assumps[i];
        if (value(a) == l_False){
            cancelUntil(0);
            return false;
        }else if (value(a) == l_Undef)
            uncheckedEnqueue(a);
    }
    unsigned trail_before = trail.size();
    bool ret = true;
    if (propagate() == CRef_Undef){
        out.clear();
        for (int j = trail_before; j < trail.size(); j++)
            out.push(trail[j]);
    }else
        ret = false;
    cancelUntil(0);
    return ret;
}
static Var mapVar(Var x, vec<Var>& map, Var& max)
{
    if (map.size() <= x || map[x] == -1){
        map.growTo(x+1, -1);
        map[x] = max++;
    }
    return map[x];
}
void Solver::toDimacs(FILE* f, Clause& c, vec<Var>& map, Var& max)
{
    if (satisfied(c)) return;
    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) != l_False)
            fprintf(f, "%s%d ", sign(c[i]) ? "-" : "", mapVar(var(c[i]), map, max)+1);
    fprintf(f, "0\n");
}
void Solver::toDimacs(const char *file, const vec<Lit>& assumps)
{
    FILE* f = fopen(file, "wr");
    if (f == NULL)
        fprintf(stderr, "could not open file %s\n", file), exit(1);
    toDimacs(f, assumps);
    fclose(f);
}
void Solver::toDimacs(FILE* f, const vec<Lit>& assumps)
{
    if (!ok){
        fprintf(f, "p cnf 1 2\n1 0\n-1 0\n");
        return; }
    vec<Var> map; Var max = 0;
    int cnt = 0;
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]]))
            cnt++;
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]])){
            Clause& c = ca[clauses[i]];
            for (int j = 0; j < c.size(); j++)
                if (value(c[j]) != l_False)
                    mapVar(var(c[j]), map, max);
        }
    cnt += assumps.size();
    fprintf(f, "p cnf %d %d\n", max, cnt);
    for (int i = 0; i < assumps.size(); i++){
        assert(value(assumps[i]) != l_False);
        fprintf(f, "%s%d 0\n", sign(assumps[i]) ? "-" : "", mapVar(var(assumps[i]), map, max)+1);
    }
    for (int i = 0; i < clauses.size(); i++)
        toDimacs(f, ca[clauses[i]], map, max);
    if (verbosity > 0)
        printf("Wrote DIMACS with %d variables and %d clauses.\n", max, cnt);
}
void Solver::printStats() const
{
    double cpu_time = cpuTime();
    double mem_used = memUsedPeak();
    printf("restarts              : %"PRIu64"\n", starts);
    printf("conflicts             : %-12"PRIu64"   (%.0f /sec)\n", conflicts , conflicts /cpu_time);
    printf("decisions             : %-12"PRIu64"   (%4.2f %% random) (%.0f /sec)\n", decisions, (float)rnd_decisions*100 / (float)decisions, decisions /cpu_time);
    printf("propagations          : %-12"PRIu64"   (%.0f /sec)\n", propagations, propagations/cpu_time);
    if ((bool)opt_instinct_gpu){
        const double online_threshold = instinctGpuApplyThreshold();
        printf("instinct-gpu reqs    : %-12d   (dropped: %d)\n", instinct_gpu_requests_submitted, instinct_gpu_requests_dropped);
        printf("instinct-gpu sched   : submit-conf=%d submit-dec=%d restart=%d\n",
               (int)opt_instinct_gpu_submit_every_conflicts,
               (int)opt_instinct_gpu_request_every,
               (bool)opt_instinct_gpu_submit_on_restart ? 1 : 0);
        printf("instinct-gpu hints   : %-12d   (seen: %d, stale: %d, threshold: %.2f)\n",
               instinct_gpu_hints_applied, instinct_gpu_hints_seen, instinct_gpu_hints_stale, online_threshold);
        if (instinct_gpu_hints_stale > 0){
            printf("instinct-gpu stale   : lag=%d level=%d assign=%d top=%d\n",
                   instinct_gpu_hints_stale_lag,
                   instinct_gpu_hints_stale_level,
                   instinct_gpu_hints_stale_assign,
                   instinct_gpu_hints_stale_top);
        }
        if ((bool)opt_instinct_gpu_aggressive_pick){
            printf("instinct-gpu aggr    : %-12d   (overrides: %d, scans: %" PRIu64 ", threshold: %.2f)\n",
                   instinct_gpu_aggressive_applied,
                   instinct_gpu_aggressive_overrides,
                   instinct_gpu_aggressive_scans,
                   (double)opt_instinct_gpu_aggressive_threshold);
        }
        if ((bool)opt_instinct_gpu_preempt){
            printf("instinct-gpu preempt : %-12d   (checks: %d, conf-thr: %.2f, p-stop: %.2f, margin: %.2f, p-stop-trg: %d)\n",
                   instinct_gpu_preempt_backtracks,
                   instinct_gpu_preempt_checks,
                   (double)opt_instinct_gpu_preempt_threshold,
                   (double)opt_instinct_gpu_stop_prob_threshold,
                   (double)opt_instinct_gpu_stop_prob_margin,
                   instinct_gpu_preempt_prob_triggers);
        }
        if ((bool)opt_instinct_gpu_hard_stop_pick){
            printf("instinct-gpu stop    : %-12d   (p-stop: %.2f, margin: %.2f)\n",
                   instinct_gpu_hard_stop_flips,
                   (double)opt_instinct_gpu_stop_prob_threshold,
                   (double)opt_instinct_gpu_stop_prob_margin);
        }
        printf("instinct-gpu usage   : %-12" PRIu64 "   (hint-available: %" PRIu64 ", hint-applied: %" PRIu64 ", aggr-overrides: %" PRIu64 ")\n",
               instinct_gpu_trace_decision_events,
               instinct_gpu_trace_decisions_with_hint,
               instinct_gpu_trace_decisions_with_applied_hint,
               instinct_gpu_trace_decisions_with_aggressive_override);
        if ((bool)opt_instinct_gpu_vsids_inject){
            printf("instinct-gpu vsids   : rounds=%" PRIu64 " scanned=%" PRIu64 " bumped=%" PRIu64 " phase=%" PRIu64 " relaxed=%" PRIu64 " (every-conf=%d)\n",
                   instinct_gpu_vsids_rounds,
                   instinct_gpu_vsids_scanned,
                   instinct_gpu_vsids_bumped,
                   instinct_gpu_vsids_phase_updates,
                   instinct_gpu_vsids_relaxed_hints,
                   (int)opt_instinct_gpu_vsids_every_conflicts);
        }
        if ((bool)opt_instinct_gpu_phase_inject){
            printf("instinct-gpu phase   : rounds=%" PRIu64 " scanned=%" PRIu64 " updates=%" PRIu64 " relaxed=%" PRIu64 " (every-conf=%d, topk=%d, max-upd=%d)\n",
                   instinct_gpu_phase_rounds,
                   instinct_gpu_phase_scanned,
                   instinct_gpu_phase_updates,
                   instinct_gpu_phase_relaxed_hints,
                   (int)opt_instinct_gpu_phase_every_conflicts,
                   (int)opt_instinct_gpu_phase_topk,
                   (int)opt_instinct_gpu_phase_max_updates);
        }
        if (instinct_gpu_quality_evals > 0){
            const double hit_rate = 100.0 * (double)instinct_gpu_quality_hits / (double)instinct_gpu_quality_evals;
            const double miss_rate = 100.0 * (double)instinct_gpu_quality_misses / (double)instinct_gpu_quality_evals;
            const double tie_rate = 100.0 * (double)instinct_gpu_quality_ties / (double)instinct_gpu_quality_evals;
            const double avg_chosen = instinct_gpu_quality_sum_chosen_p / (double)instinct_gpu_quality_evals;
            const double avg_opposite = instinct_gpu_quality_sum_opposite_p / (double)instinct_gpu_quality_evals;
            printf("instinct-gpu quality : %-12" PRIu64 "   (hit: %.1f%%, miss: %.1f%%, tie: %.1f%%)\n",
                   instinct_gpu_quality_evals, hit_rate, miss_rate, tie_rate);
            printf("instinct-gpu p(conf) : chosen=%.4f opposite=%.4f\n", avg_chosen, avg_opposite);
            if (instinct_gpu_quality_high_conf_evals > 0){
                const double high_hit_rate = 100.0 * (double)instinct_gpu_quality_high_conf_hits / (double)instinct_gpu_quality_high_conf_evals;
                const double high_miss_rate = 100.0 * (double)instinct_gpu_quality_high_conf_misses / (double)instinct_gpu_quality_high_conf_evals;
                printf("instinct-gpu q-high  : %-12" PRIu64 "   (hit: %.1f%%, miss: %.1f%%, threshold: %.2f)\n",
                       instinct_gpu_quality_high_conf_evals,
                       high_hit_rate,
                       high_miss_rate,
                       online_threshold);
            }
        }
        if (instinct_gpu_quality_applied_evals > 0){
            const double low_risk_rate = 100.0 * (double)instinct_gpu_quality_applied_low_risk / (double)instinct_gpu_quality_applied_evals;
            const double high_risk_rate = 100.0 * (double)instinct_gpu_quality_applied_high_risk / (double)instinct_gpu_quality_applied_evals;
            const double tie_rate = 100.0 * (double)instinct_gpu_quality_applied_ties / (double)instinct_gpu_quality_applied_evals;
            printf("instinct-gpu q-applied: %-12" PRIu64 "   (low-risk: %.1f%%, high-risk: %.1f%%, tie: %.1f%%)\n",
                   instinct_gpu_quality_applied_evals,
                   low_risk_rate,
                   high_risk_rate,
                   tie_rate);
        }
        if ((bool)opt_instinct_gpu_adaptive_threshold){
            printf("instinct-gpu adapt   : %-12d   (thr: %.2f, aggr: %.2f, window: %d)\n",
                   instinct_gpu_adapt_updates,
                   online_threshold,
                   (double)opt_instinct_gpu_aggressive_threshold,
                   (int)opt_instinct_gpu_adapt_window);
        }
        if (opt_instinct_gpu_trace_csv != NULL && opt_instinct_gpu_trace_csv[0] != '\0'){
            printf("instinct-gpu trace   : %-12" PRIu64 "   (%s)\n",
                   instinct_gpu_trace_events,
                   (const char*)opt_instinct_gpu_trace_csv);
        }
    }
    printf("conflict literals     : %-12"PRIu64"   (%4.2f %% deleted)\n", tot_literals, (max_literals - tot_literals)*100 / (double)max_literals);
    if (mem_used != 0) printf("Memory used           : %.2f MB\n", mem_used);
    printf("CPU time              : %g s\n", cpu_time);
}
void Solver::relocAll(ClauseAllocator& to)
{
    watches.cleanAll();
    for (int v = 0; v < nVars(); v++)
        for (int s = 0; s < 2; s++){
            Lit p = mkLit(v, s);
            vec<Watcher>& ws = watches[p];
            for (int j = 0; j < ws.size(); j++)
                ca.reloc(ws[j].cref, to);
        }
    for (int i = 0; i < trail.size(); i++){
        Var v = var(trail[i]);
        if (reason(v) != CRef_Undef && (ca[reason(v)].reloced() || locked(ca[reason(v)]))){
            assert(!isRemoved(reason(v)));
            ca.reloc(vardata[v].reason, to);
        }
    }
    int i, j;
    for (i = j = 0; i < learnts.size(); i++)
        if (!isRemoved(learnts[i])){
            ca.reloc(learnts[i], to);
            learnts[j++] = learnts[i];
        }
    learnts.shrink(i - j);
    for (i = j = 0; i < clauses.size(); i++)
        if (!isRemoved(clauses[i])){
            ca.reloc(clauses[i], to);
            clauses[j++] = clauses[i];
        }
    clauses.shrink(i - j);
}
void Solver::garbageCollect()
{
    ClauseAllocator to(ca.size() - ca.wasted());
    relocAll(to);
    if (verbosity >= 2)
        printf("|  Garbage collection:   %12d bytes => %12d bytes             |\n",
               ca.size()*ClauseAllocator::Unit_Size, to.size()*ClauseAllocator::Unit_Size);
    to.moveTo(ca);
}
