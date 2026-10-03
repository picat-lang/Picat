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
struct Lin { int op; long c; vector<long> a; vector<int> xs; long maxrange = 0; };   // op 0: = 0, 1: >= 0, 2: = 0 with AC at <= 2 unbound
struct Map { int t, s, k; };   // value w removed from owner -> remove s*w+k from t

struct fdn_net {
    int glo, ghi, nw = 0, nv = 0, heur = 0;
    vector<vector<int>> vals;
    vector<vector<Edge>> adj;
    vector<vector<int>> halls, vhall;
    map<vector<int>, int> hallkey;
    vector<vector<int>> tuples;
    map<vector<int>, int> tupkey;
    vector<vector<ChanR>> vchan;
    vector<vector<Map>> vmap;
    vector<char> chx;
    vector<Lin> lins;
    vector<vector<int>> vlin;
    vector<vector<pair<int,long>>> vlinA;  // var -> (constraint, total coefficient)
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
    n->vchan.emplace_back(); n->vlin.emplace_back(); n->vmap.emplace_back();
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
void fdn_linear(fdn_net *n, int op, long c, int k, const long *a, const int *xs) {
    Lin l{op, c, vector<long>(a, a + k), vector<int>(xs, xs + k)};
    int id = n->lins.size(); n->lins.push_back(l);
    vector<int> m(xs, xs + k); sort(m.begin(), m.end()); m.erase(unique(m.begin(), m.end()), m.end());
    for (int x : m) n->vlin[x].push_back(id);
}
void fdn_label(fdn_net *n, int h, int k, const int *xs) { n->heur = h; n->label.assign(xs, xs + k); }
void fdn_net_free(fdn_net *n) { delete n; }
}

// ------------------------------------------------------------------ search
enum { E_BOUND, E_CHG, E_CHX, E_LIN };
struct Ev { int k, x, v; };
struct TE { size_t w; W old; int x, sz, mn, mx; };
enum { R_SOL, R_DONE, R_PAUSE };

struct Task;
struct Search {
    const fdn_net &N; const int NW, GLO, GHI;
    vector<W> D; vector<int> SZ, MN, MX;
    vector<TE> TR; vector<uint64_t> STAMP; uint64_t EPOCH = 1;
    vector<Ev> EV; vector<char> PEND, PENDL;
    vector<long> SMIN, SMAX; vector<int> NUNB;   // per linear constraint, incremental
    struct CP { int x, v, lim, ls; size_t tm, dom; };
    int ls = 0;                             // label list: all entries before ls are bound
    vector<CP> st; vector<W> DS;            // DS: domain snapshots of the CP variables
    long bt = 0, nodes = 0;
    int state = 0;                          // 0 select, 1 backtrack, 2 done
    vector<pair<int,int>> prefix;

