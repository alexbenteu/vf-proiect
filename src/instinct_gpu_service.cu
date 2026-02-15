#include <cuda_runtime.h>
#include "cnf.hpp"
#include "../third_party/minisat/minisat/core/InstinctGpuShared.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
namespace {
using Minisat::InstinctGpuHint;
using Minisat::InstinctGpuRequest;
using Minisat::InstinctGpuSharedMemory;
using Minisat::INSTINCT_GPU_MAGIC;
using Minisat::INSTINCT_GPU_MAX_TARGETS;
using Minisat::INSTINCT_GPU_MAX_VARS;
using Minisat::INSTINCT_GPU_RING_CAPACITY;
using Minisat::INSTINCT_GPU_VERSION;
struct CliOptions {
  std::string cnf_path;
  std::string shm_path;
  bool quiet = false;
};
struct RunStats {
  std::uint64_t tensor_requests = 0;
};
void check_cuda(cudaError_t code, const char* stage) {
  if (code == cudaSuccess) {
    return;
  }
  throw std::runtime_error(std::string(stage) + " failed: " + cudaGetErrorString(code));
}
int pad16(int value) {
  return (value + 15) / 16 * 16;
}
int pad8(int value) {
  return (value + 7) / 8 * 8;
}
constexpr int kTensorStreamClauses = 65536;
constexpr int kTensorStreamTileClauses = 4096;
constexpr std::uint64_t kTensorMaxBytes = 1024ull * 1024ull * 1024ull;
constexpr int kPollUs = 200;
__device__ std::uint32_t hash_u32_device(std::uint64_t request_id,
                                         std::uint32_t walk,
                                         int var,
                                         int target_var) {
  std::uint64_t x = request_id;
  x ^= static_cast<std::uint64_t>(walk) * 0x9E3779B185EBCA87ull;
  x ^= static_cast<std::uint64_t>(var + 1) * 0xC2B2AE3D27D4EB4Full;
  x ^= static_cast<std::uint64_t>(target_var + 1) * 0x165667B19E3779F9ull;
  x ^= (x >> 30);
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= (x >> 27);
  x *= 0x94D049BB133111EBull;
  x ^= (x >> 31);
  return static_cast<std::uint32_t>(x & 0xffffffffu);
}
__device__ std::int8_t assignment_value_device(std::uint64_t request_id,
                                               std::uint32_t walk,
                                               int var,
                                               int forced_var,
                                               bool force_true,
                                               const std::int8_t* assignment,
                                               const std::int8_t* preferred_phase,
                                               int random_pct,
                                               int flip_pct) {
  if (var == forced_var) {
    return force_true ? std::int8_t{1} : std::int8_t{-1};
  }
  const std::uint32_t h = hash_u32_device(request_id, walk, var, forced_var);
  if (var >= 0 && var < static_cast<int>(INSTINCT_GPU_MAX_VARS)) {
    const std::int8_t value = assignment[var];
    if (value != 0) {
      return value;
    }
    if (preferred_phase != nullptr) {
      std::int8_t seeded = preferred_phase[var];
      if (seeded != 0) {
        if (flip_pct > 0 && static_cast<int>((h >> 8) % 100u) < flip_pct) {
          seeded = static_cast<std::int8_t>(-seeded);
        }
        if (random_pct <= 0 || static_cast<int>(h % 100u) >= random_pct) {
          return seeded;
        }
      }
    }
  }
  return (h & 1u) != 0u ? std::int8_t{1} : std::int8_t{-1};
}
__global__ void count_clause_stats_per_scenario_kernel(const int* C,
                                                       int scenarios,
                                                       int num_clauses,
                                                       int padded_clauses,
                                                       int* out_unsat,
                                                       int* out_unit) {
  const int idx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  if (idx >= scenarios) {
    return;
  }
  int unsat = 0;
  int unit = 0;
  const int base = idx * padded_clauses;
  for (int c = 0; c < num_clauses; ++c) {
    const int sat_count = C[base + c];
    if (sat_count <= 0) {
      ++unsat;
    } else if (sat_count == 1) {
      ++unit;
    }
  }
  out_unsat[idx] = unsat;
  out_unit[idx] = unit;
}
__global__ void generate_scenario_assignments_kernel(std::uint64_t request_id,
                                                     const std::int8_t* base_assignment,
                                                     const std::int8_t* preferred_phase,
                                                     const std::uint32_t* targets,
                                                     int num_targets,
                                                     int walks,
                                                     int num_vars,
                                                     int random_pct,
                                                     int flip_pct,
                                                     std::uint8_t* out_assignments) {
  const std::size_t linear =
      static_cast<std::size_t>(blockIdx.x) * static_cast<std::size_t>(blockDim.x) +
      static_cast<std::size_t>(threadIdx.x);
  const std::size_t total =
      static_cast<std::size_t>(num_targets) * 2ull * static_cast<std::size_t>(walks) *
      static_cast<std::size_t>(num_vars);
  if (linear >= total) {
    return;
  }
  const int scenario = static_cast<int>(linear / static_cast<std::size_t>(num_vars));
  const int var = static_cast<int>(linear % static_cast<std::size_t>(num_vars));
  const int per_target = 2 * walks;
  const int target_idx = scenario / per_target;
  const int rem = scenario % per_target;
  const bool force_true = (rem / walks) == 0;
  const std::uint32_t walk = static_cast<std::uint32_t>(rem % walks);
  const int forced_var = static_cast<int>(targets[target_idx]) - 1;
  const std::int8_t value = assignment_value_device(
      request_id,
      walk,
      var,
      forced_var,
      force_true,
      base_assignment,
      preferred_phase,
      random_pct,
      flip_pct);
  out_assignments[linear] = value > 0 ? std::uint8_t{1} : std::uint8_t{0};
}
__global__ void assignments_to_tensor_a_b1_kernel(const std::uint8_t* assignments,
                                                  int scenarios,
                                                  int num_vars,
                                                  int k_words,
                                                  unsigned* out_a) {
  const int scenario = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  if (scenario >= scenarios) {
    return;
  }
  unsigned* row = out_a + static_cast<std::size_t>(scenario) * static_cast<std::size_t>(k_words);
  const std::size_t in_base =
      static_cast<std::size_t>(scenario) * static_cast<std::size_t>(num_vars);
  for (int v = 0; v < num_vars; ++v) {
    const bool is_true = assignments[in_base + static_cast<std::size_t>(v)] != 0;
    const int bit = is_true ? (2 * v) : (2 * v + 1);
    row[bit >> 5] |= (1u << (bit & 31));
  }
}
__device__ __forceinline__ unsigned packed_bit_row_major(const unsigned* matrix,
                                                          int rows_words,
                                                          int row,
                                                          int col_bit) {
  const std::size_t word_idx = static_cast<std::size_t>(row) * static_cast<std::size_t>(rows_words) +
                               static_cast<std::size_t>(col_bit >> 5);
  const unsigned word = matrix[word_idx];
  return (word >> (col_bit & 31)) & 1u;
}
__device__ __forceinline__ unsigned packed_bit_col_major(const unsigned* matrix,
                                                          int col_words,
                                                          int row_bit,
                                                          int col) {
  const std::size_t word_idx = static_cast<std::size_t>(col) * static_cast<std::size_t>(col_words) +
                               static_cast<std::size_t>(row_bit >> 5);
  const unsigned word = matrix[word_idx];
  return (word >> (row_bit & 31)) & 1u;
}
__device__ __forceinline__ void mma_m16n8k128_b1_and_popc(unsigned a0,
                                                           unsigned a1,
                                                           unsigned b0,
                                                           int* c0,
                                                           int* c1,
                                                           int* c2,
                                                           int* c3) {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 800
  asm volatile("mma.sync.aligned.m16n8k128.row.col.s32.b1.b1.s32.and.popc "
               "{%0, %1, %2, %3}, "
               "{%4, %5}, "
               "{%6}, "
               "{%0, %1, %2, %3};\n"
               : "+r"(*c0), "+r"(*c1), "+r"(*c2), "+r"(*c3)
               : "r"(a0), "r"(a1), "r"(b0));
#else
  (void)a0;
  (void)a1;
  (void)b0;
  (void)c0;
  (void)c1;
  (void)c2;
  (void)c3;
#endif
}
__global__ void tensor_b1_gemm_inline_ptx_kernel(const unsigned* A,
                                                 const unsigned* B_colmajor,
                                                 int* C,
                                                 int M,
                                                 int N,
                                                 int K_bits,
                                                 int K_words) {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 800
  const int warp_id = static_cast<int>(threadIdx.x) / 32;
  const int lane = static_cast<int>(threadIdx.x) & 31;
  const int warps_per_block = static_cast<int>(blockDim.x) / 32;
  const int tile_m = static_cast<int>(blockIdx.y) * warps_per_block + warp_id;
  const int tile_n = static_cast<int>(blockIdx.x);
  const int row_base = tile_m * 16;
  const int col_base = tile_n * 8;
  if (row_base >= M || col_base >= N) {
    return;
  }
  const int group_id = lane >> 2;
  const int thread_id = lane & 3;
  int c0 = 0;
  int c1 = 0;
  int c2 = 0;
  int c3 = 0;
  for (int k0 = 0; k0 < K_bits; k0 += 128) {
    unsigned a0 = 0u;
    unsigned a1 = 0u;
    unsigned b0 = 0u;
#pragma unroll
    for (int i = 0; i < 32; ++i) {
      const int a_col = k0 + thread_id * 32 + i;
      const int a_row0 = row_base + group_id;
      const int a_row1 = a_row0 + 8;
      a0 |= packed_bit_row_major(A, K_words, a_row0, a_col) << i;
      a1 |= packed_bit_row_major(A, K_words, a_row1, a_col) << i;
      const int b_row = k0 + thread_id * 32 + i;
      const int b_col = col_base + group_id;
      b0 |= packed_bit_col_major(B_colmajor, K_words, b_row, b_col) << i;
    }
    mma_m16n8k128_b1_and_popc(a0, a1, b0, &c0, &c1, &c2, &c3);
  }
  const int c_row0 = row_base + group_id;
  const int c_row1 = c_row0 + 8;
  const int c_col0 = col_base + thread_id * 2;
  C[static_cast<std::size_t>(c_row0) * static_cast<std::size_t>(N) + static_cast<std::size_t>(c_col0)] = c0;
  C[static_cast<std::size_t>(c_row0) * static_cast<std::size_t>(N) +
    static_cast<std::size_t>(c_col0 + 1)] = c1;
  C[static_cast<std::size_t>(c_row1) * static_cast<std::size_t>(N) + static_cast<std::size_t>(c_col0)] = c2;
  C[static_cast<std::size_t>(c_row1) * static_cast<std::size_t>(N) +
    static_cast<std::size_t>(c_col0 + 1)] = c3;
#else
  (void)A;
  (void)B_colmajor;
  (void)C;
  (void)M;
  (void)N;
  (void)K_bits;
  (void)K_words;
#endif
}
void print_usage(const char* argv0) {
  std::cout << "Usage: " << argv0
            << " --cnf <file.cnf> --shm <path> [--walks N] [--max-batch N] [--batch-window-us N]\\n"
               "       [--engine tensor] [--tensor-require-b1]\\n"
               "       [--quiet]\\n";
}
CliOptions parse_args(int argc, char** argv) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& flag) -> const char* {
      if (i + 1 >= argc) {
        throw std::runtime_error("Missing value for " + flag);
      }
      ++i;
      return argv[i];
    };
    if (arg == "--cnf") {
      options.cnf_path = require_value(arg);
    } else if (arg == "--shm") {
      options.shm_path = require_value(arg);
    } else if (arg == "--walks") {
      (void)require_value(arg);
    } else if (arg == "--max-batch") {
      (void)require_value(arg);
    } else if (arg == "--batch-window-us") {
      (void)require_value(arg);
    } else if (arg == "--engine") {
      const std::string mode = require_value(arg);
      if (mode != "tensor" && mode != "auto") {
        throw std::runtime_error("Only --engine tensor is supported in this minimal profile");
      }
    } else if (arg == "--tensor-require-b1") {
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }
  if (options.cnf_path.empty()) {
    throw std::runtime_error("Missing required --cnf <path>");
  }
  if (options.shm_path.empty()) {
    throw std::runtime_error("Missing required --shm <path>");
  }
  return options;
}
InstinctGpuSharedMemory* map_shared_state(const std::string& path, int* fd_out) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    throw std::runtime_error("open(" + path + ") failed");
  }
  const std::size_t shm_size = sizeof(InstinctGpuSharedMemory);
  if (::ftruncate(fd, static_cast<off_t>(shm_size)) != 0) {
    ::close(fd);
    throw std::runtime_error("ftruncate(" + path + ") failed");
  }
  void* mapped = ::mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    throw std::runtime_error("mmap(" + path + ") failed");
  }
  auto* shm = reinterpret_cast<InstinctGpuSharedMemory*>(mapped);
  if (shm->magic != INSTINCT_GPU_MAGIC || shm->version != INSTINCT_GPU_VERSION) {
    std::memset(shm, 0, sizeof(*shm));
    shm->magic = INSTINCT_GPU_MAGIC;
    shm->version = INSTINCT_GPU_VERSION;
  }
  instinctGpuResetSharedState(shm);
  shm->magic = INSTINCT_GPU_MAGIC;
  shm->version = INSTINCT_GPU_VERSION;
  __sync_synchronize();
  *fd_out = fd;
  return shm;
}
void unmap_shared_state(InstinctGpuSharedMemory* shm, int fd) {
  if (shm != nullptr) {
    ::munmap(shm, sizeof(InstinctGpuSharedMemory));
  }
  if (fd >= 0) {
    ::close(fd);
  }
}
class DeviceScenarioGenerator {
 public:
  DeviceScenarioGenerator() = default;
  ~DeviceScenarioGenerator() {
    cleanup();
  }
  bool generate(const InstinctGpuRequest& req,
                int num_targets,
                int num_vars,
                int walks,
                int random_pct,
                int flip_pct,
                std::uint8_t** d_assignments,
                int* scenarios,
                std::string* error) {
    if (num_targets <= 0 || num_vars <= 0 || walks <= 0) {
      *error = "invalid scenario generation dimensions";
      return false;
    }
    const std::size_t scenario_count =
        static_cast<std::size_t>(num_targets) * 2ull * static_cast<std::size_t>(walks);
    const std::size_t total_assignments = scenario_count * static_cast<std::size_t>(num_vars);
    if (scenario_count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      *error = "scenario count overflow";
      return false;
    }
    try {
      ensure_io_buffers();
      ensure_assignment_capacity(total_assignments);
      check_cuda(cudaMemcpy(d_base_assignment_,
                            req.assignment,
                            static_cast<std::size_t>(num_vars) * sizeof(std::int8_t),
                            cudaMemcpyHostToDevice),
                 "cudaMemcpy(scenario base assignment)");
      check_cuda(cudaMemcpy(d_preferred_phase_,
                            req.preferred_phase,
                            static_cast<std::size_t>(num_vars) * sizeof(std::int8_t),
                            cudaMemcpyHostToDevice),
                 "cudaMemcpy(scenario preferred phase)");
      check_cuda(cudaMemcpy(d_targets_,
                            req.targets,
                            static_cast<std::size_t>(num_targets) * sizeof(std::uint32_t),
                            cudaMemcpyHostToDevice),
                 "cudaMemcpy(scenario targets)");
      const int block = 256;
      const int grid = static_cast<int>((total_assignments + static_cast<std::size_t>(block) - 1) /
                                        static_cast<std::size_t>(block));
      generate_scenario_assignments_kernel<<<grid, block>>>(req.request_id,
                                                            d_base_assignment_,
                                                            d_preferred_phase_,
                                                            d_targets_,
                                                            num_targets,
                                                            walks,
                                                            num_vars,
                                                            random_pct,
                                                            flip_pct,
                                                            d_assignments_);
      check_cuda(cudaGetLastError(), "generate_scenario_assignments_kernel launch");
      *d_assignments = d_assignments_;
      *scenarios = static_cast<int>(scenario_count);
    } catch (const std::exception& ex) {
      *error = ex.what();
      return false;
    }
    return true;
  }
 private:
  void ensure_io_buffers() {
    if (d_base_assignment_ == nullptr) {
      check_cuda(cudaMalloc(&d_base_assignment_, INSTINCT_GPU_MAX_VARS * sizeof(std::int8_t)),
                 "cudaMalloc(scenario base assignment)");
    }
    if (d_targets_ == nullptr) {
      check_cuda(cudaMalloc(&d_targets_, INSTINCT_GPU_MAX_TARGETS * sizeof(std::uint32_t)),
                 "cudaMalloc(scenario targets)");
    }
    if (d_preferred_phase_ == nullptr) {
      check_cuda(cudaMalloc(&d_preferred_phase_, INSTINCT_GPU_MAX_VARS * sizeof(std::int8_t)),
                 "cudaMalloc(scenario preferred phase)");
    }
  }
  void ensure_assignment_capacity(std::size_t assignment_count) {
    if (assignment_count <= assignment_capacity_) {
      return;
    }
    if (d_assignments_ != nullptr) {
      cudaFree(d_assignments_);
      d_assignments_ = nullptr;
    }
    check_cuda(cudaMalloc(&d_assignments_, assignment_count * sizeof(std::uint8_t)),
               "cudaMalloc(scenario assignments)");
    assignment_capacity_ = assignment_count;
  }
  void cleanup() {
    if (d_assignments_ != nullptr) {
      cudaFree(d_assignments_);
      d_assignments_ = nullptr;
    }
    if (d_targets_ != nullptr) {
      cudaFree(d_targets_);
      d_targets_ = nullptr;
    }
    if (d_preferred_phase_ != nullptr) {
      cudaFree(d_preferred_phase_);
      d_preferred_phase_ = nullptr;
    }
    if (d_base_assignment_ != nullptr) {
      cudaFree(d_base_assignment_);
      d_base_assignment_ = nullptr;
    }
    assignment_capacity_ = 0;
  }
 private:
  std::int8_t* d_base_assignment_ = nullptr;
  std::int8_t* d_preferred_phase_ = nullptr;
  std::uint32_t* d_targets_ = nullptr;
  std::uint8_t* d_assignments_ = nullptr;
  std::size_t assignment_capacity_ = 0;
};
int clamp_num_targets(std::uint32_t n) {
  if (n == 0) {
    return 0;
  }
  const std::uint32_t clamped = std::min(n, INSTINCT_GPU_MAX_TARGETS);
  return static_cast<int>(clamped);
}
void write_hints_from_probabilities(const InstinctGpuRequest& req,
                                    const std::vector<float>& p_true,
                                    const std::vector<float>& p_false,
                                    InstinctGpuSharedMemory* shm) {
  constexpr double kHintConfFloor = 0.001;
  constexpr double kHintMinGap = 0.003;
  const int num_targets = static_cast<int>(p_true.size());
  for (int i = 0; i < num_targets; ++i) {
    const int var = static_cast<int>(req.targets[i]);
    if (var < 1 || var > static_cast<int>(INSTINCT_GPU_MAX_VARS)) {
      continue;
    }
    const bool prefer_true = p_true[i] <= p_false[i];
    const double raw = std::fabs(static_cast<double>(p_true[i]) -
                                 static_cast<double>(p_false[i]));
    const double se_gap = kHintConfFloor;
    const double conf_denom = raw + se_gap + kHintMinGap;
    const double conf_raw = conf_denom > 0.0 ? (raw / conf_denom) : 0.0;
    const float conf = static_cast<float>(std::max(0.0, std::min(1.0, conf_raw)));
    const bool has_hint = raw > 0.0;
    InstinctGpuHint hint{};
    hint.request_id = req.request_id;
    hint.confidence = conf;
    hint.p_conflict_true = p_true[i];
    hint.p_conflict_false = p_false[i];
    hint.decision_level = req.decision_level;
    hint.assigned_count = req.assigned_count;
    hint.top_decision_var = req.top_decision_var;
    hint.top_decision_sign = req.top_decision_sign;
    hint.has_hint = has_hint ? 1 : 0;
    hint.prefer_true = prefer_true ? 1 : 0;
    shm->hints[var - 1] = hint;
  }
  __sync_synchronize();
  shm->latest_processed_request_id = req.request_id;
}
class TensorEvaluator {
 public:
  TensorEvaluator() = default;
  ~TensorEvaluator() {
    cleanup();
  }
  bool initialize(const CnfFormula& formula, std::string* reason) {
    cleanup();
    if (formula.num_variables <= 0 || formula.clauses.empty()) {
      *reason = "invalid formula";
      return false;
    }
    formula_ = &formula;
    num_vars_ = formula.num_variables;
    num_clauses_ = static_cast<int>(formula.clauses.size());
    if (num_vars_ <= 0 || num_clauses_ <= 0) {
      *reason = "invalid tensor dimensions";
      return false;
    }
    if (num_vars_ > static_cast<int>(INSTINCT_GPU_MAX_VARS)) {
      *reason = "formula vars exceed INSTINCT_GPU_MAX_VARS";
      return false;
    }
    cudaDeviceProp props{};
    try {
      check_cuda(cudaGetDeviceProperties(&props, 0), "cudaGetDeviceProperties");
    } catch (const std::exception& ex) {
      *reason = ex.what();
      return false;
    }
    const int sm = props.major * 10 + props.minor;
    if (sm < 80) {
      *reason = "SM < 80 (inline-PTX tensor b1 unavailable)";
      return false;
    }
    K_bits_ = ((2 * num_vars_ + 127) / 128) * 128;
    K_words_ = K_bits_ / 32;
    std::size_t free_bytes = 0;
    std::size_t total_bytes = 0;
    try {
      check_cuda(cudaMemGetInfo(&free_bytes, &total_bytes), "cudaMemGetInfo");
    } catch (const std::exception& ex) {
      *reason = ex.what();
      return false;
    }
    const std::uint64_t safe_budget = static_cast<std::uint64_t>(free_bytes * 7 / 10);
    int stream_budget = std::min(num_clauses_, std::max(1, kTensorStreamClauses));
    int stream_tile = std::min(stream_budget, std::max(1, kTensorStreamTileClauses));
    auto stream_b1_bytes = [&](int tile_clauses) -> std::uint64_t {
      const int padded = pad8(tile_clauses);
      return static_cast<std::uint64_t>(K_words_) * static_cast<std::uint64_t>(padded) *
             sizeof(unsigned);
    };
    while (stream_tile > 1 &&
           (stream_b1_bytes(stream_tile) > kTensorMaxBytes ||
            stream_b1_bytes(stream_tile) > safe_budget)) {
      stream_tile /= 2;
    }
    if (stream_tile <= 0 ||
        stream_b1_bytes(stream_tile) > kTensorMaxBytes ||
        stream_b1_bytes(stream_tile) > safe_budget) {
      *reason = "insufficient free VRAM for tensor b1 streaming tile";
      cleanup();
      return false;
    }
    stream_clause_budget_ = stream_budget;
    stream_tile_clauses_ = std::max(1, stream_tile);
    stream_tile_cols_padded_ = pad8(stream_tile_clauses_);
    try {
      check_cuda(
          cudaMalloc(&d_B_b1_colmajor_,
                     static_cast<std::size_t>(K_words_) *
                         static_cast<std::size_t>(stream_tile_cols_padded_) * sizeof(unsigned)),
          "cudaMalloc(tensor B b1 stream)");
      build_stream_var_clause_index();
    } catch (const std::exception& ex) {
      *reason = ex.what();
      cleanup();
      return false;
    }
    initialized_ = true;
    last_effective_clause_count_ = 0;
    *reason = "ok (b1 streaming frontier, clauses=" +
              std::to_string(stream_clause_budget_) +
              ", tile=" + std::to_string(stream_tile_clauses_) + ")";
    return true;
  }
  bool available() const {
    return initialized_;
  }
  int num_vars() const {
    return num_vars_;
  }
  int last_effective_clause_count() const {
    return last_effective_clause_count_;
  }
  bool evaluate_device_assignments(const InstinctGpuRequest& req,
                                   const std::uint8_t* d_assignments,
                                   int scenarios,
                                   std::vector<int>* unsat_counts,
                                   std::vector<int>* unit_counts,
                                   std::string* error) {
    if (!initialized_) {
      *error = "Tensor evaluator not initialized";
      return false;
    }
    if (d_assignments == nullptr) {
      *error = "device assignments pointer is null";
      return false;
    }
    if (scenarios <= 0) {
      *error = "scenarios must be > 0";
      return false;
    }
    last_effective_clause_count_ = num_clauses_;
    std::string stream_error;
    if (!evaluate_device_assignments_b1_streaming(
            req, d_assignments, scenarios, unsat_counts, unit_counts, &stream_error)) {
      *error = stream_error.empty() ? "tensor b1 streaming evaluation failed" : stream_error;
      return false;
    }
    return true;
  }
 private:
  void build_stream_var_clause_index() {
    var_to_clauses_.clear();
    var_to_clauses_.resize(static_cast<std::size_t>(num_vars_));
    for (int c = 0; c < num_clauses_; ++c) {
      for (int literal : formula_->clauses[static_cast<std::size_t>(c)].literals) {
        const int v = std::abs(literal) - 1;
        if (v < 0 || v >= num_vars_) {
          continue;
        }
        var_to_clauses_[static_cast<std::size_t>(v)].push_back(c);
      }
    }
    clause_marks_.assign(static_cast<std::size_t>(num_clauses_), 0u);
    clause_mark_epoch_ = 1u;
    clause_score_marks_.assign(static_cast<std::size_t>(num_clauses_), 0u);
    clause_score_cache_.assign(static_cast<std::size_t>(num_clauses_), 0);
    clause_score_epoch_ = 1u;
  }
  void begin_clause_score_request() {
    if (clause_score_epoch_ == std::numeric_limits<std::uint32_t>::max()) {
      std::fill(clause_score_marks_.begin(), clause_score_marks_.end(), 0u);
      clause_score_epoch_ = 1u;
    } else {
      clause_score_epoch_++;
    }
  }
  int clause_frontier_score(const InstinctGpuRequest& req, int clause_idx) {
    if (clause_idx < 0 || clause_idx >= num_clauses_) {
      return 0;
    }
    const std::uint32_t epoch = clause_score_epoch_;
    if (clause_score_marks_[static_cast<std::size_t>(clause_idx)] == epoch) {
      return static_cast<int>(clause_score_cache_[static_cast<std::size_t>(clause_idx)]);
    }
    const Clause& clause = formula_->clauses[static_cast<std::size_t>(clause_idx)];
    const int eval_vars = std::min(num_vars_, static_cast<int>(req.num_vars));
    bool satisfied = false;
    int unassigned = 0;
    int false_lits = 0;
    for (int literal : clause.literals) {
      const int var = std::abs(literal) - 1;
      if (var < 0 || var >= num_vars_) {
        continue;
      }
      std::int8_t value = 0;
      if (var < eval_vars) {
        value = req.assignment[var];
      }
      if (value == 0) {
        ++unassigned;
        continue;
      }
      const bool lit_true = literal > 0 ? (value > 0) : (value < 0);
      if (lit_true) {
        satisfied = true;
        break;
      }
      ++false_lits;
    }
    int score = 0;
    if (!satisfied) {
      if (unassigned == 0) {
        score = 8;
      } else if (unassigned == 1) {
        score = 7;
      } else if (unassigned == 2) {
        score = 6;
      } else if (unassigned <= 4) {
        score = 4;
      } else if (unassigned <= 8) {
        score = 3;
      } else {
        score = 2;
      }
      if (!clause.literals.empty() &&
          false_lits + 1 >= static_cast<int>(clause.literals.size())) {
        score += 1;
      }
    }
    clause_score_marks_[static_cast<std::size_t>(clause_idx)] = epoch;
    clause_score_cache_[static_cast<std::size_t>(clause_idx)] =
        static_cast<std::int8_t>(std::max(0, std::min(127, score)));
    return score;
  }
  void select_stream_clauses(const InstinctGpuRequest& req, std::vector<int>* out) {
    out->clear();
    const int budget = std::min(num_clauses_, std::max(1, stream_clause_budget_));
    if (budget <= 0) {
      return;
    }
    out->reserve(static_cast<std::size_t>(budget));
    if (budget >= num_clauses_) {
      for (int c = 0; c < num_clauses_; ++c) {
        out->push_back(c);
      }
      return;
    }
    if (clause_mark_epoch_ == std::numeric_limits<std::uint32_t>::max()) {
      std::fill(clause_marks_.begin(), clause_marks_.end(), 0u);
      clause_mark_epoch_ = 1u;
    } else {
      clause_mark_epoch_++;
    }
    const std::uint32_t epoch = clause_mark_epoch_;
    begin_clause_score_request();
    const int num_targets =
        std::min(static_cast<int>(req.num_targets), static_cast<int>(INSTINCT_GPU_MAX_TARGETS));
    std::vector<std::uint8_t> seed_mask(static_cast<std::size_t>(num_vars_), 0u);
    std::vector<int> seed_vars;
    seed_vars.reserve(static_cast<std::size_t>(num_targets + 1));
    auto add_seed_var = [&](int v) {
      if (v < 0 || v >= num_vars_) {
        return;
      }
      std::uint8_t& mark = seed_mask[static_cast<std::size_t>(v)];
      if (mark != 0) {
        return;
      }
      mark = 1u;
      seed_vars.push_back(v);
    };
    for (int i = 0; i < num_targets; ++i) {
      add_seed_var(static_cast<int>(req.targets[i]) - 1);
    }
    if (req.top_decision_var > 0) {
      add_seed_var(static_cast<int>(req.top_decision_var) - 1);
    }
    const int max_candidates = std::min(num_clauses_, std::max(budget + 1024, budget * 8));
    std::vector<int> candidates;
    candidates.reserve(static_cast<std::size_t>(max_candidates));
    if (!seed_vars.empty()) {
      std::vector<int> cursors(seed_vars.size(), 0);
      bool progress = true;
      while (progress && static_cast<int>(candidates.size()) < max_candidates) {
        progress = false;
        for (std::size_t s = 0; s < seed_vars.size() &&
                                static_cast<int>(candidates.size()) < max_candidates;
             ++s) {
          const int v = seed_vars[s];
          const std::vector<int>& clauses = var_to_clauses_[static_cast<std::size_t>(v)];
          int& cursor = cursors[s];
          int scanned = 0;
          while (cursor < static_cast<int>(clauses.size()) && scanned < 64) {
            const int clause_idx = clauses[static_cast<std::size_t>(cursor++)];
            ++scanned;
            if (clause_idx < 0 || clause_idx >= num_clauses_) {
              continue;
            }
            std::uint32_t& mark = clause_marks_[static_cast<std::size_t>(clause_idx)];
            if (mark == epoch) {
              continue;
            }
            mark = epoch;
            candidates.push_back(clause_idx);
            progress = true;
            break;
          }
        }
      }
    }
    if (!candidates.empty()) {
      std::vector<int> order(candidates.size(), 0);
      std::iota(order.begin(), order.end(), 0);
      const int want = std::min(budget, static_cast<int>(order.size()));
      std::partial_sort(order.begin(),
                        order.begin() + want,
                        order.end(),
                        [&](int a_idx, int b_idx) {
                          const int a_clause = candidates[static_cast<std::size_t>(a_idx)];
                          const int b_clause = candidates[static_cast<std::size_t>(b_idx)];
                          const int sa = clause_frontier_score(req, a_clause);
                          const int sb = clause_frontier_score(req, b_clause);
                          if (sa != sb) {
                            return sa > sb;
                          }
                          const int la = static_cast<int>(
                              formula_->clauses[static_cast<std::size_t>(a_clause)].literals.size());
                          const int lb = static_cast<int>(
                              formula_->clauses[static_cast<std::size_t>(b_clause)].literals.size());
                          if (la != lb) {
                            return la < lb;
                          }
                          return a_clause < b_clause;
                        });
      for (int i = 0; i < want; ++i) {
        const int clause_idx =
            candidates[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])];
        if (clause_frontier_score(req, clause_idx) <= 0) {
          continue;
        }
        out->push_back(clause_idx);
      }
    }
    if (static_cast<int>(out->size()) < budget) {
      const int stride = 7919;
      const int start = num_clauses_ > 0
                            ? static_cast<int>(req.request_id %
                                               static_cast<std::uint64_t>(num_clauses_))
                            : 0;
      const int min_scores[3] = {4, 2, 1};
      for (int pass = 0; pass < 3 && static_cast<int>(out->size()) < budget; ++pass) {
        const int min_score = min_scores[pass];
        for (int i = 0; i < num_clauses_ && static_cast<int>(out->size()) < budget; ++i) {
          const int clause_idx =
              static_cast<int>((static_cast<std::uint64_t>(start) +
                                static_cast<std::uint64_t>(i) *
                                    static_cast<std::uint64_t>(stride)) %
                               static_cast<std::uint64_t>(num_clauses_));
          std::uint32_t& mark = clause_marks_[static_cast<std::size_t>(clause_idx)];
          if (mark == epoch) {
            continue;
          }
          if (clause_frontier_score(req, clause_idx) < min_score) {
            continue;
          }
          mark = epoch;
          out->push_back(clause_idx);
        }
      }
      for (int i = 0; i < num_clauses_ && static_cast<int>(out->size()) < budget; ++i) {
        const int clause_idx =
            static_cast<int>((static_cast<std::uint64_t>(start) +
                              static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(stride)) %
                             static_cast<std::uint64_t>(num_clauses_));
        std::uint32_t& mark = clause_marks_[static_cast<std::size_t>(clause_idx)];
        if (mark == epoch) {
          continue;
        }
        mark = epoch;
        out->push_back(clause_idx);
      }
    }
  }
  bool evaluate_device_assignments_b1_streaming(const InstinctGpuRequest& req,
                                                const std::uint8_t* d_assignments,
                                                int scenarios,
                                                std::vector<int>* unsat_counts,
                                                std::vector<int>* unit_counts,
                                                std::string* error) {
    if (formula_ == nullptr) {
      *error = "tensor streaming formula unavailable";
      return false;
    }
    const int M = pad16(scenarios);
    const std::size_t a_bytes =
        static_cast<std::size_t>(M) * static_cast<std::size_t>(K_words_) * sizeof(unsigned);
    try {
      ensure_capacity_b1(M, stream_tile_cols_padded_);
      check_cuda(cudaMemset(d_A_b1_, 0, a_bytes), "cudaMemset(tensor A b1 stream)");
      const int threads_build = 128;
      const int blocks_build = (scenarios + threads_build - 1) / threads_build;
      assignments_to_tensor_a_b1_kernel<<<blocks_build, threads_build>>>(
          d_assignments, scenarios, num_vars_, K_words_, d_A_b1_);
      check_cuda(cudaGetLastError(), "assignments_to_tensor_a_b1_kernel stream launch");
      select_stream_clauses(req, &stream_selected_clauses_);
      if (stream_selected_clauses_.empty()) {
        *error = "streaming clause frontier is empty";
        return false;
      }
      last_effective_clause_count_ = static_cast<int>(stream_selected_clauses_.size());
      unsat_counts->assign(static_cast<std::size_t>(scenarios), 0);
      unit_counts->assign(static_cast<std::size_t>(scenarios), 0);
      std::vector<int> tile_unsat(static_cast<std::size_t>(scenarios), 0);
      std::vector<int> tile_unit(static_cast<std::size_t>(scenarios), 0);
      std::vector<unsigned> h_b_tile(
          static_cast<std::size_t>(K_words_) * static_cast<std::size_t>(stream_tile_cols_padded_), 0u);
      int offset = 0;
      while (offset < last_effective_clause_count_) {
        const int tile_clauses = std::min(stream_tile_clauses_, last_effective_clause_count_ - offset);
        const int tile_cols_padded = pad8(tile_clauses);
        const std::size_t tile_words =
            static_cast<std::size_t>(K_words_) * static_cast<std::size_t>(tile_cols_padded);
        ensure_capacity_b1(M, tile_cols_padded);
        std::fill(h_b_tile.begin(), h_b_tile.begin() + static_cast<std::ptrdiff_t>(tile_words), 0u);
        for (int local_clause = 0; local_clause < tile_clauses; ++local_clause) {
          const int clause_idx = stream_selected_clauses_[static_cast<std::size_t>(offset + local_clause)];
          const Clause& clause = formula_->clauses[static_cast<std::size_t>(clause_idx)];
          for (int literal : clause.literals) {
            const int v = std::abs(literal) - 1;
            if (v < 0 || v >= num_vars_) {
              continue;
            }
            const int bit = literal > 0 ? (2 * v) : (2 * v + 1);
            const std::size_t idx = static_cast<std::size_t>(local_clause) * static_cast<std::size_t>(K_words_) +
                                    static_cast<std::size_t>(bit >> 5);
            h_b_tile[idx] |= (1u << (bit & 31));
          }
        }
        check_cuda(cudaMemcpy(d_B_b1_colmajor_,
                              h_b_tile.data(),
                              tile_words * sizeof(unsigned),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy(tensor B b1 stream)");
        const dim3 block(128, 1, 1);
        const int warps_per_block = static_cast<int>(block.x) / 32;
        const dim3 grid((tile_cols_padded + 7) / 8,
                        (M + 16 * warps_per_block - 1) / (16 * warps_per_block),
                        1);
        tensor_b1_gemm_inline_ptx_kernel<<<grid, block>>>(
            d_A_b1_, d_B_b1_colmajor_, d_C_b1_, M, tile_cols_padded, K_bits_, K_words_);
        check_cuda(cudaGetLastError(), "tensor_b1_gemm_inline_ptx_kernel stream launch");
        const int threads = 256;
        const int blocks = (scenarios + threads - 1) / threads;
        count_clause_stats_per_scenario_kernel<<<blocks, threads>>>(
            d_C_b1_, scenarios, tile_clauses, tile_cols_padded, d_unsat_b1_, d_unit_b1_);
        check_cuda(cudaGetLastError(), "count_clause_stats_per_scenario_kernel stream launch");
        check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize(tensor b1 stream)");
        check_cuda(cudaMemcpy(tile_unsat.data(),
                              d_unsat_b1_,
                              static_cast<std::size_t>(scenarios) * sizeof(int),
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpy(tensor unsat b1 stream)");
        check_cuda(cudaMemcpy(tile_unit.data(),
                              d_unit_b1_,
                              static_cast<std::size_t>(scenarios) * sizeof(int),
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpy(tensor unit b1 stream)");
        for (int s = 0; s < scenarios; ++s) {
          (*unsat_counts)[static_cast<std::size_t>(s)] += tile_unsat[static_cast<std::size_t>(s)];
          (*unit_counts)[static_cast<std::size_t>(s)] += tile_unit[static_cast<std::size_t>(s)];
        }
        offset += tile_clauses;
      }
    } catch (const std::exception& ex) {
      *error = ex.what();
      return false;
    }
    return true;
  }
  void ensure_capacity_b1(int M, int N_cols) {
    if (M <= padded_m8_capacity_ && N_cols <= padded_n8_capacity_) {
      return;
    }
    const std::size_t a_bytes =
        static_cast<std::size_t>(M) * static_cast<std::size_t>(K_words_) * sizeof(unsigned);
    const std::size_t c_bytes =
        static_cast<std::size_t>(M) * static_cast<std::size_t>(N_cols) * sizeof(int);
    const std::size_t unsat_bytes = static_cast<std::size_t>(M) * sizeof(int);
    const std::size_t unit_bytes = static_cast<std::size_t>(M) * sizeof(int);
    if (d_A_b1_ != nullptr) {
      cudaFree(d_A_b1_);
      d_A_b1_ = nullptr;
    }
    if (d_C_b1_ != nullptr) {
      cudaFree(d_C_b1_);
      d_C_b1_ = nullptr;
    }
    if (d_unsat_b1_ != nullptr) {
      cudaFree(d_unsat_b1_);
      d_unsat_b1_ = nullptr;
    }
    if (d_unit_b1_ != nullptr) {
      cudaFree(d_unit_b1_);
      d_unit_b1_ = nullptr;
    }
    check_cuda(cudaMalloc(&d_A_b1_, a_bytes), "cudaMalloc(tensor A b1 dynamic)");
    check_cuda(cudaMalloc(&d_C_b1_, c_bytes), "cudaMalloc(tensor C b1 dynamic)");
    check_cuda(cudaMalloc(&d_unsat_b1_, unsat_bytes), "cudaMalloc(tensor unsat b1 dynamic)");
    check_cuda(cudaMalloc(&d_unit_b1_, unit_bytes), "cudaMalloc(tensor unit b1 dynamic)");
    padded_m8_capacity_ = M;
    padded_n8_capacity_ = N_cols;
  }
  void cleanup() {
    if (d_unit_b1_ != nullptr) {
      cudaFree(d_unit_b1_);
      d_unit_b1_ = nullptr;
    }
    if (d_unsat_b1_ != nullptr) {
      cudaFree(d_unsat_b1_);
      d_unsat_b1_ = nullptr;
    }
    if (d_C_b1_ != nullptr) {
      cudaFree(d_C_b1_);
      d_C_b1_ = nullptr;
    }
    if (d_A_b1_ != nullptr) {
      cudaFree(d_A_b1_);
      d_A_b1_ = nullptr;
    }
    if (d_B_b1_colmajor_ != nullptr) {
      cudaFree(d_B_b1_colmajor_);
      d_B_b1_colmajor_ = nullptr;
    }
    initialized_ = false;
    formula_ = nullptr;
    padded_m8_capacity_ = 0;
    padded_n8_capacity_ = 0;
    num_vars_ = 0;
    num_clauses_ = 0;
    K_bits_ = 0;
    K_words_ = 0;
    stream_clause_budget_ = 0;
    stream_tile_clauses_ = 0;
    stream_tile_cols_padded_ = 0;
    last_effective_clause_count_ = 0;
    stream_selected_clauses_.clear();
    var_to_clauses_.clear();
    clause_marks_.clear();
    clause_mark_epoch_ = 1u;
    clause_score_marks_.clear();
    clause_score_cache_.clear();
    clause_score_epoch_ = 1u;
  }
 private:
  const CnfFormula* formula_ = nullptr;
  bool initialized_ = false;
  int num_vars_ = 0;
  int num_clauses_ = 0;
  int K_bits_ = 0;
  int K_words_ = 0;
  int padded_m8_capacity_ = 0;
  int padded_n8_capacity_ = 0;
  int stream_clause_budget_ = 0;
  int stream_tile_clauses_ = 0;
  int stream_tile_cols_padded_ = 0;
  int last_effective_clause_count_ = 0;
  std::vector<int> stream_selected_clauses_;
  std::vector<std::vector<int>> var_to_clauses_;
  std::vector<std::uint32_t> clause_marks_;
  std::uint32_t clause_mark_epoch_ = 1u;
  std::vector<std::uint32_t> clause_score_marks_;
  std::vector<std::int8_t> clause_score_cache_;
  std::uint32_t clause_score_epoch_ = 1u;
  unsigned* d_B_b1_colmajor_ = nullptr;
  unsigned* d_A_b1_ = nullptr;
  int* d_C_b1_ = nullptr;
  int* d_unsat_b1_ = nullptr;
  int* d_unit_b1_ = nullptr;
};
void probabilities_from_clause_stats(const std::vector<int>& unsat_counts,
                                     const std::vector<int>& unit_counts,
                                     int num_targets,
                                     int walks,
                                     int num_clauses,
                                     double unit_weight,
                                     std::vector<float>* p_true,
                                     std::vector<float>* p_false) {
  p_true->assign(static_cast<std::size_t>(num_targets), 1.0f);
  p_false->assign(static_cast<std::size_t>(num_targets), 1.0f);
  if (num_clauses <= 0 || walks <= 0) {
    return;
  }
  const double inv_clause = 1.0 / static_cast<double>(num_clauses);
  const double inv_walks = 1.0 / static_cast<double>(walks);
  for (int t = 0; t < num_targets; ++t) {
    double sum_true = 0.0;
    double sum_false = 0.0;
    for (int w = 0; w < walks; ++w) {
      const int idx_true = (t * 2 + 0) * walks + w;
      const int idx_false = (t * 2 + 1) * walks + w;
      const int unsat_true = unsat_counts[static_cast<std::size_t>(idx_true)];
      const int unsat_false = unsat_counts[static_cast<std::size_t>(idx_false)];
      const int unit_true = unit_counts.empty() ? 0 : unit_counts[static_cast<std::size_t>(idx_true)];
      const int unit_false = unit_counts.empty() ? 0 : unit_counts[static_cast<std::size_t>(idx_false)];
      double risk_true =
          (static_cast<double>(unsat_true) + unit_weight * static_cast<double>(unit_true)) *
          inv_clause;
      double risk_false =
          (static_cast<double>(unsat_false) + unit_weight * static_cast<double>(unit_false)) *
          inv_clause;
      risk_true = std::max(0.0, std::min(1.0, risk_true));
      risk_false = std::max(0.0, std::min(1.0, risk_false));
      sum_true += risk_true;
      sum_false += risk_false;
    }
    const double mean_true = sum_true * inv_walks;
    const double mean_false = sum_false * inv_walks;
    (*p_true)[static_cast<std::size_t>(t)] = static_cast<float>(mean_true);
    (*p_false)[static_cast<std::size_t>(t)] = static_cast<float>(mean_false);
  }
}
bool evaluate_request_tensor_only(const InstinctGpuRequest& req,
                                  DeviceScenarioGenerator* scenario_generator,
                                  TensorEvaluator* tensor,
                                  std::vector<float>* p_true,
                                  std::vector<float>* p_false,
                                  std::string* error) {
  const int num_targets = clamp_num_targets(req.num_targets);
  if (num_targets <= 0) {
    *error = "request has no valid targets";
    return false;
  }
  if (tensor == nullptr || !tensor->available()) {
    *error = "Tensor engine unavailable";
    return false;
  }
  const int walks = req.walks > 0 ? static_cast<int>(req.walks) : 6;
  constexpr int seed_random_pct = 15;
  constexpr int seed_flip_pct = 3;
  const int eval_vars = std::min(static_cast<int>(req.num_vars), tensor->num_vars());
  if (eval_vars <= 0) {
    *error = "invalid eval vars";
    return false;
  }
  std::uint8_t* d_assignments = nullptr;
  int scenarios = 0;
  std::string generation_error;
  if (scenario_generator == nullptr ||
      !scenario_generator->generate(req,
                                    num_targets,
                                    eval_vars,
                                    walks,
                                    seed_random_pct,
                                    seed_flip_pct,
                                    &d_assignments,
                                    &scenarios,
                                    &generation_error)) {
    *error = generation_error.empty() ? "scenario generation failed" : generation_error;
    return false;
  }
  std::vector<int> unsat_counts;
  std::vector<int> unit_counts;
  std::string eval_error;
  if (!tensor->evaluate_device_assignments(
          req, d_assignments, scenarios, &unsat_counts, &unit_counts, &eval_error)) {
    *error = eval_error.empty() ? "tensor evaluation failed" : eval_error;
    return false;
  }
  probabilities_from_clause_stats(unsat_counts,
                                  unit_counts,
                                  num_targets,
                                  walks,
                                  std::max(1, tensor->last_effective_clause_count()),
                                  0.25,
                                  p_true,
                                  p_false);
  return true;
}
bool process_request(const InstinctGpuRequest& req,
                     DeviceScenarioGenerator* scenario_generator,
                     TensorEvaluator* tensor,
                     RunStats* stats,
                     InstinctGpuSharedMemory* shm,
                     std::string* error) {
  std::vector<float> p_true;
  std::vector<float> p_false;
  if (!evaluate_request_tensor_only(req,
                                    scenario_generator,
                                    tensor,
                                    &p_true,
                                    &p_false,
                                    error)) {
    return false;
  }
  stats->tensor_requests++;
  write_hints_from_probabilities(req, p_true, p_false, shm);
  return true;
}
}
int main(int argc, char** argv) {
  InstinctGpuSharedMemory* shm = nullptr;
  int shm_fd = -1;
  try {
    const CliOptions options = parse_args(argc, argv);
    const CnfFormula formula = parse_dimacs_cnf(options.cnf_path);
    if (formula.num_variables <= 0) {
      throw std::runtime_error("Formula has no variables");
    }
    shm = map_shared_state(options.shm_path, &shm_fd);
    if (!options.quiet) {
      std::cout << "CP3 instinct gpu service starting\n";
      std::cout << "CNF: " << options.cnf_path << " (vars=" << formula.num_variables
                << ", clauses=" << formula.clauses.size() << ")\n";
      std::cout << "Initializing engines...\n";
      std::cout.flush();
    }
    DeviceScenarioGenerator scenario_generator;
    TensorEvaluator tensor;
    std::string tensor_reason;
    tensor.initialize(formula, &tensor_reason);
    if (!tensor.available()) {
      throw std::runtime_error("Tensor engine unavailable: " + tensor_reason);
    }
    RunStats stats{};
    std::uint64_t processed = 0;
    std::uint64_t errors = 0;
    if (!options.quiet) {
      std::cout << "CP3 instinct gpu service started\n";
      std::cout << "Tensor status: " << (tensor.available() ? "enabled" : "disabled")
                << " (" << tensor_reason << ")\n";
      std::cout.flush();
    }
    const auto started = std::chrono::steady_clock::now();
    while (shm->stop_flag == 0) {
      const std::uint32_t read_seq = shm->read_seq;
      const std::uint32_t write_seq = shm->write_seq;
      if (read_seq >= write_seq) {
        std::this_thread::sleep_for(std::chrono::microseconds(kPollUs));
        continue;
      }
      const std::uint32_t slot = read_seq % INSTINCT_GPU_RING_CAPACITY;
      const InstinctGpuRequest& req = shm->requests[slot];
        std::string error;
        const bool ok = process_request(req,
                                        &scenario_generator,
                                        &tensor,
                                        &stats,
                                      shm,
                                      &error);
      if (!ok) {
        ++errors;
        if (!options.quiet && errors <= 5) {
          std::cerr << "Warning: request " << req.request_id << " failed: " << error << "\n";
        }
      }
      __sync_synchronize();
      shm->read_seq = read_seq + 1;
      processed++;
    }
    const auto ended = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                          ended - started)
                          .count();
    if (!options.quiet) {
      std::cout << "Processed requests: " << processed << ", elapsed_ms=" << ms
                << ", errors=" << errors << "\n";
      std::cout << "Engine usage: tensor=" << stats.tensor_requests << "\n";
      std::cout << "CP3 instinct gpu service done\n";
      std::cout.flush();
    }
    unmap_shared_state(shm, shm_fd);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    unmap_shared_state(shm, shm_fd);
    return 1;
  }
}
