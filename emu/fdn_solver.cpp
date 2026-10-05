// fdn_solver.cpp: native multicore FD solver behind picat-fdn.
//
// Propagation = Picat's CLP(FD) library semantics, as in the validated model
// cpeval/fdsim/fdsim.cpp (see its header): forward checking edges, anchored
// Hall checks, primal/dual channel rules, bounds-consistent linear
// constraints; singleton = bound; LIFO event processing. The search is
// Picat's labeling (leftmost / ff, ascending values, no x != v on retry), so
// solutions come out in exactly the order Picat would produce them, and the
// `backtracks` count is Picat's.
//
// Parallelism without changing that order: the search space is a list of
// tasks in DFS order. A task is a decision prefix plus, optionally, a value
// range (a, b] for the next variable. A thread running a task can donate the
// untried values of its shallowest open choice point as a new task, inserted
// right after its own task, which keeps the list in DFS order. Every task
// buffers its solutions, and the consumer (the Picat thread, in fdn_next)
// reads the tasks in list order. The consumer runs the head task itself, so
// small searches never touch the pool. Pool threads are started once a
// search exceeds SPAWN_NODES nodes, and they work on a run only while its
// consumer is active (inside fdn_next or within GRACE_MS of its last call):
// a run abandoned by a Picat cut does not keep the cores busy.
#include "fdn.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace std;
typedef uint64_t W;

struct Edge { int t, d; };
struct ChanR { int tuple, off, a; };
// op 0: = 0, 1: >= 0, 2: = 0 with AC at <= 2 unbound. c/a/xs: constants
// folded into c (bounds bookkeeping, ac2); c0/ta/tx/tk: the terms in the
// frame's order with constants in place (tx < 0: constant tk), which is the
// order of Picat's single propagation pass (chain_eq / chain_ge)
struct Lin { int op; long c; vector<long> a; vector<int> xs; long maxrange = 0;
             long c0; vector<long> ta, tk; vector<int> tx; };
struct Map { int t, s, k; };   // value w removed from owner -> remove s*w+k from t
struct Abs { int x, y, n; };   // |X-Y| = n
struct Mul { int x, y, z; };   // X*Y = Z
struct Div { int x, y, z; };   // X div y = Z, y a fixed positive divisor
struct Mm { int ismin, r; vector<int> ids; vector<long> vals; };   // R = min/max(xs), id < 0: constant vals[i]
struct Reif { int mode, b, x, y; };   // 0: B<=>X=c, 1: B<=>X!=c, 2: B<=>X=Y, 3: B<=>X!=Y, 4: B<=>X>=Y
struct Entl { int mode, b, x, y; };   // 0: B=>X=c, 1: B=>X!=c, 2: B=>X=Y, 3: B=>X!=Y
struct ElD { int i, v, t; };   // element(I, tuple, V): I fixed -> V = tuple[I-1]
struct ElF { int i, v, t; char rng; vector<int> rlo, rhi; };   // rng: value->range table present; value w (index w-glo): V=w -> I in [rlo, rhi]
struct Lex { int le; vector<pair<int,int>> pr; };   // lexicographic chain, le: last pair <=  else <

struct fdn_net {
    int glo, ghi, nw = 0, nv = 0, heur = 0, val = 0;
    vector<vector<int>> vals;
    vector<vector<Edge>> adj;
    vector<vector<int>> halls, vhall;
    map<vector<int>, int> hallkey;
    vector<vector<int>> tuples;
    map<vector<int>, int> tupkey;
    vector<vector<ChanR>> vchan;
    vector<vector<Map>> vmap, vbmap;   // vbmap: bounds of the owner -> bounds of t
    vector<char> chx;
    vector<Lin> lins;
    vector<vector<int>> vlin;
    vector<vector<pair<int,long>>> vlinA;  // var -> (constraint, total coefficient)
    vector<Abs> abss; vector<vector<int>> vabs;        // bound + change
    vector<Mul> muls; vector<vector<int>> vmul;        // bound
    vector<Div> divs; vector<vector<int>> vdiv;        // bound
    vector<Div> mods; vector<vector<int>> vmod;        // bound + change
    vector<Mm> mms; vector<vector<int>> vmm;           // bound
    vector<Reif> reifs; vector<vector<int>> vreif;     // change
    vector<Entl> ents; vector<vector<int>> vent;       // bound
    vector<vector<int>> ventc;                          // change (entail eq)
    vector<ElD> elds; vector<vector<int>> veld;        // bound
    vector<vector<int>> veldc;                          // change (unification maintenance)
    vector<vector<int>> velfv;                          // change, per removed value (element FC)
    vector<ElF> elves; vector<vector<int>> velf;       // bound + change
    vector<Lex> lexs; vector<vector<int>> vlex;        // bound
    vector<int> label;
    vector<W> D0; vector<int> SZ0, MN0, MX0; vector<char> chg;
    void finalize() {
        nv = vals.size(); nw = (ghi - glo + 64) / 64; vlinA.clear();
        D0.assign((size_t)nv * nw, 0); SZ0.resize(nv); MN0.resize(nv); MX0.resize(nv); chg.assign(nv, 0); chx.assign(nv, 0);
        for (auto &l : lins) for (size_t i = 0; i < l.xs.size(); i++) {
            const vector<int> &v = vals[l.xs[i]];
            l.maxrange = max(l.maxrange, labs(l.a[i]) * (long)(*max_element(v.begin(), v.end()) - *min_element(v.begin(), v.end())));
        }
        for (int x = 0; x < nv; x++) {
            for (int v : vals[x]) { int b = v - glo; D0[(size_t)x * nw + (b >> 6)] |= 1ULL << (b & 63); }
            SZ0[x] = vals[x].size();
            MN0[x] = *min_element(vals[x].begin(), vals[x].end());
            MX0[x] = *max_element(vals[x].begin(), vals[x].end());
            auto &a = adj[x];
            sort(a.begin(), a.end(), [](const Edge &p, const Edge &q) { return p.t != q.t ? p.t < q.t : p.d < q.d; });
            a.erase(unique(a.begin(), a.end(), [](const Edge &p, const Edge &q) { return p.t == q.t && p.d == q.d; }), a.end());
            auto &c = vchan[x];
            sort(c.begin(), c.end(), [](const ChanR &p, const ChanR &q) { return make_tuple(p.tuple, p.off, p.a) < make_tuple(q.tuple, q.off, q.a); });
            c.erase(unique(c.begin(), c.end(), [](const ChanR &p, const ChanR &q) { return p.tuple == q.tuple && p.off == q.off && p.a == q.a; }), c.end());
            chg[x] = !vhall[x].empty();
            vlinA.emplace_back();
            for (int s : vlin[x]) { long a = 0; for (size_t i = 0; i < lins[s].xs.size(); i++) if (lins[s].xs[i] == x) a += lins[s].a[i]; vlinA[x].push_back({s, a}); }
            chx[x] = !vchan[x].empty() || !vmap[x].empty();
        }
    }
};
extern "C" {
fdn_net *fdn_net_new(int glo, int ghi) { auto n = new fdn_net; n->glo = glo; n->ghi = ghi; return n; }
int fdn_var(fdn_net *n, int k, const int *v) {
    n->vals.emplace_back(v, v + k); n->adj.emplace_back(); n->vhall.emplace_back();
    n->vchan.emplace_back(); n->vlin.emplace_back(); n->vmap.emplace_back(); n->vbmap.emplace_back();
    n->vabs.emplace_back(); n->vmul.emplace_back(); n->vdiv.emplace_back(); n->vmod.emplace_back(); n->veldc.emplace_back(); n->velfv.emplace_back(); n->ventc.emplace_back();
    n->vmm.emplace_back(); n->vreif.emplace_back(); n->vent.emplace_back(); n->veld.emplace_back();
    n->velf.emplace_back(); n->vlex.emplace_back();
    return n->vals.size() - 1;
}
void fdn_edge(fdn_net *n, int o, int t, int d) { n->adj[o].push_back({t, d}); }
void fdn_hall(fdn_net *n, int k, const int *xs) {
    vector<int> m(xs, xs + k); sort(m.begin(), m.end()); m.erase(unique(m.begin(), m.end()), m.end());
    if (n->hallkey.count(m)) return;
    int id = n->halls.size(); n->hallkey[m] = id; n->halls.push_back(m);
    for (int x : m) n->vhall[x].push_back(id);
}
int fdn_tuple(fdn_net *n, int k, const int *xs) {
    vector<int> t(xs, xs + k);
    auto it = n->tupkey.find(t); if (it != n->tupkey.end()) return it->second;
    int id = n->tuples.size(); n->tupkey[t] = id; n->tuples.push_back(t); return id;
}
void fdn_chan(fdn_net *n, int x, int t, int off, int a) { n->vchan[x].push_back({t, off, a}); }
void fdn_map(fdn_net *n, int x, int t, int s, int k) { n->vmap[x].push_back({t, s, k}); }
void fdn_bmap(fdn_net *n, int y, int t, int s, int k) { n->vbmap[y].push_back({t, s, k}); }
void fdn_linear(fdn_net *n, int op, long c, int k, const long *a, const int *xs, const long *kv) {
    Lin l; l.op = op; l.c = l.c0 = c;
    for (int i = 0; i < k; i++) {
        l.ta.push_back(a[i]); l.tx.push_back(xs[i]); l.tk.push_back(xs[i] < 0 ? kv[i] : 0);
        if (xs[i] < 0) l.c += a[i] * kv[i]; else { l.a.push_back(a[i]); l.xs.push_back(xs[i]); }
    }
    int id = n->lins.size(); n->lins.push_back(l);
    vector<int> m(l.xs); sort(m.begin(), m.end()); m.erase(unique(m.begin(), m.end()), m.end());
    for (int x : m) n->vlin[x].push_back(id);
}
void fdn_abs(fdn_net *n, int x, int y, int nn) {
    int id = n->abss.size(); n->abss.push_back({x, y, nn});
    n->vabs[x].push_back(id); n->vabs[y].push_back(id);
}
void fdn_mul(fdn_net *n, int x, int y, int z) {
    int id = n->muls.size(); n->muls.push_back({x, y, z});
    n->vmul[x].push_back(id); n->vmul[y].push_back(id); n->vmul[z].push_back(id);
}
void fdn_div(fdn_net *n, int x, int y, int z) {
    int id = n->divs.size(); n->divs.push_back({x, y, z});
    n->vdiv[x].push_back(id); n->vdiv[z].push_back(id);
}
void fdn_mod(fdn_net *n, int x, int y, int z) {
    int id = n->mods.size(); n->mods.push_back({x, y, z});
    n->vmod[x].push_back(id); n->vmod[z].push_back(id);
}
void fdn_mm(fdn_net *n, int ismin, int r, int k, const int *ids, const long *vals) {
    Mm m{ismin, r, vector<int>(ids, ids + k), vector<long>(vals, vals + k)};
    int id = n->mms.size(); n->mms.push_back(m);
    n->vmm[r].push_back(id);
    for (int i = 0; i < k; i++) if (m.ids[i] >= 0) n->vmm[m.ids[i]].push_back(id);
}
void fdn_reif(fdn_net *n, int mode, int b, int x, int y) {
    int id = n->reifs.size(); n->reifs.push_back({mode, b, x, y});
    n->vreif[b].push_back(id); n->vreif[x].push_back(id);
    if (mode == 2 || mode == 3 || mode == 4) n->vreif[y].push_back(id);
}
void fdn_entail(fdn_net *n, int mode, int b, int x, int y) {
    int id = n->ents.size(); n->ents.push_back({mode, b, x, y});
    n->vent[b].push_back(id); n->vent[x].push_back(id);
    if (mode == 2 || mode == 3) n->vent[y].push_back(id);
    if (mode == 0 || mode == 2) n->ventc[b].push_back(id);   // entail eq: fires on changes too
}
void fdn_eldelay(fdn_net *n, int i, int v, int t) {
    int id = n->elds.size(); n->elds.push_back({i, v, t});
    n->veld[i].push_back(id);
    n->veldc[i].push_back(id); n->veldc[v].push_back(id);
    for (int w : n->tuples[t]) n->veldc[w].push_back(id);
}
void fdn_elfast(fdn_net *n, int i, int v, int t, int nsup, const long *ks, const long *lo, const long *hi) {
    ElF f{}; f.i = i; f.v = v; f.t = t; f.rng = 1;
    f.rlo.assign(n->ghi - n->glo + 1, 0); f.rhi.assign(n->ghi - n->glo + 1, -1);
    for (int k = 0; k < nsup; k++) {
        if (ks[k] < n->glo || ks[k] > n->ghi || lo[k] > hi[k]) continue;
        f.rlo[ks[k] - n->glo] = (int)lo[k]; f.rhi[ks[k] - n->glo] = (int)hi[k];
    }
    int id = n->elves.size(); n->elves.push_back(f);
    n->velf[i].push_back(id); n->velf[v].push_back(id);
    n->velfv[i].push_back(id); n->velfv[v].push_back(id);   // change-triggered FC
}
void fdn_elfc(fdn_net *n, int i, int v, int t) {
    ElF f{}; f.i = i; f.v = v; f.t = t; f.rng = 0;
    int id = n->elves.size(); n->elves.push_back(f);
    n->velfv[i].push_back(id); n->velfv[v].push_back(id);   // change-triggered FC only
}
void fdn_lex(fdn_net *n, int le, int np, const int *pr) {
    Lex l{}; l.le = le;
    for (int i = 0; i < np; i++) l.pr.push_back({pr[2 * i], pr[2 * i + 1]});
    int id = n->lexs.size(); n->lexs.push_back(l);
    for (int i = 0; i < np - 1; i++) { n->vlex[l.pr[i].first].push_back(id); n->vlex[l.pr[i].second].push_back(id); }
}
void fdn_label(fdn_net *n, int h, int v, int k, const int *xs) { n->heur = h; n->val = v; n->label.assign(xs, xs + k); }
void fdn_net_free(fdn_net *n) { delete n; }
}