    Search(const fdn_net &n, bool init_sums = true) : N(n), NW(n.nw), GLO(n.glo), GHI(n.ghi), D(n.D0), SZ(n.SZ0), MN(n.MN0), MX(n.MX0),
        STAMP(n.D0.size(), 0), PEND(n.nv, 0), PENDL(n.lins.size(), 0),
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
        if (N.chg[x] && !PEND[x]) { PEND[x] = 1; EV.push_back({E_CHG, x, 0}); }
        for (int s : N.vlin[x]) if (!PENDL[s]) { PENDL[s] = 1; EV.push_back({E_LIN, s, 0}); }
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
    bool linear(int s) {
        const Lin &c = N.lins[s];
        if (c.op == 2 && NUNB[s] <= 2) return ac2(c);
        long smin = SMIN[s], smax = SMAX[s];
        if ((c.op != 1 && smin > 0) || smax < 0) return false;
        if ((c.op == 1 ? smax : min(smax, -smin)) >= c.maxrange) return true;   // nothing can be pruned
        for (size_t i = 0; i < c.xs.size(); i++) {
            int y = c.xs[i]; long a = c.a[i];
            if (SZ[y] == 1) continue;
            long lo_i = a > 0 ? a * MN[y] : a * MX[y], hi_i = a > 0 ? a * MX[y] : a * MN[y];
            long tlo = -(smax - hi_i), thi = c.op != 1 ? -(smin - lo_i) : LONG_MAX / 4;
            long xlo, xhi;
            if (a > 0) { xlo = cdiv(tlo, a); xhi = c.op != 1 ? fdiv(thi, a) : LONG_MAX / 4; }
            else { xlo = c.op != 1 ? cdiv(thi, a) : LONG_MIN / 4; xhi = fdiv(tlo, a); }
            if (xlo > MN[y] || xhi < MX[y]) {
                if (!restrict_bounds(y, max<long>(xlo, MN[y]), min<long>(xhi, MX[y]))) return false;
                smin = SMIN[s]; smax = SMAX[s];   // maintained by setword
            }
        }
        return true;
    }
    bool propagate1() {
        while (!EV.empty()) {
            Ev e = EV.back(); EV.pop_back();
            int x = e.x;
            if (e.k == E_BOUND) {
                int v = MN[x];
                for (const Edge &ed : N.adj[x]) if (!remove_val(ed.t, v + ed.d)) return false;
                for (const ChanR &r : N.vchan[x]) {
                    const vector<int> &t = N.tuples[r.tuple]; int i = v - r.off;
                    if (i >= 1 && i <= (int)t.size() && !assign(t[i - 1], r.a)) return false;
                }
            } else if (e.k == E_LIN) {
                PENDL[x] = 0;
                if (!linear(x)) return false;
            } else if (e.k == E_CHG) {
                PEND[x] = 0;
                if (SZ[x] > 1) for (int g : N.vhall[x]) if (!hall(g, x)) return false;
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
        if (assign(x, v) && propagate1()) return true;
        for (auto &e : EV) { if (e.k == E_CHG) PEND[e.x] = 0; else if (e.k == E_LIN) PENDL[e.x] = 0; }
        EV.clear(); return false;
    }
    int select_var() {
        int best = -1, bs = INT_MAX, n = N.label.size();
        while (ls < n && SZ[N.label[ls]] == 1) ls++;
        for (int i = ls; i < n; i++) {
            int x = N.label[i];
            if (SZ[x] == 1) continue;
            if (N.heur == 0) return x;
            if (SZ[x] < bs) { bs = SZ[x]; best = x; if (bs == 2) break; }
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
    void push_cp(int x, int v, int lim) {
        size_t off = DS.size(); W *d = dom(x); DS.insert(DS.end(), d, d + NW);
        st.push_back({x, v, lim, ls, TR.size(), off});
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
                if (x < 0) { state = 1; return R_SOL; }
                push_cp(x, MN[x], INT_MAX); EPOCH++; nodes++;
                if (!try_assign(x, MN[x])) state = 1;
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
                int v = next_in(&DS[c.dom], c.v, c.lim);
                if (v == INT_MIN) { DS.resize(c.dom); st.pop_back(); continue; }
                bt++; c.v = v; EPOCH++; nodes++;
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
        for (size_t i = 0; i < st.size(); i++)
            if (next_in(&DS[st[i].dom], st[i].v, st[i].lim) != INT_MIN) return i;
        return -1;
    }
    // a new search for the untried values (v, lim] of choice point i, with
    // the state rolled back to that choice point (no prefix replay)
    Search *clone_at(int i, vector<pair<int,int>> &pre, int &ra) {
        CP &c = st[i];
        Search *s = new Search(N, false);
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
    int n = e ? atoi(e) : (int)thread::hardware_concurrency();
    return n < 1 ? 1 : n;
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
        t->s.reset(new Search(*r->N));
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
            t->done = true; t->bt_total = s.bt; t->s.reset();
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
fdn_run *fdn_start(fdn_net *n) {
    n->finalize();
    fdn_run *r = new fdn_run; r->N = n;
    r->head = new Task; r->pending.insert(r->head); r->live = r->ntasks = 1;
    r->last = chrono::steady_clock::now();
    lock_guard<mutex> lk(G); RUNS.push_back(r);
    return r;
}
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
