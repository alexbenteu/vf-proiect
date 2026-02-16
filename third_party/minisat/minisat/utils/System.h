#ifndef Minisat_System_h
#define Minisat_System_h 
#if defined(__linux__)
#include <fpu_control.h>
#endif
#include "minisat/mtl/IntTypes.h"
namespace Minisat {
static inline double cpuTime(void);
extern double memUsed();
extern double memUsedPeak(bool strictlyPeak = false);
extern void setX86FPUPrecision();
extern void limitMemory(uint64_t max_mem_mb);
extern void limitTime(uint32_t max_cpu_time);
extern void sigTerm(void handler(int));
}
#if defined(_MSC_VER) || defined(__MINGW32__)
#include <time.h>
static inline double Minisat::cpuTime(void) { return (double)clock() / CLOCKS_PER_SEC; }
#else
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
static inline double Minisat::cpuTime(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1000000; }
#endif
#endif
