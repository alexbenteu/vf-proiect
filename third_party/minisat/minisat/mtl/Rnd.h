#ifndef Minisat_Rnd_h
#define Minisat_Rnd_h 
#include "minisat/mtl/Vec.h"
namespace Minisat {
static inline double drand(double& seed)
{
    seed *= 1389796;
    int q = (int)(seed / 2147483647);
    seed -= (double)q * 2147483647;
    return seed / 2147483647;
}
static inline int irand(double& seed, int size) { return (int)(drand(seed) * size); }
template<class T>
static void randomShuffle(double& seed, vec<T>& xs)
{
    for (int i = 0; i < xs.size(); i++){
        int pick = i + irand(seed, xs.size() - i);
        T tmp = xs[i];
        xs[i] = xs[pick];
        xs[pick] = tmp;
    }
}
template<class T>
static void randomShuffle(double& seed, vec<vec<T> >& xs)
{
    for (int i = 0; i < xs.size(); i++){
        int pick = i + irand(seed, xs.size() - i);
        vec<T> tmp; xs[i].moveTo(tmp);
        xs[pick].moveTo(xs[i]);
        tmp.moveTo(xs[pick]);
    }
}
}
#endif
