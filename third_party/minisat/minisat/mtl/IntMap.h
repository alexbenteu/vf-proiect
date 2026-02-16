#ifndef Minisat_IntMap_h
#define Minisat_IntMap_h 
#include "minisat/mtl/Vec.h"
namespace Minisat {
    template<class T> struct MkIndexDefault {
        typename vec<T>::Size operator()(T t) const { return (typename vec<T>::Size)t; }
    };
    template<class K, class V, class MkIndex = MkIndexDefault<K> >
    class IntMap {
        vec<V> map;
        MkIndex index;
    public:
        explicit IntMap(MkIndex _index = MkIndex()) : index(_index){}
        bool has (K k) const { return index(k) < map.size(); }
        const V& operator[](K k) const { assert(has(k)); return map[index(k)]; }
        V& operator[](K k) { assert(has(k)); return map[index(k)]; }
        const V* begin () const { return &map[0]; }
        const V* end () const { return &map[map.size()]; }
        V* begin () { return &map[0]; }
        V* end () { return &map[map.size()]; }
        void reserve(K key, V pad) { map.growTo(index(key)+1, pad); }
        void reserve(K key) { map.growTo(index(key)+1); }
        void insert (K key, V val, V pad){ reserve(key, pad); operator[](key) = val; }
        void insert (K key, V val) { reserve(key); operator[](key) = val; }
        void clear (bool dispose = false) { map.clear(dispose); }
        void moveTo (IntMap& to) { map.moveTo(to.map); to.index = index; }
        void copyTo (IntMap& to) const { map.copyTo(to.map); to.index = index; }
    };
    template<class K, class MkIndex = MkIndexDefault<K> >
    class IntSet
    {
        IntMap<K, char, MkIndex> in_set;
        vec<K> xs;
    public:
        int size (void) const { return xs.size(); }
        void clear (bool free = false){
            if (free)
                in_set.clear(true);
            else
                for (int i = 0; i < xs.size(); i++)
                    in_set[xs[i]] = 0;
            xs.clear(free);
        }
        const vec<K>&
                 toVec () const { return xs; }
        K operator [] (int index) const { return xs[index]; }
        void insert (K k) { in_set.reserve(k, 0); if (!in_set[k]) { in_set[k] = 1; xs.push(k); } }
        bool has (K k) { in_set.reserve(k, 0); return in_set[k]; }
    };
    #if 0
    template<class K, class V, V nil, class MkIndex = MkIndexDefault<K> >
    class IntMapNil {
        vec<V> map;
        V nil;
    public:
        IntMap(){}
        void reserve(K);
        V& find (K);
        const V& operator[](K k) const;
    };
    #endif
}
#endif
