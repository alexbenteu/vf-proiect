#ifndef Minisat_SimpSolver_h
#define Minisat_SimpSolver_h 
#include "minisat/mtl/Queue.h"
#include "minisat/core/Solver.h"
namespace Minisat {
class SimpSolver : public Solver {
 public:
    SimpSolver();
    ~SimpSolver();
    Var newVar (lbool upol = l_Undef, bool dvar = true);
    void releaseVar(Lit l);
    bool addClause (const vec<Lit>& ps);
    bool addEmptyClause();
    bool addClause (Lit p);
    bool addClause (Lit p, Lit q);
    bool addClause (Lit p, Lit q, Lit r);
    bool addClause (Lit p, Lit q, Lit r, Lit s);
    bool addClause_( vec<Lit>& ps);
    bool substitute(Var v, Lit x);
    void setFrozen (Var v, bool b);
    bool isEliminated(Var v) const;
    void freezeVar (Var v);
    void thaw ();
    bool solve (const vec<Lit>& assumps, bool do_simp = true, bool turn_off_simp = false);
    lbool solveLimited(const vec<Lit>& assumps, bool do_simp = true, bool turn_off_simp = false);
    bool solve ( bool do_simp = true, bool turn_off_simp = false);
    bool solve (Lit p , bool do_simp = true, bool turn_off_simp = false);
    bool solve (Lit p, Lit q, bool do_simp = true, bool turn_off_simp = false);
    bool solve (Lit p, Lit q, Lit r, bool do_simp = true, bool turn_off_simp = false);
    bool eliminate (bool turn_off_elim = false);
    virtual void garbageCollect();
#if 0
    void toDimacs (const char* file, const vec<Lit>& assumps);
    void toDimacs (const char* file);
    void toDimacs (const char* file, Lit p);
    void toDimacs (const char* file, Lit p, Lit q);
    void toDimacs (const char* file, Lit p, Lit q, Lit r);
#endif
    int grow;
    int clause_lim;
    int subsumption_lim;
    double simp_garbage_frac;
    bool use_asymm;
    bool use_rcheck;
    bool use_elim;
    bool extend_model;
    int merges;
    int asymm_lits;
    int eliminated_vars;
 protected:
    struct ElimLt {
        const LMap<int>& n_occ;
        explicit ElimLt(const LMap<int>& no) : n_occ(no) {}
        uint64_t cost (Var x) const { return (uint64_t)n_occ[mkLit(x)] * (uint64_t)n_occ[~mkLit(x)]; }
        bool operator()(Var x, Var y) const { return cost(x) < cost(y); }
    };
    struct ClauseDeleted {
        const ClauseAllocator& ca;
        explicit ClauseDeleted(const ClauseAllocator& _ca) : ca(_ca) {}
        bool operator()(const CRef& cr) const { return ca[cr].mark() == 1; } };
    int elimorder;
    bool use_simplification;
    Var max_simp_var;
    vec<uint32_t> elimclauses;
    VMap<char> touched;
    OccLists<Var, vec<CRef>, ClauseDeleted>
                        occurs;
    LMap<int> n_occ;
    Heap<Var,ElimLt> elim_heap;
    Queue<CRef> subsumption_queue;
    VMap<char> frozen;
    vec<Var> frozen_vars;
    VMap<char> eliminated;
    int bwdsub_assigns;
    int n_touched;
    CRef bwdsub_tmpunit;
    lbool solve_ (bool do_simp = true, bool turn_off_simp = false);
    bool asymm (Var v, CRef cr);
    bool asymmVar (Var v);
    void updateElimHeap (Var v);
    void gatherTouchedClauses ();
    bool merge (const Clause& _ps, const Clause& _qs, Var v, vec<Lit>& out_clause);
    bool merge (const Clause& _ps, const Clause& _qs, Var v, int& size);
    bool backwardSubsumptionCheck (bool verbose = false);
    bool eliminateVar (Var v);
    void extendModel ();
    void removeClause (CRef cr);
    bool strengthenClause (CRef cr, Lit l);
    bool implied (const vec<Lit>& c);
    void relocAll (ClauseAllocator& to);
};
inline bool SimpSolver::isEliminated (Var v) const { return eliminated[v]; }
inline void SimpSolver::updateElimHeap(Var v) {
    assert(use_simplification);
    if (elim_heap.inHeap(v) || (!frozen[v] && !isEliminated(v) && value(v) == l_Undef))
        elim_heap.update(v); }
inline bool SimpSolver::addClause (const vec<Lit>& ps) { ps.copyTo(add_tmp); return addClause_(add_tmp); }
inline bool SimpSolver::addEmptyClause() { add_tmp.clear(); return addClause_(add_tmp); }
inline bool SimpSolver::addClause (Lit p) { add_tmp.clear(); add_tmp.push(p); return addClause_(add_tmp); }
inline bool SimpSolver::addClause (Lit p, Lit q) { add_tmp.clear(); add_tmp.push(p); add_tmp.push(q); return addClause_(add_tmp); }
inline bool SimpSolver::addClause (Lit p, Lit q, Lit r) { add_tmp.clear(); add_tmp.push(p); add_tmp.push(q); add_tmp.push(r); return addClause_(add_tmp); }
inline bool SimpSolver::addClause (Lit p, Lit q, Lit r, Lit s){ add_tmp.clear(); add_tmp.push(p); add_tmp.push(q); add_tmp.push(r); add_tmp.push(s); return addClause_(add_tmp); }
inline void SimpSolver::setFrozen (Var v, bool b) { frozen[v] = (char)b; if (use_simplification && !b) { updateElimHeap(v); } }
inline void SimpSolver::freezeVar(Var v){
    if (!frozen[v]){
        frozen[v] = 1;
        frozen_vars.push(v);
    } }
inline void SimpSolver::thaw(){
    for (int i = 0; i < frozen_vars.size(); i++){
        Var v = frozen_vars[i];
        frozen[v] = 0;
        if (use_simplification)
            updateElimHeap(v);
    }
    frozen_vars.clear(); }
inline bool SimpSolver::solve ( bool do_simp, bool turn_off_simp) { budgetOff(); assumptions.clear(); return solve_(do_simp, turn_off_simp) == l_True; }
inline bool SimpSolver::solve (Lit p , bool do_simp, bool turn_off_simp) { budgetOff(); assumptions.clear(); assumptions.push(p); return solve_(do_simp, turn_off_simp) == l_True; }
inline bool SimpSolver::solve (Lit p, Lit q, bool do_simp, bool turn_off_simp) { budgetOff(); assumptions.clear(); assumptions.push(p); assumptions.push(q); return solve_(do_simp, turn_off_simp) == l_True; }
inline bool SimpSolver::solve (Lit p, Lit q, Lit r, bool do_simp, bool turn_off_simp) { budgetOff(); assumptions.clear(); assumptions.push(p); assumptions.push(q); assumptions.push(r); return solve_(do_simp, turn_off_simp) == l_True; }
inline bool SimpSolver::solve (const vec<Lit>& assumps, bool do_simp, bool turn_off_simp){
    budgetOff(); assumps.copyTo(assumptions); return solve_(do_simp, turn_off_simp) == l_True; }
inline lbool SimpSolver::solveLimited (const vec<Lit>& assumps, bool do_simp, bool turn_off_simp){
    assumps.copyTo(assumptions); return solve_(do_simp, turn_off_simp); }
}
#endif