// ------------------------------------------------------------------ search
enum { E_BOUND, E_CHG, E_CHX, E_LIN, E_BMAP, E_REIF };
struct Ev { int k, x, v; };
struct TE { size_t w; W old; int x, sz, mn, mx; };
enum { R_SOL, R_DONE, R_PAUSE };

struct Task;
struct Search {
    const fdn_net &N; const int NW, GLO, GHI;
    vector<W> D; vector<int> SZ, MN, MX;
    vector<TE> TR; vector<uint64_t> STAMP; uint64_t EPOCH = 1;
    vector<Ev> EV; vector<char> PEND, PENDL, PENDR;
    int SEED = -1;                          // the linear constraint being run (see setword)
    vector<long> SMIN, SMAX; vector<int> NUNB;   // per linear constraint, incremental
    struct CP { int x, v, lim, ls, kind, d, ph, lo, hi; size_t tm, dom; };
    int ls = 0;                             // label list: all entries before ls are bound
    vector<CP> st; vector<W> DS;            // DS: domain snapshots of the CP variables
    long bt = 0, nodes = 0;
    bool counting = false; long nsol = 0;  // count mode: solutions are counted, not returned
    int state = 0;                          // 0 select, 1 backtrack, 2 done
    vector<pair<int,int>> prefix;

    Search(const fdn_net &n, bool init_sums = true) : N(n), NW(n.nw), GLO(n.glo), GHI(n.ghi), D(n.D0), SZ(n.SZ0), MN(n.MN0), MX(n.MX0),
        STAMP(n.D0.size(), 0), PEND(n.nv, 0), PENDL(n.lins.size(), 0), PENDR(n.nv, 0),
        SMIN(n.lins.size()), SMAX(n.lins.size()), NUNB(n.lins.size(), 0) {
        if (!init_sums) return;
        for (size_t s = 0; s < n.lins.size(); s++) { SMIN[s] = SMAX[s] = n.lins[s].c; }
        for (int x = 0; x < n.nv; x++)
            for (auto &p : n.vlinA[x]) {
                long a = p.second;
                SMIN[p.first] += a > 0 ? a * MN[x] : a * MX[x]; SMAX[p.first] += a > 0 ? a * MX[x] : a * MN[x];
                if (SZ[x] > 1) NUNB[p.first]++;
            }
    }
    void lin_delta(int x, int omn, int omx, int osz) {     // keep SMIN/SMAX/NUNB in step with x
        for (auto &p : N.vlinA[x]) {
            long a = p.second; int s = p.first;
            if (a > 0) { SMIN[s] += a * (MN[x] - omn); SMAX[s] += a * (MX[x] - omx); }
            else { SMIN[s] += a * (MX[x] - omx); SMAX[s] += a * (MN[x] - omn); }
            NUNB[s] += (SZ[x] > 1) - (osz > 1);
        }
    }

