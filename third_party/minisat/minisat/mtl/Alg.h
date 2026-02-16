#ifndef Minisat_Alg_h
#define Minisat_Alg_h 
#include "minisat/mtl/Vec.h"
namespace Minisat {
template<class V, class T>
static inline void remove(V& ts, const T& t)
{
    int j = 0;
    for (; j < (int)ts.size() && ts[j] != t; j++);
    assert(j < (int)ts.size());
    for (; j < (int)ts.size()-1; j++) ts[j] = ts[j+1];
    ts.pop();
}
template<class V, class T>
static inline bool find(V& ts, const T& t)
{
    int j = 0;
    for (; j < (int)ts.size() && ts[j] != t; j++);
    return j < (int)ts.size();
}
template<class T>
static inline void copy(const T& from, T& to)
{
    to = from;
}
template<class T>
static inline void copy(const vec<T>& from, vec<T>& to, bool append = false)
{
    if (!append)
        to.clear();
    for (int i = 0; i < from.size(); i++){
        to.push();
        copy(from[i], to.last());
    }
}
template<class T>
static inline void append(const vec<T>& from, vec<T>& to){ copy(from, to, true); }
}
#endif
