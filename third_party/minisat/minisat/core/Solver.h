#ifndef Minisat_Solver_h
#define Minisat_Solver_h 
#include "minisat/mtl/Vec.h"
#include "minisat/mtl/Heap.h"
#include "minisat/mtl/Alg.h"
#include "minisat/mtl/IntMap.h"
#include "minisat/utils/Options.h"
#include "minisat/core/SolverTypes.h"
namespace Minisat {
class Solver {
public:
    Solver();
    virtual ~Solver();
    Var newVar (lbool upol = l_Undef, bool dvar = true);
    void releaseVar(Lit l);
    bool addClause (const vec<Lit>& ps);
    bool addEmptyClause();
    bool addClause (Lit p);
    bool addClause (Lit p, Lit q);
    bool addClause (Lit p, Lit q, Lit r);
    bool addClause (Lit p, Lit q, Lit r, Lit s);
    bool addClause_( vec<Lit>& ps);
    bool simplify ();
    bool solve (const vec<Lit>& assumps);
    lbool solveLimited (const vec<Lit>& assumps);
    bool solve ();
    bool solve (Lit p);
    bool solve (Lit p, Lit q);
    bool solve (Lit p, Lit q, Lit r);
    bool okay () const;
    bool implies (const vec<Lit>& assumps, vec<Lit>& out);
    ClauseIterator clausesBegin() const;
    ClauseIterator clausesEnd() const;
    TrailIterator trailBegin() const;
    TrailIterator trailEnd () const;
    void toDimacs (FILE* f, const vec<Lit>& assumps);
    void toDimacs (const char *file, const vec<Lit>& assumps);
    void toDimacs (FILE* f, Clause& c, vec<Var>& map, Var& max);
    void toDimacs (const char* file);
    void toDimacs (const char* file, Lit p);
    void toDimacs (const char* file, Lit p, Lit q);
    void toDimacs (const char* file, Lit p, Lit q, Lit r);
    void setPolarity (Var v, lbool b);
    void setDecisionVar (Var v, bool b);
    lbool value (Var x) const;
    lbool value (Lit p) const;
    lbool modelValue (Var x) const;
    lbool modelValue (Lit p) const;
    int nAssigns () const;
    int nClauses () const;
    int nLearnts () const;
    int nVars () const;
    int nFreeVars () const;
    void printStats () const;
    void setConfBudget(int64_t x);
    void setPropBudget(int64_t x);
    void budgetOff();
    void interrupt();
    void clearInterrupt();
    virtual void garbageCollect();
    void checkGarbage(double gf);
    void checkGarbage();
    vec<lbool> model;
    LSet conflict;
    int verbosity;
    double var_decay;
    double clause_decay;
    double random_var_freq;
    double random_seed;
    bool luby_restart;
    int ccmin_mode;
    int phase_saving;
    bool rnd_pol;
    bool rnd_init_act;
    double garbage_frac;
    int min_learnts_lim;
    int restart_first;
    double restart_inc;
    double learntsize_factor;
    double learntsize_inc;
    int learntsize_adjust_start_confl;
    double learntsize_adjust_inc;
    uint64_t solves, starts, decisions, rnd_decisions, propagations, conflicts;
    uint64_t dec_vars, num_clauses, num_learnts, clauses_literals, learnts_literals, max_literals, tot_literals;
protected:
    struct VarData { CRef reason; int level; };
    static inline VarData mkVarData(CRef cr, int l){ VarData d = {cr, l}; return d; }
    struct Watcher {
        CRef cref;
        Lit blocker;
        Watcher(CRef cr, Lit p) : cref(cr), blocker(p) {}
        bool operator==(const Watcher& w) const { return cref == w.cref; }
        bool operator!=(const Watcher& w) const { return cref != w.cref; }
    };
    struct WatcherDeleted
    {
        const ClauseAllocator& ca;
        WatcherDeleted(const ClauseAllocator& _ca) : ca(_ca) {}
        bool operator()(const Watcher& w) const { return ca[w.cref].mark() == 1; }
    };
    struct VarOrderLt {
        const IntMap<Var, double>& activity;
        bool operator () (Var x, Var y) const { return activity[x] > activity[y]; }
        VarOrderLt(const IntMap<Var, double>& act) : activity(act) { }
    };
    struct ShrinkStackElem {
        uint32_t i;
        Lit l;
        ShrinkStackElem(uint32_t _i, Lit _l) : i(_i), l(_l){}
    };
    vec<CRef> clauses;
    vec<CRef> learnts;
    vec<Lit> trail;
    vec<int> trail_lim;
    vec<Lit> assumptions;
    VMap<double> activity;
    VMap<lbool> assigns;
    VMap<char> polarity;
    VMap<lbool> user_pol;
    VMap<char> decision;
    VMap<VarData> vardata;
    OccLists<Lit, vec<Watcher>, WatcherDeleted, MkIndexLit>
                        watches;
    Heap<Var,VarOrderLt>order_heap;
    bool ok;
    double cla_inc;
    double var_inc;
    int qhead;
    int simpDB_assigns;
    int64_t simpDB_props;
    double progress_estimate;
    bool remove_satisfied;
    Var next_var;
    ClauseAllocator ca;
    vec<Var> released_vars;
    vec<Var> free_vars;
    VMap<char> seen;
    vec<ShrinkStackElem>analyze_stack;
    vec<Lit> analyze_toclear;
    vec<Lit> add_tmp;
    double max_learnts;
    double learntsize_adjust_confl;
    int learntsize_adjust_cnt;
    int64_t conflict_budget;
    int64_t propagation_budget;
    bool asynch_interrupt;
    void insertVarOrder (Var x);
    Lit pickBranchLit ();
    void submitInstinctGpuRequest(bool force);
    void applyInstinctGpuVsidsInjection();
    void applyInstinctGpuPhaseInjection();
    void collectTopUnassignedDecisionVars(int limit, vec<Var>& out);
    void newDecisionLevel ();
    void uncheckedEnqueue (Lit p, CRef from = CRef_Undef);
    bool enqueue (Lit p, CRef from = CRef_Undef);
    CRef propagate ();
    void cancelUntil (int level);
    void analyze (CRef confl, vec<Lit>& out_learnt, int& out_btlevel);
    void analyzeFinal (Lit p, LSet& out_conflict);
    bool litRedundant (Lit p);
    lbool search (int nof_conflicts);
    lbool solve_ ();
    void reduceDB ();
    void removeSatisfied (vec<CRef>& cs);
    void rebuildOrderHeap ();
    void varDecayActivity ();
    void varBumpActivity (Var v, double inc);
    void varBumpActivity (Var v);
    void claDecayActivity ();
    void claBumpActivity (Clause& c);
    void attachClause (CRef cr);
    void detachClause (CRef cr, bool strict = false);
    void removeClause (CRef cr);
    bool isRemoved (CRef cr) const;
    bool locked (const Clause& c) const;
    bool satisfied (const Clause& c) const;
    int decisionLevel () const;
    uint32_t abstractLevel (Var x) const;
    CRef reason (Var x) const;
    int level (Var x) const;
    double progressEstimate () const;
    bool withinBudget () const;
    void relocAll (ClauseAllocator& to);
    static inline double drand(double& seed) {
        seed *= 1389796;
        int q = (int)(seed / 2147483647);
        seed -= (double)q * 2147483647;
        return seed / 2147483647; }
    static inline int irand(double& seed, int size) {
        return (int)(drand(seed) * size); }
};
inline CRef Solver::reason(Var x) const { return vardata[x].reason; }
inline int Solver::level (Var x) const { return vardata[x].level; }
inline void Solver::insertVarOrder(Var x) {
    if (!order_heap.inHeap(x) && decision[x]) order_heap.insert(x); }
inline void Solver::varDecayActivity() { var_inc *= (1 / var_decay); }
inline void Solver::varBumpActivity(Var v) { varBumpActivity(v, var_inc); }
inline void Solver::varBumpActivity(Var v, double inc) {
    if ( (activity[v] += inc) > 1e100 ) {
        for (int i = 0; i < nVars(); i++)
            activity[i] *= 1e-100;
        var_inc *= 1e-100; }
    if (order_heap.inHeap(v))
        order_heap.decrease(v); }
inline void Solver::claDecayActivity() { cla_inc *= (1 / clause_decay); }
inline void Solver::claBumpActivity (Clause& c) {
        if ( (c.activity() += cla_inc) > 1e20 ) {
            for (int i = 0; i < learnts.size(); i++)
                ca[learnts[i]].activity() *= 1e-20;
            cla_inc *= 1e-20; } }
inline void Solver::checkGarbage(void){ return checkGarbage(garbage_frac); }
inline void Solver::checkGarbage(double gf){
    if (ca.wasted() > ca.size() * gf)
        garbageCollect(); }
inline bool Solver::enqueue (Lit p, CRef from) { return value(p) != l_Undef ? value(p) != l_False : (uncheckedEnqueue(p, from), true); }
inline bool Solver::addClause (const vec<Lit>& ps) { ps.copyTo(add_tmp); return addClause_(add_tmp); }
inline bool Solver::addEmptyClause () { add_tmp.clear(); return addClause_(add_tmp); }
inline bool Solver::addClause (Lit p) { add_tmp.clear(); add_tmp.push(p); return addClause_(add_tmp); }
inline bool Solver::addClause (Lit p, Lit q) { add_tmp.clear(); add_tmp.push(p); add_tmp.push(q); return addClause_(add_tmp); }
inline bool Solver::addClause (Lit p, Lit q, Lit r) { add_tmp.clear(); add_tmp.push(p); add_tmp.push(q); add_tmp.push(r); return addClause_(add_tmp); }
inline bool Solver::addClause (Lit p, Lit q, Lit r, Lit s){ add_tmp.clear(); add_tmp.push(p); add_tmp.push(q); add_tmp.push(r); add_tmp.push(s); return addClause_(add_tmp); }
inline bool Solver::isRemoved (CRef cr) const { return ca[cr].mark() == 1; }
inline bool Solver::locked (const Clause& c) const { return value(c[0]) == l_True && reason(var(c[0])) != CRef_Undef && ca.lea(reason(var(c[0]))) == &c; }
inline void Solver::newDecisionLevel() { trail_lim.push(trail.size()); }
inline int Solver::decisionLevel () const { return trail_lim.size(); }
inline uint32_t Solver::abstractLevel (Var x) const { return 1 << (level(x) & 31); }
inline lbool Solver::value (Var x) const { return assigns[x]; }
inline lbool Solver::value (Lit p) const { return assigns[var(p)] ^ sign(p); }
inline lbool Solver::modelValue (Var x) const { return model[x]; }
inline lbool Solver::modelValue (Lit p) const { return model[var(p)] ^ sign(p); }
inline int Solver::nAssigns () const { return trail.size(); }
inline int Solver::nClauses () const { return num_clauses; }
inline int Solver::nLearnts () const { return num_learnts; }
inline int Solver::nVars () const { return next_var; }
inline int Solver::nFreeVars () const { return (int)dec_vars - (trail_lim.size() == 0 ? trail.size() : trail_lim[0]); }
inline void Solver::setPolarity (Var v, lbool b){ user_pol[v] = b; }
inline void Solver::setDecisionVar(Var v, bool b)
{
    if ( b && !decision[v]) dec_vars++;
    else if (!b && decision[v]) dec_vars--;
    decision[v] = b;
    insertVarOrder(v);
}
inline void Solver::setConfBudget(int64_t x){ conflict_budget = conflicts + x; }
inline void Solver::setPropBudget(int64_t x){ propagation_budget = propagations + x; }
inline void Solver::interrupt(){ asynch_interrupt = true; }
inline void Solver::clearInterrupt(){ asynch_interrupt = false; }
inline void Solver::budgetOff(){ conflict_budget = propagation_budget = -1; }
inline bool Solver::withinBudget() const {
    return !asynch_interrupt &&
           (conflict_budget < 0 || conflicts < (uint64_t)conflict_budget) &&
           (propagation_budget < 0 || propagations < (uint64_t)propagation_budget); }
inline bool Solver::solve () { budgetOff(); assumptions.clear(); return solve_() == l_True; }
inline bool Solver::solve (Lit p) { budgetOff(); assumptions.clear(); assumptions.push(p); return solve_() == l_True; }
inline bool Solver::solve (Lit p, Lit q) { budgetOff(); assumptions.clear(); assumptions.push(p); assumptions.push(q); return solve_() == l_True; }
inline bool Solver::solve (Lit p, Lit q, Lit r) { budgetOff(); assumptions.clear(); assumptions.push(p); assumptions.push(q); assumptions.push(r); return solve_() == l_True; }
inline bool Solver::solve (const vec<Lit>& assumps){ budgetOff(); assumps.copyTo(assumptions); return solve_() == l_True; }
inline lbool Solver::solveLimited (const vec<Lit>& assumps){ assumps.copyTo(assumptions); return solve_(); }
inline bool Solver::okay () const { return ok; }
inline ClauseIterator Solver::clausesBegin() const { return ClauseIterator(ca, &clauses[0]); }
inline ClauseIterator Solver::clausesEnd () const { return ClauseIterator(ca, &clauses[clauses.size()]); }
inline TrailIterator Solver::trailBegin () const { return TrailIterator(&trail[0]); }
inline TrailIterator Solver::trailEnd () const {
    return TrailIterator(&trail[decisionLevel() == 0 ? trail.size() : trail_lim[0]]); }
inline void Solver::toDimacs (const char* file){ vec<Lit> as; toDimacs(file, as); }
inline void Solver::toDimacs (const char* file, Lit p){ vec<Lit> as; as.push(p); toDimacs(file, as); }
inline void Solver::toDimacs (const char* file, Lit p, Lit q){ vec<Lit> as; as.push(p); as.push(q); toDimacs(file, as); }
inline void Solver::toDimacs (const char* file, Lit p, Lit q, Lit r){ vec<Lit> as; as.push(p); as.push(q); as.push(r); toDimacs(file, as); }
}
#endif
