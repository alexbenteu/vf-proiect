#ifndef Minisat_ParseUtils_h
#define Minisat_ParseUtils_h 
#include <stdlib.h>
#include <stdio.h>
#include "minisat/utils/ZlibWrapper.h"
#include "minisat/mtl/XAlloc.h"
namespace Minisat {
class StreamBuffer {
    gzFile in;
    unsigned char* buf;
    int pos;
    int size;
    enum { buffer_size = 64*1024 };
    void assureLookahead() {
        if (pos >= size) {
            pos = 0;
            size = gzread(in, buf, buffer_size); } }
public:
    explicit StreamBuffer(gzFile i) : in(i), pos(0), size(0){
        buf = (unsigned char*)xrealloc(NULL, buffer_size);
        assureLookahead();
    }
    ~StreamBuffer() { free(buf); }
    int operator * () const { return (pos >= size) ? EOF : buf[pos]; }
    void operator ++ () { pos++; assureLookahead(); }
    int position () const { return pos; }
};
static inline bool isEof(StreamBuffer& in) { return *in == EOF; }
static inline bool isEof(const char* in) { return *in == '\0'; }
template<class B>
static void skipWhitespace(B& in) {
    while ((*in >= 9 && *in <= 13) || *in == 32)
        ++in; }
template<class B>
static void skipLine(B& in) {
    for (;;){
        if (isEof(in)) return;
        if (*in == '\n') { ++in; return; }
        ++in; } }
template<class B>
static int parseInt(B& in) {
    int val = 0;
    bool neg = false;
    skipWhitespace(in);
    if (*in == '-') neg = true, ++in;
    else if (*in == '+') ++in;
    if (*in < '0' || *in > '9') fprintf(stderr, "PARSE ERROR! Unexpected char: %c\n", *in), exit(3);
    while (*in >= '0' && *in <= '9')
        val = val*10 + (*in - '0'),
        ++in;
    return neg ? -val : val; }
template<class B>
static bool match(B& in, const char* str) {
    int i;
    for (i = 0; str[i] != '\0'; i++)
        if (in[i] != str[i])
            return false;
    in += i;
    return true;
}
template<class B>
static bool eagerMatch(B& in, const char* str) {
    for (; *str != '\0'; ++str, ++in)
        if (*str != *in)
            return false;
    return true; }
}
#endif