    W *dom(int x) { return &D[(size_t)x * NW]; }
    void mm(int x) {
        W *d = dom(x); int k;
        for (k = 0; !d[k]; k++) ;
        MN[x] = GLO + k * 64 + __builtin_ctzll(d[k]);
        for (k = NW - 1; !d[k]; k--) ;
        MX[x] = GLO + k * 64 + 63 - __builtin_clzll(d[k]);
    }
    bool setword(int x, int k, W nw) {
        size_t w = (size_t)x * NW + k; W old = D[w], rem = old & ~nw;
        if (!rem) return true;
        if (STAMP[w] != EPOCH) { STAMP[w] = EPOCH; TR.push_back({w, old, x, SZ[x], MN[x], MX[x]}); }
        int omn = MN[x], omx = MX[x], osz = SZ[x];
        D[w] = nw; SZ[x] -= __builtin_popcountll(rem);
        if (SZ[x] == 0) { SZ[x] = osz; D[w] = old; return false; }
        mm(x);
        if (!N.vlinA[x].empty()) lin_delta(x, omn, omx, osz);
        if (N.chx[x])
            for (W r = rem; r; r &= r - 1) EV.push_back({E_CHX, x, GLO + k * 64 + __builtin_ctzll(r)});
        if (!N.vabs[x].empty() || !N.velf[x].empty() || !N.velfv[x].empty() || !N.vmod[x].empty())
            for (W r = rem; r; r &= r - 1) EV.push_back({E_REIF, x, GLO + k * 64 + __builtin_ctzll(r)});
        if (!N.vreif[x].empty() && !PENDR[x]) { PENDR[x] = 1; EV.push_back({E_REIF, x, -1}); }
        if (N.chg[x] && !PEND[x]) { PEND[x] = 1; EV.push_back({E_CHG, x, 0}); }
        // Picat wakes a linear constraint on ins and bound (min/max) events of
        // its variables, but not on its own min/max changes (the trigger
        // carries the running frame as seed: INSERT_TRIGGER_minmax_checkseed);
        // its own bindings do wake it (ASSIGN_DVAR, ins). An ARC constraint
        // with <= 2 unbound variables is '$binary_constr_eq', arc consistent,
        // woken by any change. Picat runs the constraint once per event, so
        // a constraint already pending is queued again (its pass is not
        // idempotent)
        bool bnd = SZ[x] == 1 || MN[x] != omn || MX[x] != omx;
        if (bnd && !N.vbmap[x].empty()) EV.push_back({E_BMAP, x, 0});
        for (int s : N.vlin[x]) {
            bool ac = N.lins[s].op == 2 && NUNB[s] <= 2;
            if (ac || (bnd && (s != SEED || SZ[x] == 1))) { PENDL[s] = 1; EV.push_back({E_LIN, s, 0}); }
        }
        if (SZ[x] == 1) EV.push_back({E_BOUND, x, 0});
        return true;
    }
    bool has(int x, int v) { if (v < GLO || v > GHI) return false; int b = v - GLO; return dom(x)[b >> 6] >> (b & 63) & 1; }
    bool remove_val(int x, int v) {
        if (!has(x, v)) return true;
        int b = v - GLO; return setword(x, b >> 6, dom(x)[b >> 6] & ~(1ULL << (b & 63)));
    }
    bool assign(int x, int v) {
        if (!has(x, v)) return false;
        int b = v - GLO;
        for (int k = 0; k < NW; k++) if (!setword(x, k, k == (b >> 6) ? 1ULL << (b & 63) : 0)) return false;
        return true;
    }
    bool restrict_bounds(int x, long lo, long hi) {
        if (lo > MX[x] || hi < MN[x]) return false;
        for (int k = 0; k < NW; k++) {
            W m = ~0ULL; long a = lo - GLO - k * 64L, b = hi - GLO - k * 64L;
            if (a > 63 || b < 0) m = 0;
            else { if (a > 0) m &= ~0ULL << a; if (b < 63) m &= ~0ULL >> (63 - b); }
            if (!setword(x, k, dom(x)[k] & m)) return false;
        }
        return true;
    }
    bool hall(int g, int x) {
        const vector<int> &xs = N.halls[g]; W *dx = dom(x);
        int size = SZ[x], count = 1;
        static thread_local vector<char> sub; sub.assign(xs.size(), 0);
        for (size_t i = 0; i < xs.size(); i++) {
            int y = xs[i]; if (y == x || SZ[y] == 1) continue;
            W *dy = dom(y); bool s = true;
            for (int k = 0; k < NW; k++) if (dy[k] & ~dx[k]) { s = false; break; }
            if (s) { sub[i] = 1; count++; }
        }
        if (count > size) return false;
        if (count < size) return true;
        static thread_local vector<W> mask; mask.assign(dx, dx + NW);
        for (size_t i = 0; i < xs.size(); i++) {
            int y = xs[i]; if (y == x || sub[i] || SZ[y] == 1) continue;
            for (int k = 0; k < NW; k++) if (!setword(y, k, dom(y)[k] & ~mask[k])) return false;
        }
        return true;
    }
    static long fdiv(long a, long b) { long q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
    static long cdiv(long a, long b) { return -fdiv(-a, b); }
    bool ac2(const Lin &c) {               // arc consistency, <= 2 unbound variables
        long K = c.c; int u[2], m = 0;
        for (size_t i = 0; i < c.xs.size(); i++) {
            int y = c.xs[i];
            if (SZ[y] == 1) K += c.a[i] * MN[y]; else u[m++] = i;
        }
        if (m == 0) return K == 0;
        if (m == 1) { long a = c.a[u[0]]; if ((-K) % a) return false; long v = -K / a;
            return v >= GLO && v <= GHI && assign(c.xs[u[0]], (int)v); }
        for (int d = 0; d < 2; d++) {
            int x = c.xs[u[d]], y = c.xs[u[1 - d]]; long a = c.a[u[d]], b = c.a[u[1 - d]];
            for (int vx = MN[x], hi = MX[x]; vx <= hi; vx++) {
                if (!has(x, vx)) continue;
                long t = -(K + a * vx);
                if (t % b == 0 && has(y, (int)(t / b))) continue;
                if (!remove_val(x, vx)) return false;
            }
        }
        return true;
    }
    // narrow x to [lo, hi] as Picat's CALL_DOMAIN_REGION (fails if empty)
    bool region(int x, long lo, long hi) {
        if (lo > hi) return false;
        if (lo <= MN[x] && hi >= MX[x]) return true;
        return restrict_bounds(x, max<long>(lo, MN[x]), min<long>(hi, MX[x]));
    }
    // bounds of term i (a * x, or a * constant); bound variables count as constants
    void tbounds(const Lin &c, size_t i, long &lo, long &hi) {
        int x = c.tx[i]; long a = c.ta[i];
        if (x < 0 || SZ[x] == 1) { lo = hi = a * (x < 0 ? c.tk[i] : MN[x]); return; }
        if (a > 0) { lo = a * MN[x]; hi = a * MX[x]; } else { lo = a * MX[x]; hi = a * MN[x]; }
    }
    // narrow the partial sum [l1, u1] to [first, last]; 0: fail, 1: stop the
    // pass (no narrowing), 2: continue (the tail of REDUCE_DOMAIN_* in clpfd.h)
    static int narrow(long &l1, long &u1, long first, long last) {
        if (first <= l1) { if (last >= u1) return 1; u1 = last; }
        else { l1 = first; if (last < u1) u1 = last; }
        return u1 < l1 ? 0 : 2;
    }
    // Picat's nary_interval_consistent_eq (clpfd_libs.c): partial sums
    // T0 = c, T(i+1) = T(i) + a_i x_i; reduce the last term from T(n-1), then
    // the others from the last down, each from T(i) and the narrowed T(i+1),
    // narrowing T(i) in turn; stop as soon as a partial sum cannot narrow
    bool chain_eq(const Lin &c) {
        size_t n = c.tx.size();
        static thread_local vector<long> tl, tu; tl.resize(n + 1); tu.resize(n + 1);
        tl[0] = tu[0] = c.c0;
        for (size_t i = 0; i + 1 < n; i++) { long lo, hi; tbounds(c, i, lo, hi); tl[i + 1] = tl[i] + lo; tu[i + 1] = tu[i] + hi; }
        for (size_t j = n; j-- > 0;) {
            int x = c.tx[j]; long a = c.ta[j], A = labs(a); bool var = x >= 0 && SZ[x] > 1;
            long xv = var ? 0 : A * (x < 0 ? c.tk[j] : MN[x]);
            long first, last, &l1 = tl[j], &u1 = tu[j];
            if (j == n - 1) {                       // T(n-1) + a x = 0
                if (a > 0) {
                    first = -u1; last = -l1;
                    if (var) { if (!region(x, cdiv(first, A), fdiv(last, A))) return false; first = -(A * MX[x]); last = -(A * MN[x]); }
                    else { if (xv < first || xv > last) return false; first = last = -xv; }
                } else {
                    first = l1; last = u1;
                    if (var) { if (!region(x, cdiv(first, A), fdiv(last, A))) return false; first = A * MN[x]; last = A * MX[x]; }
                    else { if (xv < first || xv > last) return false; first = last = xv; }
                }
            } else {                                // T(j+1) = T(j) + a x
                long l2 = tl[j + 1], u2 = tu[j + 1];
                if (a > 0) {
                    first = l2 - u1; last = u2 - l1;
                    if (var) { if (!region(x, cdiv(first, A), fdiv(last, A))) return false; first = l2 - A * MX[x]; last = u2 - A * MN[x]; }
                    else { if (xv < first || xv > last) return false; first = l2 - xv; last = u2 - xv; }
                } else {
                    first = l1 - u2; last = u1 - l2;
                    if (var) { if (!region(x, cdiv(first, A), fdiv(last, A))) return false; first = l2 + A * MN[x]; last = u2 + A * MX[x]; }
                    else { if (xv < first || xv > last) return false; first = xv + l2; last = xv + u2; }
                }
            }
            int r = narrow(l1, u1, first, last);
            if (r != 2) return r == 1;
        }
        return true;
    }
    // Picat's nary_interval_consistent_ge: T(n) >= 0, lower bounds only
    bool chain_ge(const Lin &c) {
        size_t n = c.tx.size();
        static thread_local vector<long> tl, tu; tl.resize(n + 1); tu.resize(n + 1);
        tl[0] = tu[0] = c.c0;
        for (size_t i = 0; i < n; i++) { long lo, hi; tbounds(c, i, lo, hi); tl[i + 1] = tl[i] + lo; tu[i + 1] = tu[i] + hi; }
        if (tl[n] >= 0) return true;                // entailed (Picat kills the frame)
        if (tu[n] < 0) return false;
        tl[n] = 0;
        for (size_t j = n; j-- > 0;) {
            int x = c.tx[j]; long a = c.ta[j], A = labs(a); bool var = x >= 0 && SZ[x] > 1;
            long xv = var ? 0 : A * (x < 0 ? c.tk[j] : MN[x]);
            long l2 = tl[j + 1], first;
            if (a > 0) {                            // T(j+1) = T(j) + a x >= l2
                first = l2 - tu[j];
                if (var) { long f0 = cdiv(first, A); if (f0 > MN[x] && !region(x, f0, MX[x])) return false; first = l2 - A * MX[x]; }
                else { if (xv < first) return false; first = l2 - xv; }
            } else {                                // T(j+1) = T(j) - A x >= l2
                long last = tu[j] - l2;
                if (var) { long l0 = fdiv(last, A); if (l0 < MX[x] && !region(x, MN[x], l0)) return false; first = l2 + A * MN[x]; }
                else { if (xv > last) return false; first = xv + l2; }
            }
            if (first < tl[j]) return true;
            tl[j] = first;
        }
        return true;
    }
    bool linear(int s) {
        const Lin &c = N.lins[s];
        if (c.op == 2 && NUNB[s] <= 2) return ac2(c);
        long smin = SMIN[s], smax = SMAX[s];
        if (c.op == 1 ? smin >= 0 || smax >= c.maxrange                    // entailed / nothing can be pruned
                      : smin <= 0 && smax >= 0 && min(smax, -smin) >= c.maxrange) return true;
        SEED = s;
        bool ok = c.op == 1 ? chain_ge(c) : chain_eq(c);
        SEED = -1;
        return ok;
    }
    // ------- new propagators (rule replays of Picat's action rules) -------
    bool abs_prop(int s) {                 // |X-Y| = n: ins rule (bounds) on a bound event
        const Abs &c = N.abss[s];
        int x = c.x, y = c.y;
        if (SZ[x] == 1) {
            long v = MN[x];
            if (!restrict_bounds(y, v - c.n, v + c.n)) return false;
        }
        if (SZ[y] == 1) {
            long v = MN[y];
            if (!restrict_bounds(x, v - c.n, v + c.n)) return false;
        }
        return true;
    }
    bool abs_dom(int s, int own, int ex) { // FC on a value removed from own
        const Abs &c = N.abss[s];
        int oth = own == c.x ? c.y : c.x;
        long e1 = ex - c.n, e2 = ex + c.n;
        if (!has(own, (int)(e1 - c.n)) && has(oth, (int)e1) && !remove_val(oth, (int)e1)) return false;
        if (!has(own, (int)(e2 + c.n)) && has(oth, (int)e2) && !remove_val(oth, (int)e2)) return false;
        return true;
    }
    static long fldiv(long a, long b) { long q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
    static long sldiv(long a, long b) { if (b < 0) { b = -b; a = -a; } return a >= 0 ? a / b : fldiv(a, b); }
    static long sudiv(long a, long b) { if (b < 0) { b = -b; a = -a; } return a <= 0 ? a / b : (a % b ? a / b + 1 : a / b); }
    bool mul_prop(int s) {
        const Mul &c = N.muls[s];
        int x = c.x, y = c.y, z = c.z;
        static const bool tr_ = getenv("FDN_TRACE") != NULL;
        if (tr_) fprintf(stderr, "  mul s=%d x=(%d,%ld,%ld,%d) y=(%d,%ld,%ld,%d) z=(%d,%ld,%ld,%d)\n", s, x, (long)MN[x], (long)MX[x], SZ[x], y, (long)MN[y], (long)MX[y], SZ[y], z, (long)MN[z], (long)MX[z], SZ[z]);
        if (SZ[x] == 1 && SZ[y] == 1) {
            long p = (long)MN[x] * MN[y];
            return SZ[z] == 1 ? MN[z] == p : assign(z, (int)p);
        }
        if (SZ[x] == 1 && SZ[y] > 1 && SZ[z] > 1) {          // the rule posts the 2-var ARC linear
            if (MN[x] == 0) return SZ[z] == 1 ? MN[z] == 0 : assign(z, 0);
            Lin t{0, 0, {(long)MN[x], -1}, {y, z}};
            return ac2(t);
        }
        if (SZ[y] == 1 && SZ[x] > 1 && SZ[z] > 1) {
            if (MN[y] == 0) return SZ[z] == 1 ? MN[z] == 0 : assign(z, 0);
            Lin t{0, 0, {(long)MN[y], -1}, {x, z}};
            return ac2(t);
        }
        if (SZ[z] == 1 && SZ[x] > 1 && SZ[y] > 1) {          // the factoring bounds of b_CLPFD_MULTIPLY_INT_ccc
            long z0 = MN[z];
            if (x == y) {                                    // square: exact root filtering, X in {r,-r} (or {0})
                if (z0 < 0) return false;
                long r = (long)sqrt((double)z0);
                while (r * r > z0) r--;
                while ((r + 1) * (r + 1) <= z0) r++;
                if (r * r != z0) return false;
                if (!has(x, (int)r) && !has(x, (int)-r)) return false;
                for (long v = MN[x]; v <= MX[x]; v++) {
                    if (!has(x, (int)v) || v == r || v == -r) continue;
                    if (!remove_val(x, (int)v)) return false;
                }
                return true;
            }
            long mnx = MN[x], mxx = MX[x], mny = MN[y], mxy = MX[y];
            if (z0 > 0 && mnx >= 0) { if (!restrict_bounds(y, 1, mxy)) return false; mny = MN[y]; mxy = MX[y]; if (SZ[y] == 1) { if (!assign(x, (int)(z0 / mny)) || (long)MN[x] * mny != z0) return false; return true; } }
            if (z0 > 0 && mny >= 0) { if (!restrict_bounds(x, 1, mxx)) return false; mnx = MN[x]; mxx = MX[x]; if (SZ[x] == 1) { if (!assign(y, (int)(z0 / mnx)) || (long)MN[y] * mnx != z0) return false; return true; } }
            if (z0 != 0) {
                if (has(x, 0) && !remove_val(x, 0)) return false;
                if (has(y, 0) && !remove_val(y, 0)) return false;
                mnx = MN[x]; mxx = MX[x]; mny = MN[y]; mxy = MX[y];
            }
            long lowZ = min(min((long)mnx * mny, (long)mnx * mxy), min((long)mxx * mny, (long)mxx * mxy));
            long upZ = max(max((long)mnx * mny, (long)mnx * mxy), max((long)mxx * mny, (long)mxx * mxy));
            if (z0 < lowZ || z0 > upZ) return false;
            if (mnx >= 0 && mny >= 0 && z0 > 0) {
                if (mxx > z0 && !restrict_bounds(x, mnx, (int)z0)) return false;
                mxx = MX[x]; if (SZ[x] == 1) { if (!assign(y, (int)(z0 / mxx)) || (long)MN[y] * mxx != z0) return false; return true; }
                if (mxy > z0 && !restrict_bounds(y, mny, (int)z0)) return false;
                mxy = MX[y]; if (SZ[y] == 1) { if (!assign(x, (int)(z0 / mxy)) || (long)MN[x] * mxy != z0) return false; return true; }
                mnx = MN[x]; mxx = MX[x];
            }
            if ((mnx > 0 && mxx < 1L << 40) || (mxx < 0 && mnx > -(1L << 40))) {
                long lowY = min(min(sldiv(z0, mnx), sldiv(z0, mxx)), min(sldiv(z0, mny), sldiv(z0, mxy)));
                long upY = max(max(sudiv(z0, mnx), sudiv(z0, mxx)), max(sudiv(z0, mny), sudiv(z0, mxy)));
                if (!restrict_bounds(y, max(lowY, mny), min(upY, mxy))) return false;
                if (SZ[y] == 1) { if (!assign(x, (int)(z0 / MN[y])) || (long)MN[x] * MN[y] != z0) return false; return true; }
                mny = MN[y]; mxy = MX[y];
            }
            if ((mny > 0 && mxy < 1L << 40) || (mxy < 0 && mny > -(1L << 40))) {
                long lowX = min(min(sldiv(z0, mny), sldiv(z0, mxy)), min(sldiv(z0, mnx), sldiv(z0, mxx)));
                long upX = max(max(sudiv(z0, mny), sudiv(z0, mxy)), max(sudiv(z0, mnx), sudiv(z0, mxx)));
                if (!restrict_bounds(x, max(lowX, mnx), min(upX, mxx))) return false;
                if (SZ[x] == 1) { if (!assign(y, (int)(z0 / MN[x])) || (long)MN[y] * MN[x] != z0) return false; return true; }
            }
            return true;
        }
        return true;                           // all unbound: no propagation (Picat waits for ins events)
    }
    bool div_prop(int s) {
        const Div &c = N.divs[s];
        int x = c.x, z = c.z; long y = c.y;
        if (SZ[x] == 1) {                      // X fixed: Z = X div y
            long q = fldiv(MN[x], y);
            return SZ[z] == 1 ? MN[z] == q : assign(z, (int)q);
        }
        long lo = fldiv(MN[x], y), hi = fldiv(MX[x], y);
        if (SZ[z] == 1) {                      // Z fixed: X in [y*Z, y*(Z+1)-1]
            long q = MN[z];
            if (q < lo || q > hi) return false;
            return restrict_bounds(x, y * q, y * (q + 1) - 1);
        }
        if (!restrict_bounds(z, lo, hi)) return false;
        return restrict_bounds(x, y * MN[z], y * (MX[z] + 1) - 1);
    }
    bool div_dom(int s, int ev) {          // the arc rule: v removed from Z -> exclude [y*v, y*v+y-1] from X
        const Div &c = N.divs[s];
        int x = c.x; long y = c.y;
        long lo = y * (long)ev, hi = y * (long)ev + y - 1;
        if (lo > MX[x] || hi < MN[x]) return true;
        return restrict_bounds(x, max(lo, (long)MN[x]), min(hi, (long)MX[x]));
    }
    bool mod_prop(int s) {                 // X mod y = Z: full domain filtering (b_MOD_CON_ccc)
        const Div &c = N.mods[s];
        int x = c.x, z = c.z; long y = c.y;
        if (MX[x] >= 3000) return true;
        for (long v = MN[x]; v <= MX[x]; v++) {
            if (!has(x, (int)v)) continue;
            if (!has(z, (int)(v % y)) && !remove_val(x, (int)v)) return false;
        }
        return true;
    }
    bool mm_prop(int s) {                  // R = min/max(xs): b_PROP_MIN_c / b_PROP_MAX_c
        const Mm &c = N.mms[s];
        int r = c.r;
        long acc_min = LONG_MAX, acc_max = LONG_MIN; int nunb = 0, uvar = -1;
        if (c.ismin) {
            long first1 = SZ[r] == 1 ? MN[r] : MN[r], last1 = MX[r];
            for (size_t i = 0; i < c.ids.size(); i++) {
                int id = c.ids[i];
                if (id < 0) { long v = c.vals[i]; if (v < first1) return false; if (v < acc_min) acc_min = v; if (v > acc_max) acc_max = v; continue; }
                nunb++; uvar = id;
                if (MN[id] < first1 && !restrict_bounds(id, first1, MX[id])) return false;
                if (MX[id] > acc_max) acc_max = MX[id];
            }
            if (nunb == 0) return SZ[r] == 1 ? MN[r] == acc_min : assign(r, (int)acc_min);
            if (SZ[r] == 1) {
                if (nunb == 1 && acc_min > first1) return assign(uvar, (int)first1);
                return true;
            }
            if (acc_max < last1 && !restrict_bounds(r, MN[r], acc_max)) return false;
            if (nunb == 1 && acc_min > MN[r]) return assign(uvar, (int)MN[r]);
            return true;
        } else {
            long first1 = MN[r], last1 = MX[r];
            for (size_t i = 0; i < c.ids.size(); i++) {
                int id = c.ids[i];
                if (id < 0) { long v = c.vals[i]; if (v > last1) return false; if (v > acc_max) acc_max = v; if (v < acc_min) acc_min = v; continue; }
                nunb++; uvar = id;
                if (MX[id] > last1 && !restrict_bounds(id, MN[id], last1)) return false;
                if (MN[id] < acc_min) acc_min = MN[id];
            }
            if (nunb == 0) return SZ[r] == 1 ? MN[r] == acc_max : assign(r, (int)acc_max);
            if (SZ[r] == 1) {
                if (nunb == 1 && acc_max < last1) return assign(uvar, (int)last1);
                return true;
            }
            if (acc_min > first1 && !restrict_bounds(r, acc_min, MX[r])) return false;
            if (nunb == 1 && acc_max < MX[r]) return assign(uvar, (int)MX[r]);
            return true;
        }
    }
    bool same_link(int x, int y) {         // X = Y: intersect and keep in step (AC on equality)
        if (x == y) return true;
        for (int k = 0; k < NW; k++) {
            W m = dom(x)[k] & dom(y)[k];
            if (m != dom(x)[k] && !setword(x, k, m)) return false;
            if (m != dom(y)[k] && !setword(y, k, m)) return false;
        }
        return true;
    }
    bool neq_link(int x, int y) {          // X != Y: FC on singletons
        if (x == y) return false;
        if (SZ[x] == 1 && SZ[y] > 1) return remove_val(y, MN[x]);
        if (SZ[y] == 1 && SZ[x] > 1) return remove_val(x, MN[y]);
        return true;
    }
    bool reif_prop(int s) {                // any change of B, X or Y
        const Reif &c = N.reifs[s];
        int b = c.b, x = c.x, y = c.y;
        if (x == y) {                      // B <=> X = X / X != X
            if (c.mode == 2) return SZ[b] == 1 ? MN[b] == 1 : assign(b, 1);
            if (c.mode == 3) return SZ[b] == 1 ? MN[b] == 0 : assign(b, 0);
        }
        if (SZ[b] == 1) {
            if (c.mode == 0) return MN[b] == 1 ? (SZ[x] == 1 ? MN[x] == y : assign(x, y)) : remove_val(x, y);
            if (c.mode == 1) return MN[b] == 1 ? remove_val(x, y) : (SZ[x] == 1 ? MN[x] == y : assign(x, y));
            if (c.mode == 2) return MN[b] == 1 ? same_link(x, y) : neq_link(x, y);
            if (c.mode == 3) return MN[b] == 1 ? neq_link(x, y) : same_link(x, y);
            if (c.mode == 4) {                 // ge: bounds
                if (MN[b] == 1) return restrict_bounds(x, max(MN[x], MN[y]), MX[x]) && restrict_bounds(y, MN[y], min(MX[y], MX[x]));
                if (MN[x] >= MX[y]) return false;
                return restrict_bounds(x, MN[x], MX[y] - 1) && restrict_bounds(y, MN[x] + 1, MX[y]);
            }
        }
        switch (c.mode) {
        case 0:                                // B <=> X = c
            if (!has(x, y)) return assign(b, 0);
            if (SZ[x] == 1) return assign(b, MN[x] == y ? 1 : 0);
            return true;
        case 1:                                // B <=> X != c
            if (!has(x, y)) return assign(b, 1);
            if (SZ[x] == 1) return assign(b, MN[x] != y ? 1 : 0);
            return true;
        case 2:                                // B <=> X = Y
            { W *dx = dom(x), *dy = dom(y); bool disj = true;
              for (int k = 0; k < NW && disj; k++) if (dx[k] & dy[k]) disj = false;
              if (disj) return assign(b, 0);
              if (SZ[x] == 1 && SZ[y] == 1) return assign(b, MN[x] == MN[y] ? 1 : 0);
              return true; }
        case 3:                                // B <=> X != Y
            { W *dx = dom(x), *dy = dom(y); bool disj = true;
              for (int k = 0; k < NW && disj; k++) if (dx[k] & dy[k]) disj = false;
              if (disj) return assign(b, 1);
              if (SZ[x] == 1 && SZ[y] == 1) return assign(b, MN[x] != MN[y] ? 1 : 0);
              return true; }
        default: return true;
        }
    }
    bool reif_bound(int s) {               // ge fires only on bound events (Picat: ins/bound)
        const Reif &c = N.reifs[s];
        if (c.mode != 4) return true;
        int b = c.b, x = c.x, y = c.y;
        if (SZ[b] == 1) return reif_prop(s);
        if (MN[x] >= MX[y]) return assign(b, 1);
        if (MX[x] < MN[y]) return assign(b, 0);
        return true;
    }
    bool ent_prop(int s) {                 // B => constraint
        const Entl &c = N.ents[s];
        int b = c.b, x = c.x, y = c.y;
        if (SZ[b] == 1 && MN[b] == 1) {
            if (c.mode == 0) return SZ[x] == 1 ? MN[x] == y : assign(x, y);
            if (c.mode == 1) return remove_val(x, y);
            if (c.mode == 2) return same_link(x, y);
            return neq_link(x, y);
        }
        switch (c.mode) {
        case 0: if (!has(x, y)) return assign(b, 0); return true;
        case 1: if (SZ[x] == 1 && MN[x] == y) return assign(b, 0); return true;
        case 2: { W *dx = dom(x), *dy = dom(y); bool disj = true;
                  for (int k = 0; k < NW && disj; k++) if (dx[k] & dy[k]) disj = false;
                  if (disj) return assign(b, 0);
                  return true; }
        default: if (SZ[x] == 1 && SZ[y] == 1 && MN[x] == MN[y]) return assign(b, 0);
                 return true;
        }
    }
    bool eld_prop(int s) {                 // I fixed -> V = tuple[I-1] (unification)
        const ElD &c = N.elds[s];
        if (SZ[c.i] != 1) return true;     // I not bound: the stock's var(A1) ins rule waits
        const vector<int> &t = N.tuples[c.t];
        long k = MN[c.i] - 1;
        static const bool tr2_ = getenv("FDN_TRACE") != NULL;
        if (tr2_) { fprintf(stderr, "  eld s=%d i=v%d(v=%ld) v=v%d tuple=%d tsize=%zu k=%ld t[k]=%d tup:", s, c.i, (long)MN[c.i], c.v, (int)c.t, t.size(), k, k >= 0 && k < (long)t.size() ? t[k] : -99); for (size_t q = 0; q < t.size(); q++) fprintf(stderr, " v%d:[%ld,%ld]", t[q], (long)MN[t[q]], (long)MX[t[q]]); fprintf(stderr, "\n"); }
        if (k < 0 || k >= (long)t.size()) return false;
        return same_link(c.v, t[k]);
    }
    bool eld_same(int s) {                 // keep the unification in step on any change
        const ElD &c = N.elds[s];
        if (SZ[c.i] != 1) return true;
        const vector<int> &t = N.tuples[c.t];
        long k = MN[c.i] - 1;
        if (k < 0 || k >= (long)t.size()) return false;
        return same_link(c.v, t[k]);
    }
    bool elf_ins(int s) {                  // V fixed -> I in [rlo, rhi]
        const ElF &f = N.elves[s];
        if (!f.rng || SZ[f.v] != 1) return true;
        long w = MN[f.v] - GLO;
        static const bool tr = getenv("FDN_TRACE") != NULL;
        if (tr) fprintf(stderr, "  elf_ins s=%d i=%d v=%d mnv=%d glo=%d w=%ld rlo=%d rhi=%d mni=%d mxi=%d\n", s, f.i, f.v, MN[f.v], GLO, w, w >= 0 && w < (long)f.rlo.size() ? f.rlo[w] : -99, w >= 0 && w < (long)f.rhi.size() ? f.rhi[w] : -99, MN[f.i], MX[f.i]);
        if (w < 0 || w >= (long)f.rlo.size() || f.rhi[w] < f.rlo[w]) return false;
        return restrict_bounds(f.i, f.rlo[w], f.rhi[w]);
    }
    bool elf_fc(int s, int own, int ev) {  // FC on a value removed from own
        const ElF &f = N.elves[s];
        const vector<int> &t = N.tuples[f.t];
        static const bool tr3_ = getenv("FDN_TRACE") != NULL;
        if (tr3_) { fprintf(stderr, "  elffc s=%d own=v%d ev=%d f.i=v%d[%ld,%ld] f.v=v%d[%ld,%ld] tsize=%zu tup:", s, own, ev, f.i, (long)MN[f.i], (long)MX[f.i], f.v, (long)MN[f.v], (long)MX[f.v], t.size()); for (size_t q = 0; q < t.size(); q++) fprintf(stderr, " v%d:[%ld,%ld]", t[q], (long)MN[t[q]], (long)MX[t[q]]); fprintf(stderr, "\n"); }
        if (own == f.v) {                                 // value removed -> indices without support out of I
            for (long k = MN[f.i]; k <= MX[f.i]; k++) {
                if (!has(f.i, (int)k)) continue;
                int tv = t[k - 1]; bool sup = false;
                if (SZ[tv] == 1) sup = has(f.v, MN[tv]);
                else for (long u = MN[tv]; u <= MX[tv] && !sup; u++) if (has(tv, (int)u) && has(f.v, (int)u)) sup = true;
                if (!sup && !remove_val(f.i, (int)k)) return false;
            }
        } else {                                          // index removed -> values without support out of V
            for (long v = MN[f.v]; v <= MX[f.v]; v++) {
                if (!has(f.v, (int)v)) continue;
                bool sup = false;
                for (long k = MN[f.i]; k <= MX[f.i] && !sup; k++) if (has(f.i, (int)k) && has(t[k - 1], (int)v)) sup = true;
                if (!sup && !remove_val(f.v, (int)v)) return false;
            }
        }
        return true;
    }
    bool lex_prop(int s, int evx) {        // watch pairs: bounds fail-check on a bound event
        const Lex &l = N.lexs[s];
        for (size_t i = 0; i + 1 < l.pr.size(); i++) {
            int x = l.pr[i].first, y = l.pr[i].second;
            if (SZ[x] == 1 && SZ[y] == 1) {
                if (MN[x] < MN[y]) return true;        // satisfied
                if (MN[x] > MN[y]) return false;
                continue;                              // equal: next pair
            }
            if (x != evx && y != evx) return true;     // the event was for a later pair
            if (MN[x] > MX[y]) return false;           // the watch check
            return true;
        }
        return lex_tail(s, (int)l.pr.size() - 1);      // all pairs fixed-equal: the arc pair
    }
    bool lex_tail(int s, int i) {          // the final pair with arc consistency
        const Lex &l = N.lexs[s];
        int x = l.pr[i].first, y = l.pr[i].second;
        if (SZ[x] == 1 && SZ[y] == 1) return l.le ? MN[x] <= MN[y] : MN[x] < MN[y];
        if (l.le) return restrict_bounds(x, MN[x], MX[y]) && restrict_bounds(y, MN[x], MX[y]);
        return restrict_bounds(x, MN[x], MX[y] - 1) && restrict_bounds(y, MN[x] + 1, MX[y]);
    }
    bool propagate1() {
        while (!EV.empty()) {
            Ev e = EV.back(); EV.pop_back();
            int x = e.x;
            static const bool dbg = getenv("FDN_TRACE") != NULL;
            if (e.k == E_BOUND) {
                int v = MN[x];
                for (const Edge &ed : N.adj[x]) if (!remove_val(ed.t, v + ed.d)) return false;
                for (const ChanR &r : N.vchan[x]) {
                    const vector<int> &t = N.tuples[r.tuple]; int i = v - r.off;
                    if (i >= 1 && i <= (int)t.size() && !assign(t[i - 1], r.a)) return false;
                }
                for (int s : N.vabs[x]) { if (dbg) fprintf(stderr, "abs_prop %d\n", s); if (!abs_prop(s)) { if (dbg) fprintf(stderr, "abs_prop %d FAIL\n", s); return false; } }
                for (int s : N.vmul[x]) { if (dbg) fprintf(stderr, "mul_prop %d\n", s); if (!mul_prop(s)) { if (dbg) fprintf(stderr, "mul_prop %d FAIL\n", s); return false; } }
                for (int s : N.vdiv[x]) { if (dbg) fprintf(stderr, "div_prop %d\n", s); if (!div_prop(s)) { if (dbg) fprintf(stderr, "div_prop %d FAIL\n", s); return false; } }
                for (int s : N.vmod[x]) { if (dbg) fprintf(stderr, "mod_prop %d\n", s); if (!mod_prop(s)) { if (dbg) fprintf(stderr, "mod_prop %d FAIL\n", s); return false; } }
                for (int s : N.vmm[x]) { if (dbg) fprintf(stderr, "mm_prop %d\n", s); if (!mm_prop(s)) { if (dbg) fprintf(stderr, "mm_prop %d FAIL\n", s); return false; } }
                for (int s : N.veld[x]) { if (dbg) fprintf(stderr, "eld_prop %d\n", s); if (!eld_prop(s)) { if (dbg) fprintf(stderr, "eld_prop %d FAIL\n", s); return false; } }
                for (int s : N.velf[x]) { if (dbg) fprintf(stderr, "elf_ins %d\n", s); if (!elf_ins(s)) { if (dbg) fprintf(stderr, "elf_ins %d FAIL\n", s); return false; } }
                for (int s : N.vlex[x]) { if (dbg) fprintf(stderr, "lex_prop %d\n", s); if (!lex_prop(s, x)) { if (dbg) fprintf(stderr, "lex_prop %d FAIL\n", s); return false; } }
                for (int s : N.vent[x]) { if (dbg) fprintf(stderr, "ent_prop %d\n", s); if (!ent_prop(s)) { if (dbg) fprintf(stderr, "ent_prop %d FAIL\n", s); return false; } }
                for (int s : N.vreif[x]) { if (dbg) fprintf(stderr, "reif_bound %d\n", s); if (!reif_bound(s)) { if (dbg) fprintf(stderr, "reif_bound %d FAIL\n", s); return false; } }
            } else if (e.k == E_LIN) {
                PENDL[x] = 0;
                if (!linear(x)) return false;
            } else if (e.k == E_BMAP) {         // X = Y+C / C-Y, bounds part ('$v_in_*_int')
                for (const Map &mp : N.vbmap[x]) {
                    long lo = mp.s > 0 ? MN[x] + mp.k : mp.k - MX[x], hi = mp.s > 0 ? MX[x] + mp.k : mp.k - MN[x];
                    if (!region(mp.t, lo, hi)) return false;
                }
            } else if (e.k == E_CHG) {
                PEND[x] = 0;
                if (SZ[x] > 1) for (int g : N.vhall[x]) if (!hall(g, x)) return false;
            } else if (e.k == E_REIF) {
                if (e.v < 0) {                     // batched change event
                    PENDR[x] = 0;
                    for (int s : N.vreif[x]) if (!reif_prop(s)) return false;
                    for (int s : N.veldc[x]) if (!eld_same(s)) return false;
                    for (int s : N.vmod[x]) if (!mod_prop(s)) return false;
                } else {                           // per-removed-value event
                    static const bool dbg2 = getenv("FDN_TRACE") != NULL;
                    if (dbg2) fprintf(stderr, "reif_val v%d ev=%d\n", x, e.v);
                    for (int s : N.vabs[x]) { if (dbg2) fprintf(stderr, "abs_dom %d\n", s); if (!abs_dom(s, x, e.v)) { if (dbg2) fprintf(stderr, "abs_dom %d FAIL\n", s); return false; } }
                    for (int s : N.velf[x]) { if (dbg2) fprintf(stderr, "elf_fc %d\n", s); if (!elf_fc(s, x, e.v)) { if (dbg2) fprintf(stderr, "elf_fc %d FAIL\n", s); return false; } }
                    for (int s : N.velfv[x]) { if (dbg2) fprintf(stderr, "elf_fcv %d\n", s); if (!elf_fc(s, x, e.v)) { if (dbg2) fprintf(stderr, "elf_fcv %d FAIL\n", s); return false; } }
                    for (int s : N.ventc[x]) { if (dbg2) fprintf(stderr, "entc %d\n", s); if (!ent_prop(s)) { if (dbg2) fprintf(stderr, "entc %d FAIL\n", s); return false; } }
                }
            } else {
                for (const Map &mp : N.vmap[x]) if (!remove_val(mp.t, mp.s * e.v + mp.k)) return false;
                for (const ChanR &r : N.vchan[x]) {
                    const vector<int> &t = N.tuples[r.tuple]; int i = e.v - r.off;
                    if (i >= 1 && i <= (int)t.size() && !remove_val(t[i - 1], r.a)) return false;
                }
            }
        }
        return true;
    }
    bool try_assign(int x, int v) {
        static const bool tr = getenv("FDN_TRACE") != NULL;
        if (tr) fprintf(stderr, "assign v%d=%d\n", x, v);
        if (assign(x, v) && propagate1()) return true;
        if (tr) fprintf(stderr, "  FAIL\n");
        for (auto &e : EV) { if (e.k == E_CHG) PEND[e.x] = 0; else if (e.k == E_LIN) PENDL[e.x] = 0; else if (e.k == E_REIF && e.v < 0) PENDR[e.x] = 0; }
        EV.clear(); return false;
    }
    int select_var() {
        int best = -1, n = N.label.size();
        while (ls < n && SZ[N.label[ls]] == 1) ls++;
        if (N.heur == 0) { for (int i = ls; i < n; i++) { int x = N.label[i]; if (SZ[x] > 1) return x; } return -1; }
        long bs = LONG_MAX;                       // strict improvement only: the first-seen wins ties
        for (int i = ls; i < n; i++) {
            int x = N.label[i];
            if (SZ[x] == 1) continue;
            switch (N.heur) {
            case 1: if (SZ[x] < bs) { bs = SZ[x]; best = x; if (bs == 2) return best; } break;          // ff
            case 2: if (MN[x] < bs || (MN[x] == bs && best >= 0 && SZ[x] < SZ[best])) { bs = MN[x]; best = x; } break;   // min
            case 3: if (MX[x] < bs || (MX[x] == bs && best >= 0 && SZ[x] < SZ[best])) { bs = MX[x]; best = x; } break;   // max
            case 4: if (SZ[x] < bs || (SZ[x] == bs && MN[x] < MN[best])) { bs = SZ[x]; best = x; } break;                // ff_min
            default: if (SZ[x] < bs || (SZ[x] == bs && MX[x] < MX[best])) { bs = SZ[x]; best = x; }                      // ff_max
            }
        }
        return best;
    }
    int next_in(const W *d, int v, int lim) {   // smallest value > v in snapshot d, <= lim
        for (int u = v + 1; u <= GHI && u <= lim; u++) {
            int b = u - GLO, k = b >> 6;
            W w = d[k] >> (b & 63);
            if (w) { u += __builtin_ctzll(w); return u <= lim && u <= GHI ? u : INT_MIN; }
            u = GLO + (k + 1) * 64 - 1;
        }
        return INT_MIN;
    }
    int prev_in(const W *d, int v, int lim) {   // largest value < v in snapshot d, >= lim
        for (int u = v - 1; u >= GLO && u >= lim; u--) {
            int b = u - GLO, k = b >> 6;
            if (b < 0) break;
            W w = u == GLO ? d[k] & 1 : d[k] & ((1ULL << ((b & 63) + 1)) - 1);
            if (w) { int t = 63 - __builtin_clzll(w); return GLO + k * 64 + t; }
            u = GLO + k * 64;
        }
        return INT_MIN;
    }
    void push_cp(int x, int v, int lim) {
        static const bool tr4_ = getenv("FDN_TRACE") != NULL;
        if (tr4_) fprintf(stderr, "push_cp v%d lim=%d\n", x, lim);
        size_t off = DS.size(); W *d = dom(x); DS.insert(DS.end(), d, d + NW);
        st.push_back({x, v, lim, ls, 0, 0, 0, 0, 0, TR.size(), off});
    }
    void push_cp_val(int x) {               // the choice point per the value strategy
        size_t off = DS.size(); W *d = dom(x); DS.insert(DS.end(), d, d + NW);
        int v = MN[x], lim = INT_MAX, kind = N.val, ph = 0;
        if (N.val == 1) { v = MX[x]; lim = GLO; }
        else if (N.val == 2) {
            long mid = fldiv((long)MN[x] + MX[x], 2);
            if (mid <= MN[x]) { v = next_in(d, MN[x], INT_MAX); lim = MN[x]; bt += 1; }        // stock counts 1 per narrow visit
            else { int p = prev_in(d, (int)mid, GLO); v = next_in(d, p, INT_MAX); lim = p; bt += 2; }   // Q first (Picat), then P; stock: 2 per visit
        } else if (N.val >= 3) { v = 0; lim = 0; }
        st.push_back({x, v, lim, ls, kind, 0, ph, MN[x], MX[x], TR.size(), off});
    }
    bool try_restrict(int x, long lo, long hi) {
        if (restrict_bounds(x, lo, hi) && propagate1()) return true;
        for (auto &e : EV) { if (e.k == E_CHG) PEND[e.x] = 0; else if (e.k == E_LIN) PENDL[e.x] = 0; else if (e.k == E_REIF && e.v < 0) PENDR[e.x] = 0; }
        EV.clear(); return false;
    }
    // set up the task: replay the prefix, then the root choice (x, (a, b])
    bool init(const vector<pair<int,int>> &pre, bool root, int rx, int ra, int rb, long credit) {
        prefix = pre; bt = credit;
        for (auto &d : pre) if (!try_assign(d.first, d.second)) return false;
        TR.clear();
        if (root) {
            int v = next_in(dom(rx), ra, rb);
            if (v == INT_MIN) return false;
            push_cp(rx, v, rb); EPOCH++; nodes++;
            if (!try_assign(rx, v)) state = 1;
        }
        return true;
    }
    template <class Poll> int run(Poll &&poll) {
        for (;;) {
            if ((nodes & 255) == 0 && poll(*this)) return R_PAUSE;
            if (state == 2) return R_DONE;
            if (state == 0) {
                int x = select_var();
                if (x < 0) { state = 1; if (counting) { nsol++; continue; } return R_SOL; }
                if (N.val == 0) {
                    push_cp(x, MN[x], INT_MAX); EPOCH++; nodes++;
                    if (!try_assign(x, MN[x])) state = 1;
                } else if (N.val >= 3) {           // split: branch on the two halves
                    push_cp_val(x); EPOCH++; nodes++;
                    CP &c = st.back();
                    long mid = fldiv((long)c.lo + c.hi, 2);
                    bool ok = c.kind == 3 ? try_restrict(x, c.lo, mid) : try_restrict(x, mid + 1, c.hi);
                    if (!ok) state = 1;
                } else {
                    push_cp_val(x); EPOCH++; nodes++;
                    if (!try_assign(x, st.back().v)) state = 1;
                }
                continue;
            }
            // backtrack
            for (;;) {
                if (st.empty()) { state = 2; break; }
                CP &c = st.back();
                while (TR.size() > c.tm) {
                    undo(TR.back()); TR.pop_back();
                }
                ls = c.ls;
                int v;
                if (c.kind >= 3) {                 // split: the second half, then done
                    if (c.ph != 0) { DS.resize(c.dom); st.pop_back(); continue; }
                    long mid = fldiv((long)c.lo + c.hi, 2);
                    bool ok = c.kind == 3 ? try_restrict(c.x, mid + 1, c.hi) : try_restrict(c.x, c.lo, mid);
                    c.ph = 1;
                    if (ok) { state = 0; break; }
                    continue;
                }
                if (c.kind == 2) {                 // updown zigzag
                    switch (c.ph) {
                    case 0: v = c.lim; c.ph = 2; break;                        // the first down try: P
                    case 2: v = next_in(&DS[c.dom], c.v, INT_MAX);
                            if (v == INT_MIN) { c.ph = 3; v = prev_in(&DS[c.dom], c.lim, GLO); if (v == INT_MIN) { DS.resize(c.dom); st.pop_back(); continue; } c.lim = v; }
                            else { c.v = v; c.ph = 1; }
                            break;
                    case 1: v = prev_in(&DS[c.dom], c.lim, GLO);
                            if (v == INT_MIN) { c.ph = 4; v = next_in(&DS[c.dom], c.v, INT_MAX); if (v == INT_MIN) { DS.resize(c.dom); st.pop_back(); continue; } c.v = v; }
                            else { c.lim = v; c.ph = 2; }
                            break;
                    case 3: v = prev_in(&DS[c.dom], c.lim, GLO); if (v == INT_MIN) { DS.resize(c.dom); st.pop_back(); continue; } c.lim = v; break;
                    default: v = next_in(&DS[c.dom], c.v, INT_MAX); if (v == INT_MIN) { DS.resize(c.dom); st.pop_back(); continue; } c.v = v; break;
                    }
                } else if (c.kind == 1) {
                    v = prev_in(&DS[c.dom], c.v, c.lim);
                    if (v == INT_MIN) { DS.resize(c.dom); st.pop_back(); continue; }
                    c.v = v;
                } else {
                    v = next_in(&DS[c.dom], c.v, c.lim);
                    if (v == INT_MIN) { DS.resize(c.dom); st.pop_back(); continue; }
                    c.v = v;
                }
                if (c.kind != 2) bt++;                 // the updown visits are counted at the push (stock's quirk)
                EPOCH++; nodes++;
                if (try_assign(c.x, v)) { state = 0; break; }
            }
        }
    }
    // donation level: the shallowest choice point with untried values. Only
    // the shallowest keeps the donor's remaining work contiguous (all of it
    // lies left of the donated range); donating a deeper level would leave
    // the donor untried values at shallower levels, i.e. to the right of the
    // donated range, and break the DFS order of the task list.
    int donation_level() {
        if (N.val != 0) return -1;             // the untried values are not a contiguous range
        for (size_t i = 0; i < st.size(); i++)
            if (next_in(&DS[st[i].dom], st[i].v, st[i].lim) != INT_MIN) return i;
        return -1;
    }
    // a new search for the untried values (v, lim] of choice point i, with
    // the state rolled back to that choice point (no prefix replay)
    Search *clone_at(int i, vector<pair<int,int>> &pre, int &ra) {
        CP &c = st[i];
        Search *s = new Search(N, false); s->counting = counting;
        s->D = D; s->SZ = SZ; s->MN = MN; s->MX = MX; s->SMIN = SMIN; s->SMAX = SMAX; s->NUNB = NUNB;
        for (size_t k = TR.size(); k > c.tm; k--) s->undo(TR[k - 1]);
        pre = prefix;
        for (int j = 0; j < i; j++) pre.push_back({st[j].x, st[j].v});
        s->prefix = pre; s->ls = c.ls; s->bt = 1;   // the retry into this range
        ra = c.v;
        int v = next_in(&DS[c.dom], c.v, c.lim);
        s->push_cp(c.x, v, c.lim); s->EPOCH++; s->nodes++;
        if (!s->try_assign(c.x, v)) s->state = 1;
        c.lim = c.v;
        return s;
    }
    void undo(const TE &t) {
        int omn = MN[t.x], omx = MX[t.x], osz = SZ[t.x];
        D[t.w] = t.old; SZ[t.x] = t.sz; MN[t.x] = t.mn; MX[t.x] = t.mx; STAMP[t.w] = 0;
        if (!N.vlinA[t.x].empty()) lin_delta(t.x, omn, omx, osz);
    }
    void solution(vector<int> &out) { out.clear(); for (int x : N.label) out.push_back(MN[x]); }
};

// ------------------------------------------------------------------ runs
struct Sol { vector<int> v; long bt_at; };
struct Task {
    vector<pair<int,int>> prefix; bool root = false; int rx = 0, ra = 0, rb = 0; long credit = 0;
    vector<int> key;                        // value path: lexicographic order = DFS order
    unique_ptr<Search> s; deque<Sol> sols; bool done = false, owned = false; long bt_total = 0;
    Task *next = nullptr;
};
struct KeyLess { bool operator()(const Task *a, const Task *b) const { return a->key < b->key; } };
struct fdn_run {
    fdn_net *N; Task *head; long head_bt = 0;  // tasks: linked list from head; a task is freed once consumed
    long live = 0, ntasks = 0;
    set<Task *, KeyLess> pending;           // runnable: not owned, not done
    bool in_next = false, stop = false; int active_owners = 0; long buffered = 0, nodes = 0, donations = 0;
    bool count = false; long csol = 0, cbt = 0;   // count mode (fdn_count): totals of finished tasks
    chrono::steady_clock::time_point last;
    string stats;
};

// never destroyed: detached pool threads may still wait on them at exit
static mutex &G = *new mutex;
static condition_variable &CV_WORK = *new condition_variable, &CV_CONS = *new condition_variable;
static vector<fdn_run *> &RUNS = *new vector<fdn_run *>;
static int POOL_N = 0; static bool POOL_UP = false;
static atomic<int> IDLE{0};
static const long SPAWN_NODES = getenv("FDN_SPAWN") ? atol(getenv("FDN_SPAWN")) : 20000;
// solution buffers (in label values): per task ahead of the consumer, and per run
static const long TASK_BUF = 1L << 18, RUN_BUF = 1L << 26; static const int GRACE_MS = 50;

static int nthreads() {
    const char *e = getenv("FDN_THREADS");
    if (e) { int n = atoi(e); return n < 1 ? 1 : n; }   // explicit: exactly as specified, no cap
    int hc = (int)thread::hardware_concurrency();       // automatic: capped at 64
    return hc < 1 ? 1 : (hc > 64 ? 64 : hc);            // (over-subscription on cgroup-limited boxes)
}
static bool over_buf(fdn_run *r, Task *t) {   // should a task ahead of the consumer pause?
    long n = r->N->label.size();
    return t != r->head && ((long)t->sols.size() * n >= TASK_BUF || r->buffered * n >= RUN_BUF);
}
static bool active(fdn_run *r) {
    return r->in_next || chrono::steady_clock::now() - r->last < chrono::milliseconds(GRACE_MS);
}
static void worker_main();
static void start_pool() {            // with G held
    if (POOL_UP) return;
    POOL_UP = true; POOL_N = nthreads() - 1;
    for (int i = 0; i < POOL_N; i++) thread(worker_main).detach();
}

// run task t of r on the calling thread until a solution / done / pause.
// Called without G; t->owned is set by the caller.
static int run_task(fdn_run *r, Task *t, bool consumer) {
    if (!t->s) {
        t->s.reset(new Search(*r->N)); t->s->counting = r->count;
        if (!t->s->init(t->prefix, t->root, t->rx, t->ra, t->rb, t->credit)) {
            // cannot happen for a donated task; the root task fails here if
            // the start state is inconsistent
            t->s->state = 2;
        }
    }
    Search &s = *t->s;
    long n0 = s.nodes;
    auto poll = [&](Search &se) {
        bool want;
        {
            unique_lock<mutex> lk(G);
            r->nodes += se.nodes - n0; n0 = se.nodes;
            if (r->stop) return true;
            if (!consumer) {
                if (!active(r)) return true;
                if (over_buf(r, t)) return true;
            } else if (!POOL_UP && r->nodes > SPAWN_NODES && nthreads() > 1) start_pool();
            want = IDLE.load() > 0 && r->pending.empty();
        }
        if (!want) return false;
        { lock_guard<mutex> lk(G); if ((long)r->pending.size() >= 2L * nthreads()) return false; }   // pending tasks hold a state copy
        int lev = se.donation_level();
        if (lev < 0) return false;
        unique_ptr<Task> u(new Task);
        u->s.reset(se.clone_at(lev, u->prefix, u->ra));
        u->root = true;
        for (auto &d : u->prefix) u->key.push_back(d.second);
        u->key.push_back(u->ra + 1);
        unique_lock<mutex> lk(G);
        u->next = t->next; t->next = u.get();
        r->pending.insert(u.get()); u.release(); r->live++; r->ntasks++; r->donations++;
        CV_WORK.notify_one();
        return false;
    };
    for (;;) {
        int res = s.run(poll);
        unique_lock<mutex> lk(G);
        r->nodes += s.nodes - n0; n0 = s.nodes;
        if (res == R_SOL) {
            Sol so; s.solution(so.v); so.bt_at = s.bt;
            t->sols.push_back(move(so)); r->buffered++;
            CV_CONS.notify_all();
            if (consumer) return res;
            if (r->stop || !active(r) || over_buf(r, t)) return R_PAUSE;
            continue;
        }
        if (res == R_DONE) {
            t->done = true; t->bt_total = s.bt;
            if (r->count) { r->csol += s.nsol; r->cbt += s.bt; }
            t->s.reset();
            vector<pair<int,int>>().swap(t->prefix); vector<int>().swap(t->key);
            CV_CONS.notify_all();
        }
        return res;
    }
}

static void claim(fdn_run *r, Task *t) { t->owned = true; r->active_owners++; r->pending.erase(t); }
static void release(fdn_run *r, Task *t) {
    t->owned = false; r->active_owners--;
    if (!t->done) r->pending.insert(t);
    else if (r->count) { delete t; r->live--; }   // count mode: tasks are not linked
    CV_CONS.notify_all();
}
static void worker_main() {
    unique_lock<mutex> lk(G);
    for (;;) {
        fdn_run *r = nullptr; Task *t = nullptr;
        for (auto it = RUNS.rbegin(); it != RUNS.rend() && !t; ++it) {
            fdn_run *q = *it;
            if (q->stop || !active(q) || q->pending.empty()) continue;
            Task *u = *q->pending.begin();
            if (!over_buf(q, u)) { r = q; t = u; }
        }
        if (!t) { IDLE++; CV_WORK.wait_for(lk, chrono::milliseconds(GRACE_MS)); IDLE--; continue; }
        claim(r, t);
        lk.unlock();
        run_task(r, t, false);
        lk.lock();
        release(r, t);
    }
}

extern "C" {
static fdn_run *start(fdn_net *n, bool count) {
    if (getenv("FDN_DUMP")) {
        fprintf(stderr, "net: nv=%d glo=%d ghi=%d muls=%zu lins=%zu\n", n->nv, n->glo, n->ghi, n->muls.size(), n->lins.size());
        for (size_t i = 0; i < n->muls.size(); i++) fprintf(stderr, "  mul %zu: %d*%d=%d\n", i, n->muls[i].x, n->muls[i].y, n->muls[i].z);
        for (size_t i = 0; i < n->lins.size(); i++) {
            fprintf(stderr, "  lin %zu: op=%d c=%ld:", i, n->lins[i].op, n->lins[i].c);
            for (size_t j = 0; j < n->lins[i].a.size(); j++) fprintf(stderr, " %ld*v%d", n->lins[i].a[j], n->lins[i].xs[j]);
            fprintf(stderr, "\n");
        }
    }
    n->finalize();
    fdn_run *r = new fdn_run; r->N = n; r->count = count;   // set before pool threads can see the run
    Task *t = new Task; r->pending.insert(t); r->live = r->ntasks = 1;
    r->head = count ? nullptr : t;   // count mode: tasks are not linked, freed when done
    r->last = chrono::steady_clock::now();
    lock_guard<mutex> lk(G); RUNS.push_back(r);
    return r;
}
fdn_run *fdn_start(fdn_net *n) { return start(n, false); }
int fdn_nthreads(void) { return nthreads(); }
fdn_run *fdn_start_count(fdn_net *n) { return start(n, true); }
int fdn_nlabel(fdn_run *r) { return r->N->label.size(); }
const int *fdn_label_ids(fdn_run *r) { return r->N->label.data(); }
int fdn_next(fdn_run *r, int *vals, long *bt) {
    unique_lock<mutex> lk(G);
    r->in_next = true; *bt = 0; int res = 0;
    for (;;) {
        Task *t = r->head;
        if (!t) break;
        if (!t->sols.empty()) {
            Sol &so = t->sols.front();
            *bt += so.bt_at - r->head_bt; r->head_bt = so.bt_at;
            copy(so.v.begin(), so.v.end(), vals);
            t->sols.pop_front(); r->buffered--; res = 1; break;
        }
        if (t->done && !t->owned) {
            *bt += t->bt_total - r->head_bt; r->head_bt = 0; r->head = t->next;
            r->buffered -= t->sols.size(); delete t; r->live--;
            continue;
        }
        if (!t->owned) {
            claim(r, t);
            lk.unlock(); run_task(r, t, true); lk.lock();
            release(r, t);
            continue;
        }
        CV_CONS.wait(lk);
    }
    r->in_next = false; r->last = chrono::steady_clock::now();
    return res;
}
const char *fdn_stats(fdn_run *r) {
    lock_guard<mutex> lk(G);
    char b[200]; snprintf(b, sizeof b, "nodes=%ld tasks=%ld donations=%ld threads=%d", r->nodes, r->ntasks, r->donations, POOL_UP ? POOL_N + 1 : 1);
    r->stats = b; return r->stats.c_str();
}
// count mode: the consumer works like a pool thread, on any pending task
// (leftmost first), until no task is left. No order and no solution buffers.
long fdn_count(fdn_run *r, long *bt) {   // r from fdn_start_count
    unique_lock<mutex> lk(G);
    r->in_next = true;
    for (;;) {
        if (!r->pending.empty()) {
            Task *t = *r->pending.begin();
            claim(r, t);
            lk.unlock(); run_task(r, t, true); lk.lock();
            release(r, t);
            continue;
        }
        if (r->live == 0) break;
        CV_CONS.wait(lk);
    }
    r->in_next = false; r->last = chrono::steady_clock::now();
    *bt = r->cbt;
    return r->csol;
}
void fdn_free(fdn_run *r) {
    unique_lock<mutex> lk(G);
    r->stop = true;
    while (r->active_owners > 0) CV_CONS.wait(lk);
    RUNS.erase(find(RUNS.begin(), RUNS.end(), r));
    lk.unlock();
    for (Task *t = r->head; t;) { Task *n = t->next; delete t; t = n; }
    delete r->N; delete r;
}
}
