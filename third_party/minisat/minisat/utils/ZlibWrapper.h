#ifndef Minisat_ZlibWrapper_h
#define Minisat_ZlibWrapper_h 
#if defined(__has_include)
#if __has_include(<zlib.h>)
#include <zlib.h>
#define MINISAT_HAS_ZLIB 1
#else
#define MINISAT_HAS_ZLIB 0
#endif
#else
#define MINISAT_HAS_ZLIB 0
#endif
#if !MINISAT_HAS_ZLIB
#include <cstdio>
typedef FILE* gzFile;
static inline gzFile gzopen(const char* path, const char* mode) {
    (void)mode;
    return fopen(path, "rb");
}
static inline gzFile gzdopen(int fd, const char* mode) {
    (void)mode;
    return fdopen(fd, "rb");
}
static inline int gzread(gzFile in, void* buf, unsigned int len) {
    return static_cast<int>(fread(buf, 1, len, in));
}
static inline int gzclose(gzFile in) {
    return fclose(in);
}
#endif
#endif
