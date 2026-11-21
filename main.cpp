// mechfight/1 bot in C++17.  Survival-first: stay in the zone, use cover, kite, dodge, shoot with lead.
#include <bits/stdc++.h>
#include <poll.h>
#include <unistd.h>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

static const double PI = 3.14159265358979323846;
static bool GHOST = false;  // camp mode (hurt): silent, hidden, snipes unseen targets only
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline double wrap(double a) {
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}
static inline double fin(double v) { return std::isfinite(v) ? v : 0.0; }

struct V3 {
    double x = 0, y = 0, z = 0;
};
static V3 getV3(const json& a) {
    V3 r;
    if (a.is_array() && a.size() >= 2) {
        r.x = a[0].get<double>();
        r.y = a[1].get<double>();
        if (a.size() >= 3) r.z = a[2].get<double>();
    }
    return r;
}
static inline double hdist(const V3& a, const V3& b) { return hypot(a.x - b.x, a.y - b.y); }
static inline double num(const json& j, const char* k, double d) {
    if (j.is_object()) {
        auto it = j.find(k);
        if (it != j.end() && it->is_number()) return it->get<double>();
    }
    return d;
}

// ------------------------------------------------------------------------------------ map
struct Prism {
    int kind = 0;  // 0 building/stadium, 1 bridge
    double base = 0, top = 0;
    vector<double> ring;
    vector<vector<double>> holes;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
};

static bool inRing(const vector<double>& r, double x, double y) {
    bool in = false;
    size_t n = r.size() / 2;
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = r[2 * i], yi = r[2 * i + 1], xj = r[2 * j], yj = r[2 * j + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) in = !in;
    }
    return in;
}

struct GameMap {
    double OX = -513, OY = -334, tstep = 1;
    int tnx = 1070, tny = 724;
    vector<int16_t> terr;
    vector<Prism> prisms;
    double bx0 = -512, bx1 = 555, by0 = -333, by1 = 388;
    bool loaded = false;

    // navigation grid
    static constexpr double CS = 3.0;
    int W = 0, H = 0;
    vector<uint8_t> blocked;
    vector<float> cost;
    static constexpr float INF = 1e9f;
    double flowGX = 1e9, flowGY = 1e9;

    // line-of-sight grid (height aware)
    static constexpr double LS = 2.0;
    int LW = 0, LH = 0;
    vector<float> lbase, ltop;
    vector<float> openness;

    double ground(double x, double y) const {
        if (terr.empty()) return 0;
        double fx = (x - OX) / tstep, fy = (y - OY) / tstep;
        int i = (int)floor(fx), j = (int)floor(fy);
        i = max(0, min(tnx - 2, i));
        j = max(0, min(tny - 2, j));
        double tx = clampd(fx - i, 0, 1), ty = clampd(fy - j, 0, 1);
        auto h = [&](int a, int b) { return terr[(size_t)b * tnx + a] * 0.01; };
        double h00 = h(i, j), h10 = h(i + 1, j), h01 = h(i, j + 1), h11 = h(i + 1, j + 1);
        return (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
    }

    bool load(const string& dir) {
        try {
            {
                ifstream f(dir + "/v2/terrain.i16", ios::binary);
                if (!f) return false;
                f.seekg(0, ios::end);
                size_t sz = f.tellg();
                f.seekg(0);
                terr.resize(sz / 2);
                f.read((char*)terr.data(), terr.size() * 2);
            }
            ifstream wf(dir + "/v2/world.json");
            if (!wf) return false;
            json w = json::parse(wf);
            const json& T = w["terrain"];
            OX = num(T, "minX", -513);
            OY = num(T, "minY", -334);
            tstep = num(T, "step", 1);
            tnx = (int)num(T, "nx", 1070);
            tny = (int)num(T, "ny", 724);
            if ((size_t)tnx * tny > terr.size()) return false;
            const json& B = w["bounds"];
            bx0 = num(B, "minX", -512);
            bx1 = num(B, "maxX", 555);
            by0 = num(B, "minY", -333);
            by1 = num(B, "maxY", 388);
            for (auto& p : w["prisms"]) {
                Prism pr;
                string k = p.value("kind", "building");
                pr.kind = (k == "bridge") ? 1 : 0;
                pr.base = p["base"].get<double>();
                pr.top = p["top"].get<double>();
                for (auto& v : p["ring"]) pr.ring.push_back(v.get<double>());
                if (p.contains("holes"))
                    for (auto& h : p["holes"]) {
                        vector<double> hv;
                        for (auto& v : h) hv.push_back(v.get<double>());
                        pr.holes.push_back(hv);
                    }
                for (size_t i = 0; i + 1 < pr.ring.size(); i += 2) {
                    pr.x0 = min(pr.x0, pr.ring[i]);
                    pr.x1 = max(pr.x1, pr.ring[i]);
                    pr.y0 = min(pr.y0, pr.ring[i + 1]);
                    pr.y1 = max(pr.y1, pr.ring[i + 1]);
                }
                prisms.push_back(std::move(pr));
            }
            buildGrids();
            buildOpenness();
            loaded = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool inPrism(const Prism& p, double x, double y) const {
        if (x < p.x0 || x > p.x1 || y < p.y0 || y > p.y1) return false;
        if (!inRing(p.ring, x, y)) return false;
        for (auto& h : p.holes)
            if (inRing(h, x, y)) return false;
        return true;
    }

    // does solid geometry block a mech body standing on the ground at (x,y)?
    bool solidAt(double x, double y) const {
        double g = ground(x, y);
        for (auto& p : prisms) {
            if (!(p.base < g + 9.0 && p.top > g + 1.2)) continue;
            if (inPrism(p, x, y)) return true;
        }
        return false;
    }

    void buildGrids() {
        W = (int)ceil((tnx * tstep) / CS);
        H = (int)ceil((tny * tstep) / CS);
        blocked.assign((size_t)W * H, 0);
        const double infl = 2.9;
        static const double ox[9] = {0, 1, -1, 0, 0, 0.7071, -0.7071, 0.7071, -0.7071};
        static const double oy[9] = {0, 0, 0, 1, -1, 0.7071, 0.7071, -0.7071, -0.7071};
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++) {
                double cx = OX + (i + 0.5) * CS, cy = OY + (j + 0.5) * CS;
                bool b = false;
                if (cx < bx0 + 4 || cx > bx1 - 4 || cy < by0 + 4 || cy > by1 - 4) b = true;
                if (!b) {
                    for (int k = 0; k < 9 && !b; k++)
                        if (solidAt(cx + ox[k] * infl, cy + oy[k] * infl)) b = true;
                }
                if (!b) {
                    double gx = (ground(cx + 1.5, cy) - ground(cx - 1.5, cy)) / 3.0;
                    double gy = (ground(cx, cy + 1.5) - ground(cx, cy - 1.5)) / 3.0;
                    if (hypot(gx, gy) > 0.95) b = true;
                }
                blocked[(size_t)j * W + i] = b;
            }
        LW = (int)ceil((tnx * tstep) / LS);
        LH = (int)ceil((tny * tstep) / LS);
        lbase.assign((size_t)LW * LH, 1e9f);
        ltop.assign((size_t)LW * LH, -1e9f);
        for (auto& p : prisms) {
            int i0 = max(0, (int)floor((p.x0 - OX) / LS)), i1 = min(LW - 1, (int)floor((p.x1 - OX) / LS));
            int j0 = max(0, (int)floor((p.y0 - OY) / LS)), j1 = min(LH - 1, (int)floor((p.y1 - OY) / LS));
            for (int j = j0; j <= j1; j++)
                for (int i = i0; i <= i1; i++) {
                    double cx = OX + (i + 0.5) * LS, cy = OY + (j + 0.5) * LS;
                    if (inPrism(p, cx, cy)) {
                        size_t id = (size_t)j * LW + i;
                        lbase[id] = min(lbase[id], (float)p.base);
                        ltop[id] = max(ltop[id], (float)p.top);
                    }
                }
        }
    }

    void buildOpenness() {
        openness.assign((size_t)W * H, 0.f);
        const int ND = 12;
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++) {
                size_t id0 = (size_t)j * W + i;
                if (blocked[id0]) continue;
                double cx = OX + (i + 0.5) * CS, cy = OY + (j + 0.5) * CS;
                double sum = 0;
                for (int k = 0; k < ND; k++) {
                    double a = 2 * PI * k / ND, ca = cos(a), sa = sin(a), d = 4;
                    for (; d <= 150; d += 3) {
                        double x = cx + ca * d, y = cy + sa * d;
                        int li = (int)floor((x - OX) / LS), lj = (int)floor((y - OY) / LS);
                        if (li < 0 || lj < 0 || li >= LW || lj >= LH) break;
                        size_t id = (size_t)lj * LW + li;
                        if (ltop[id] > -1e8f && ltop[id] - lbase[id] > 6) break;
                    }
                    sum += min(d, 150.0);
                }
                openness[id0] = (float)(sum / ND);
            }
    }

    bool navBlockedXY(double x, double y) const {
        int i = (int)floor((x - OX) / CS), j = (int)floor((y - OY) / CS);
        if (i < 0 || j < 0 || i >= W || j >= H) return true;
        return blocked[(size_t)j * W + i] != 0;
    }

    // line of sight between two 3D points (eye to eye style); true if clear
    bool los(const V3& a, const V3& b) const {
        if (!loaded) return true;
        double d = hdist(a, b);
        int n = max(1, (int)(d / 1.5));
        for (int s = 1; s < n; s++) {
            double f = (double)s / n;
            double x = a.x + (b.x - a.x) * f, y = a.y + (b.y - a.y) * f, z = a.z + (b.z - a.z) * f;
            int i = (int)floor((x - OX) / LS), j = (int)floor((y - OY) / LS);
            if (i < 0 || j < 0 || i >= LW || j >= LH) continue;
            size_t id = (size_t)j * LW + i;
            if (ltop[id] > -1e8f && z >= lbase[id] - 0.1 && z <= ltop[id] + 0.1) return false;
        }
        return true;
    }

    // Dijkstra distance field (meters) from goal over walkable cells
    void flow(double gx, double gy) {
        if (!loaded) return;
        flowGX = gx;
        flowGY = gy;
        cost.assign((size_t)W * H, INF);
        int gi = (int)floor((gx - OX) / CS), gj = (int)floor((gy - OY) / CS);
        gi = max(0, min(W - 1, gi));
        gj = max(0, min(H - 1, gj));
        if (blocked[(size_t)gj * W + gi]) {
            bool found = false;
            for (int r = 1; r < 25 && !found; r++)
                for (int dj = -r; dj <= r && !found; dj++)
                    for (int di = -r; di <= r && !found; di++) {
                        int a = gi + di, b = gj + dj;
                        if (a < 0 || b < 0 || a >= W || b >= H) continue;
                        if (!blocked[(size_t)b * W + a]) {
                            gi = a;
                            gj = b;
                            found = true;
                        }
                    }
            if (!found) return;
        }
        typedef pair<float, int> P;
        priority_queue<P, vector<P>, greater<P>> pq;
        cost[(size_t)gj * W + gi] = 0;
        pq.push({0, gj * W + gi});
        static const int di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
        static const int dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
        while (!pq.empty()) {
            auto [c, id] = pq.top();
            pq.pop();
            if (c > cost[id]) continue;
            int i = id % W, j = id / W;
            for (int k = 0; k < 8; k++) {
                int a = i + di[k], b = j + dj[k];
                if (a < 0 || b < 0 || a >= W || b >= H) continue;
                int nid = b * W + a;
                if (blocked[nid]) continue;
                if (k >= 4 && (blocked[j * W + a] || blocked[b * W + i])) continue;
                float nc = c + (k < 4 ? (float)CS : (float)(CS * 1.41421356));
                if (nc < cost[nid]) {
                    cost[nid] = nc;
                    pq.push({nc, nid});
                }
            }
        }
    }

    // cost-to-go at a world position (INF if unknown)
    float costAt(double x, double y) const {
        if (cost.empty()) return INF;
        int i = (int)floor((x - OX) / CS), j = (int)floor((y - OY) / CS);
        if (i < 0 || j < 0 || i >= W || j >= H) return INF;
        return cost[(size_t)j * W + i];
    }

    // direction of steepest descent a few cells ahead; false if no information
    bool flowDir(double px, double py, double& dx, double& dy) const {
        if (cost.empty()) return false;
        int i = (int)floor((px - OX) / CS), j = (int)floor((py - OY) / CS);
        if (i < 0 || j < 0 || i >= W || j >= H) return false;
        static const int di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
        static const int dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
        float c = cost[(size_t)j * W + i];
        if (c >= INF / 2) {
            // standing in a blocked/unreached cell: find best nearby reached cell
            float best = INF;
            int bi = -1, bj = -1;
            for (int r = 1; r <= 4; r++) {
                for (int b = j - r; b <= j + r; b++)
                    for (int a = i - r; a <= i + r; a++) {
                        if (a < 0 || b < 0 || a >= W || b >= H) continue;
                        float cc = cost[(size_t)b * W + a];
                        if (cc < best) {
                            best = cc;
                            bi = a;
                            bj = b;
                        }
                    }
                if (bi >= 0) break;
            }
            if (bi < 0) return false;
            dx = OX + (bi + 0.5) * CS - px;
            dy = OY + (bj + 0.5) * CS - py;
            double l = hypot(dx, dy);
            if (l < 1e-6) return false;
            dx /= l;
            dy /= l;
            return true;
        }
        int ci = i, cj = j;
        for (int step = 0; step < 4; step++) {
            float bc = cost[(size_t)cj * W + ci];
            int bi = ci, bj = cj;
            for (int k = 0; k < 8; k++) {
                int a = ci + di[k], b = cj + dj[k];
                if (a < 0 || b < 0 || a >= W || b >= H) continue;
                if (k >= 4 && (blocked[cj * W + a] || blocked[b * W + ci])) continue;
                float cc = cost[(size_t)b * W + a];
                if (cc < bc) {
                    bc = cc;
                    bi = a;
                    bj = b;
                }
            }
            if (bi == ci && bj == cj) break;
            ci = bi;
            cj = bj;
        }
        double tx, ty;
        if (cost[(size_t)cj * W + ci] <= 0.01f && hypot(flowGX - px, flowGY - py) < 9) {
            tx = flowGX;
            ty = flowGY;
        } else {
            tx = OX + (ci + 0.5) * CS;
            ty = OY + (cj + 0.5) * CS;
        }
        dx = tx - px;
        dy = ty - py;
        double l = hypot(dx, dy);
        if (l < 1e-6) return false;
        dx /= l;
        dy /= l;
        return true;
    }
};

// ------------------------------------------------------------------------------------ bot
struct Enemy {
    int id = 0;
    V3 pos, vel;
    double aimYaw = 0, aimPitch = 0;
    double hp = 1400, maxhp = 1400;
    int cubes = 0;
    bool charging = false;
    double dist = 0;
    bool hasLaser = false;
};

static int weaponRank(const string& k) {
    if (k == "lightning") return 3;
    if (k == "laser") return 2;
    if (k == "mortar") return 1;
    return 0;
}

struct Bot {
    GameMap map;
    json cfg;
    uint64_t rng = 88172645463325252ULL;
    double gunSpeed = 252, mortarSpeed = 72, mortarG = 22;
    double laserCharge = 0.75;
    int lastTick = -1;
    double strafeSign = 1;
    int nextFlip = 0;
    V3 prevMove;
    double underFireUntil = -1;
    double lastHitBearing = 0;
    bool boostLatch = false;
    set<int> badPickups;
    double flowRefreshT = -100;
    double scanPhase = 0;
    int takeTries = 0;
    int lastTakeId = -1;
    V3 lastEnemyPos;
    double lastEnemyT = -100;
    bool camp = false;
    bool haveHide = false;
    double hideX = 0, hideY = 0, hideR = -1;
    V3 wp;
    double wpT = -100;
    bool haveWp = false;
    vector<V3> visited;
    int lastVisitTick = -100;
    V3 farmLast;
    double farmSince = -100;
    vector<V3> farmBad;

    double rnd() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (rng >> 11) * (1.0 / 9007199254740992.0);
    }

    void init(const json& hello) {
        cfg = hello["config"];
        uint64_t s = hello.value("seed", 1u);
        rng ^= s * 0x9E3779B97F4A7C15ULL;
        if (rng == 0) rng = 1;
        gunSpeed = num(cfg["weapons"]["gun"], "speed", 252);
        mortarSpeed = num(cfg["weapons"]["mortar"], "speed", 72);
        mortarG = num(cfg["weapons"]["mortar"], "gravity", 22);
        laserCharge = num(cfg["weapons"]["laser"], "charge", 0.75);
        string dir = "/arena";
        if (hello.contains("map") && hello["map"].contains("dir")) dir = hello["map"]["dir"].get<string>();
        if (!map.load(dir)) {
            if (!map.load("/arena")) cerr << "map load failed\n";
        }
    }

    // pick the best of 16 directions given a desired movement vector
    V3 chooseMove(const V3& me, double dx, double dy, const vector<Enemy>& en, double zcx, double zcy, double zr,
                  double coverW) {
        double dl = hypot(dx, dy);
        if (dl < 1e-6) return V3{0, 0, 0};
        dx /= dl;
        dy /= dl;
        double best = -1e18;
        V3 bm;
        const int N = 16;
        for (int k = 0; k < N; k++) {
            double a = 2 * PI * k / N;
            double cx = cos(a), cy = sin(a);
            double sc = (cx * dx + cy * dy) * 3.0;
            // free path length
            double freeLen = 0;
            for (int s = 1; s <= 6; s++) {
                if (map.loaded && map.navBlockedXY(me.x + cx * 1.5 * s, me.y + cy * 1.5 * s)) break;
                freeLen = 1.5 * s;
            }
            if (freeLen < 9.0) sc -= (9.0 - freeLen) * 0.9;
            double ex = me.x + cx * 12, ey = me.y + cy * 12;
            if (hypot(ex - zcx, ey - zcy) > zr - 4) sc -= 2.5;
            sc += 0.3 * (cx * prevMove.x + cy * prevMove.y);
            if (coverW > 0 && map.loaded) {
                int cnt = 0;
                double covered = 0;
                for (auto& e : en) {
                    if (cnt++ >= 2) break;
                    V3 from{ex, ey, map.ground(ex, ey) + 8};
                    V3 to{e.pos.x, e.pos.y, e.pos.z + 8};
                    if (!map.los(from, to)) covered += 1;
                }
                sc += coverW * covered;
            }
            if (sc > best) {
                best = sc;
                bm = V3{cx, cy, 0};
            }
        }
        return bm;
    }

    void pickHideout(const V3& me, double cx, double cy, double R) {
        hideR = R;
        if (!map.loaded) return;
        map.flow(me.x, me.y);  // distances from my position
        double best = 1e18;
        long bi = -1;
        for (int j = 0; j < map.H; j++)
            for (int i = 0; i < map.W; i++) {
                size_t id = (size_t)j * map.W + i;
                if (map.blocked[id]) continue;
                float c = map.cost[id];
                if (c >= GameMap::INF / 2 || c > 220) continue;
                double wx = map.OX + (i + 0.5) * GameMap::CS, wy = map.OY + (j + 0.5) * GameMap::CS;
                double d = hypot(wx - cx, wy - cy);
                if (d > R * 0.8) continue;
                double sc = map.openness[id] + 0.9 * c + 0.1 * fabs(d - R * 0.55);
                if (sc < best) {
                    best = sc;
                    bi = (long)id;
                }
            }
        if (bi >= 0) {
            hideX = map.OX + ((bi % map.W) + 0.5) * GameMap::CS;
            hideY = map.OY + ((bi / map.W) + 0.5) * GameMap::CS;
        } else {
            hideX = cx;
            hideY = cy;
        }
        haveHide = true;
        map.flowGX = 1e9;  // force a fresh flow field towards the new goal
    }

    // could this enemy see me right now (proximity sensors or inside its 120 degree wedge, with line of sight)?
    bool canSeeUs(const Enemy& e, const V3& myEye) {
        if (e.dist <= 47) return true;
        if (e.dist > 205) return false;
        double brg = atan2(myEye.y - e.pos.y, myEye.x - e.pos.x);
        if (fabs(wrap(brg - e.aimYaw)) > 1.15) return false;
        return map.los(V3{e.pos.x, e.pos.y, e.pos.z + 8}, myEye);
    }

    static double angDiff(double a, double b) { return wrap(a - b); }

    // gun intercept: returns aim point
    V3 lead(const V3& from, const V3& tp, const V3& tv, double speed) {
        V3 p = tp;
        for (int it = 0; it < 3; it++) {
            double d = sqrt((p.x - from.x) * (p.x - from.x) + (p.y - from.y) * (p.y - from.y) + (p.z - from.z) * (p.z - from.z));
            double t = d / speed;
            p = V3{tp.x + tv.x * t, tp.y + tv.y * t, tp.z};
        }
        return p;
    }

    string act(const json& o) {
        int tick = o["tick"].get<int>();
        double t = num(o, "t", tick * 0.05);
        const json& S = o["self"];
        V3 pos = getV3(S["pos"]);
        V3 vel = getV3(S["vel"]);
        double hp = num(S, "hp", 1400), maxhp = num(S, "max_hp", 1400);
        double energy = num(S, "energy", 100);
        double yaw = S["aim"][0].get<double>(), pitch = S["aim"][1].get<double>();
        bool inZone = S.value("in_zone", true);
        const json& st = S["stats"];
        double muzzleH = num(st, "muzzle_height", 7), eyeH = num(st, "eye_height", 8);
        double walk = num(st, "walk_speed", 11.7);
        V3 muzzle{pos.x, pos.y, pos.z + muzzleH};
        V3 eye{pos.x, pos.y, pos.z + eyeH};
        double hpFrac = hp / max(1.0, maxhp);
        if (!camp && hpFrac < 0.5) {
            camp = true;
            haveHide = false;
        } else if (camp && hpFrac >= 0.85) {
            camp = false;
        }
        GHOST = camp;
        int cubesNow = (int)num(S, "cubes", 0);
        int myId = (int)num(S, "id", -1);

        // ---------------- weapons held
        int gunSlot = -1, laserSlot = -1, ltnSlot = -1, mortSlot = -1;
        string slotKind[2];
        double gunAmmo = 0, laserChargeNow = 0, laserCd = 0, mortCd = 0, gunSpeedNow = gunSpeed, mortSpeedNow = mortarSpeed;
        double gunRange = 230;
        for (int i = 0; i < 2 && i < (int)S["weapons"].size(); i++) {
            const json& w = S["weapons"][i];
            if (!w.is_object()) continue;
            string k = w.value("kind", "");
            slotKind[i] = k;
            if (k == "gun") {
                gunSlot = i;
                gunAmmo = num(w, "ammo", 0);
                if (w.contains("stats")) {
                    gunSpeedNow = num(w["stats"], "speed", gunSpeed);
                    gunRange = num(w["stats"], "range", 230);
                }
            } else if (k == "laser") {
                laserSlot = i;
                laserChargeNow = num(w, "charge", 0);
                laserCd = num(w, "cooldown", 0);
            } else if (k == "lightning") {
                ltnSlot = i;
            } else if (k == "mortar") {
                mortSlot = i;
                mortCd = num(w, "cooldown", 0);
                if (w.contains("stats")) mortSpeedNow = num(w["stats"], "speed", mortarSpeed);
            }
        }

        if (tick - lastVisitTick >= 20) {
            visited.push_back(pos);
            lastVisitTick = tick;
            if (visited.size() > 200) visited.erase(visited.begin());
        }
        // ---------------- events
        int pick = -1;
        double hitBearing = 0;
        bool gotHit = false, hitRear = false;
        for (auto& e : o["events"]) {
            string ty = e.value("type", "");
            if (ty == "damaged") {
                if (e.contains("by") && !e["by"].is_null()) {
                    gotHit = true;
                    hitBearing = num(e, "bearing", 0);
                    hitRear = e.value("rear", false);
                }
            } else if (ty == "take_failed") {
                badPickups.insert((int)num(e, "id", -1));
            }
        }
        if (gotHit) {
            underFireUntil = t + 3.0;
            lastHitBearing = hitBearing;
        }
        if (o.contains("offer") && o["offer"].is_object()) {
            static const char* pri[] = {"armor",     "plating",  "gun_damage", "nanites",   "servos",         "gun_rate",
                                        "gyros",     "gun_mag",  "laser_damage", "laser_charge", "lightning_efficiency",
                                        "lightning_chain", "mortar_reload", "gun_velocity", "capacitor", "jets",
                                        "sensors", "reactor"};
            int bestIdx = 0, bestR = 1000;
            const json& ch = o["offer"]["choices"];
            for (int i = 0; i < (int)ch.size(); i++) {
                string id = ch[i].value("id", "");
                int r = 500;
                for (int p = 0; p < (int)(sizeof(pri) / sizeof(pri[0])); p++)
                    if (id == pri[p]) r = p;
                if (r < bestR) {
                    bestR = r;
                    bestIdx = i;
                }
            }
            pick = bestIdx;
        }

        // ---------------- visible things
        vector<Enemy> en;
        vector<pair<V3, V3>> npcs;
        for (auto& m : o["mechs"]) {
            string kind = m.value("kind", "mech");
            if (kind == "npc") {
                npcs.push_back({getV3(m["pos"]), getV3(m["vel"])});
                continue;
            }
            Enemy e;
            e.id = (int)num(m, "id", -1);
            e.pos = getV3(m["pos"]);
            e.vel = getV3(m["vel"]);
            if (m.contains("aim") && m["aim"].is_array() && m["aim"].size() >= 2) {
                e.aimYaw = m["aim"][0].get<double>();
                e.aimPitch = m["aim"][1].get<double>();
            }
            e.hp = num(m, "hp", 1400);
            e.maxhp = num(m, "max_hp", 1400);
            e.cubes = (int)num(m, "cubes", 0);
            e.charging = m.value("charging", false);
            e.dist = hdist(e.pos, pos);
            if (m.contains("weapons") && m["weapons"].is_array())
                for (auto& w : m["weapons"])
                    if (w.is_string() && w.get<string>() == "laser") e.hasLaser = true;
            en.push_back(e);
        }
        sort(en.begin(), en.end(), [](const Enemy& a, const Enemy& b) { return a.dist < b.dist; });

        // ---------------- zone
        const json& Z = o["zone"];
        V3 zc0 = getV3(Z["center"]), zc1 = getV3(Z["next_center"]);
        double zr0 = num(Z, "radius", 560), zr1 = num(Z, "next_radius", zr0);
        double ss = num(Z, "shrink_start", 0), se = num(Z, "shrink_end", 0);
        double f = (se > ss) ? clampd((t - ss) / (se - ss), 0, 1) : (t >= ss ? 1 : 0);
        double zcx = zc0.x + (zc1.x - zc0.x) * f, zcy = zc0.y + (zc1.y - zc0.y) * f;
        double zr = zr0 + (zr1 - zr0) * f;
        // target spot inside the next circle, on my side of it
        double gx, gy;
        {
            double vx = pos.x - zc1.x, vy = pos.y - zc1.y;
            double vl = hypot(vx, vy);
            double rr = max(0.0, zr1 * 0.55);
            if (zr1 < 40) rr = zr1 * 0.3;
            if (vl < 1e-3) {
                vx = 1;
                vy = 0;
                vl = 1;
            }
            double use = min(vl, rr);
            gx = zc1.x + vx / vl * use;
            gy = zc1.y + vy / vl * use;
        }
        if (GHOST && (!haveHide || fabs(hideR - zr1) > 0.5)) pickHideout(pos, zc1.x, zc1.y, zr1);
        if (GHOST && haveHide) {
            gx = hideX;
            gy = hideY;
        }
        // refresh flow field to the goal when needed
        double gd = hypot(gx - map.flowGX, gy - map.flowGY);
        if (map.loaded && (gd > 12 || t - flowRefreshT > 4.0 || map.cost.empty())) {
            map.flow(gx, gy);
            flowRefreshT = t;
        }
        double pathLen = map.loaded ? map.costAt(pos.x, pos.y) : hypot(gx - pos.x, gy - pos.y);
        if (pathLen >= GameMap::INF / 2) pathLen = hypot(gx - pos.x, gy - pos.y) * 1.3;
        double distGoal = hypot(gx - pos.x, gy - pos.y);
        double leadT = pathLen / max(1.0, walk * 0.85) + 6.0;
        bool outsideNow = !inZone || hypot(pos.x - zcx, pos.y - zcy) > zr - 3;
        bool outsideNext = hypot(pos.x - zc1.x, pos.y - zc1.y) > zr1 * 0.8;
        bool zoneUrgent = outsideNow || (outsideNext && t >= ss - leadT);

        // zone-direction (towards goal)
        auto zoneDir = [&](double& dx, double& dy) {
            if (!(map.loaded && map.flowDir(pos.x, pos.y, dx, dy))) {
                dx = gx - pos.x;
                dy = gy - pos.y;
                double l = hypot(dx, dy);
                if (l > 1e-6) {
                    dx /= l;
                    dy /= l;
                }
            }
        };

        // ---------------- dodge incoming projectiles
        double dodgeX = 0, dodgeY = 0, dodgeStrength = 0;
        V3 myC{pos.x, pos.y, pos.z + 4.5};
        for (auto& p : o["projectiles"]) {
            string k = p.value("kind", "gun");
            V3 pp = getV3(p["pos"]), pv = getV3(p["vel"]);
            if (k == "mortar") {
                V3 q = pp, v = pv;
                double tt = 0;
                while (tt < 4.0) {
                    q.x += v.x * 0.05;
                    q.y += v.y * 0.05;
                    q.z += v.z * 0.05;
                    v.z -= mortarG * 0.05;
                    tt += 0.05;
                    if (q.z <= map.ground(q.x, q.y)) break;
                }
                double dl = hypot(q.x - pos.x, q.y - pos.y);
                if (dl < 12 && tt < 3.5) {
                    double ax = pos.x - q.x, ay = pos.y - q.y, al = hypot(ax, ay);
                    if (al < 1e-3) {
                        ax = 1;
                        ay = 0;
                        al = 1;
                    }
                    double w = (12 - dl) / 12 + 0.5;
                    dodgeX += ax / al * w;
                    dodgeY += ay / al * w;
                    dodgeStrength += w;
                }
                continue;
            }
            double rx = pp.x - myC.x, ry = pp.y - myC.y, rz = pp.z - myC.z;
            double vv = pv.x * pv.x + pv.y * pv.y + pv.z * pv.z;
            if (vv < 1) continue;
            double ts = -(rx * pv.x + ry * pv.y + rz * pv.z) / vv;
            if (ts < 0 || ts > 0.6) continue;
            double cx = rx + pv.x * ts, cy = ry + pv.y * ts, cz = rz + pv.z * ts;
            double miss = sqrt(cx * cx + cy * cy + cz * cz);
            if (miss > 4.5) continue;
            // perpendicular (horizontal) to bullet travel
            double hl = hypot(pv.x, pv.y);
            if (hl < 1e-3) continue;
            double px = -pv.y / hl, py = pv.x / hl;
            // side: move away from the closest-approach offset
            double side = (-(cx) * px - (cy) * py);
            double sgn = (fabs(side) < 0.3) ? strafeSign : (side > 0 ? 1 : -1);
            double w = (4.5 - miss) / 4.5 + 0.6;
            dodgeX += px * sgn * w;
            dodgeY += py * sgn * w;
            dodgeStrength += w;
        }
        // enemy lasers lined up on me
        for (auto& e : en) {
            if (e.charging && e.dist < 290) {
                double brg = atan2(pos.y - e.pos.y, pos.x - e.pos.x);
                if (fabs(angDiff(brg, e.aimYaw)) < 0.12) {
                    double px = -sin(e.aimYaw), py = cos(e.aimYaw);
                    double side = (pos.x - e.pos.x) * px + (pos.y - e.pos.y) * py;
                    double sgn = (fabs(side) < 0.3) ? strafeSign : (side > 0 ? 1 : -1);
                    dodgeX += px * sgn * 2.0;
                    dodgeY += py * sgn * 2.0;
                    dodgeStrength += 2.0;
                }
            }
        }

        // strafe timer
        if (tick >= nextFlip) {
            strafeSign = (rnd() < 0.5) ? -1 : 1;
            nextFlip = tick + 6 + (int)(rnd() * 10);
        }

        // ---------------- action defaults
        double moveX = 0, moveY = 0, speed = 1.0;
        bool wantBoost = false;
        double aimYaw = yaw, aimPitch = pitch;
        bool fire0 = false, fire1 = false;
        bool reload = false;
        int take = -1, drop = -1;
        string mode = "idle";

        const Enemy* tgt = nullptr;
        for (auto& e : en)
            if (e.dist < 260) {
                tgt = &e;
                break;
            }
        // prefer weaker targets that are about as close
        if (tgt) {
            double bestScore = 1e18;
            for (auto& e : en) {
                if (e.dist > 240) continue;
                double sc = e.dist + e.hp * 0.03;
                if (sc < bestScore) {
                    bestScore = sc;
                    tgt = &e;
                }
            }
        }

        bool farSeen = false, snipe = false;
        if (GHOST && tgt && t >= underFireUntil) {
            if (tgt->dist >= 60 && !canSeeUs(*tgt, eye) && (gunSlot >= 0 || laserSlot >= 0 || mortSlot >= 0)) {
                snipe = true;  // they cannot see us: take a free shot from where we stand
            } else if (tgt->dist >= 70) {
                farSeen = true;
                lastEnemyPos = tgt->pos;
                lastEnemyT = t;
                tgt = nullptr;
            }
        }
        if (tgt) {
            // ======================= COMBAT =======================
            int n150 = 0;
            double cxs = 0, cys = 0;
            for (auto& e : en)
                if (e.dist < 150) {
                    n150++;
                    double w = 1.0 / max(10.0, e.dist);
                    cxs += (pos.x - e.pos.x) * w;
                    cys += (pos.y - e.pos.y) * w;
                }
            lastEnemyPos = tgt->pos;
            lastEnemyT = t;
            bool flee = !snipe && (GHOST || hpFrac < 0.38 || n150 >= 3);
            mode = flee ? "flee" : "fight";
            V3 tc{tgt->pos.x, tgt->pos.y, tgt->pos.z + 4.5 * (1 + 0.05 * tgt->cubes)};
            double d = tgt->dist;
            double dx = 0, dy = 0;
            double R = 36;
            if (ltnSlot >= 0) R = 20;
            else if (laserSlot >= 0) R = 55;
            else if (mortSlot >= 0) R = 45;
            double tx = tgt->pos.x - pos.x, ty = tgt->pos.y - pos.y;
            double tl = max(1e-6, hypot(tx, ty));
            tx /= tl;
            ty /= tl;
            if (flee) {
                double al = hypot(cxs, cys);
                double ax = al > 1e-6 ? cxs / al : -tx, ay = al > 1e-6 ? cys / al : -ty;
                double zx = 0, zy = 0;
                zoneDir(zx, zy);
                dx = ax * 1.0 + zx * 0.6 + (-ty) * strafeSign * 0.3;
                dy = ay * 1.0 + zy * 0.6 + (tx) * strafeSign * 0.3;
                if (energy > 25 || boostLatch) wantBoost = true;
            } else {
                double radial = clampd((d - R) / 20.0, -1, 1);
                dx = tx * radial + (-ty) * strafeSign;
                dy = ty * radial + (tx) * strafeSign;
                if (zoneUrgent) {
                    double zx = 0, zy = 0;
                    zoneDir(zx, zy);
                    dx += zx * 1.5;
                    dy += zy * 1.5;
                }
            }
            if (dodgeStrength > 0) {
                dx = dx * 0.5 + dodgeX * 1.8;
                dy = dy * 0.5 + dodgeY * 1.8;
            }
            V3 mv = chooseMove(pos, dx, dy, en, zcx, zcy, zr, flee ? 1.2 : 0.4);
            moveX = mv.x;
            moveY = mv.y;
            if (snipe && dodgeStrength <= 0) {
                moveX = 0;
                moveY = 0;
                wantBoost = false;
            }
            // ---------- aiming and firing
            bool clear = map.los(eye, V3{tgt->pos.x, tgt->pos.y, tgt->pos.z + 6});
            // gun
            V3 ap = lead(muzzle, tc, tgt->vel, gunSpeedNow);
            double gdx = ap.x - muzzle.x, gdy = ap.y - muzzle.y, gdz = ap.z - muzzle.z;
            double gh = max(1e-6, hypot(gdx, gdy));
            double wantYaw = atan2(gdy, gdx), wantPitch = atan2(gdz, gh);
            aimYaw = wantYaw;
            aimPitch = clampd(wantPitch, -0.7, 1.3);
            double errYaw = fabs(angDiff(wantYaw, yaw));
            double thr = clampd(atan2(3.0, max(d, 1.0)), 0.02, 0.16);
            double errPit = fabs(aimPitch - pitch);
            if (gunSlot >= 0 && d < gunRange - 12 && clear && errYaw < thr && errPit < thr + 0.03 && gunAmmo > 0) {
                if (gunSlot == 0) fire0 = true;
                else fire1 = true;
            }
            // lightning
            if (ltnSlot >= 0 && d < 40 && energy >= 8 && clear && errYaw < 0.4) {
                if (ltnSlot == 0) fire0 = true;
                else fire1 = true;
            }
            // laser: hold to charge, release to fire
            if (laserSlot >= 0) {
                bool lf = false;
                if (laserChargeNow > 0.01) {
                    lf = (laserChargeNow < laserCharge * 0.92) && d < 270 && clear;
                    if (laserChargeNow >= laserCharge * 0.92 && errYaw > 0.05 && d < 270 && clear) lf = true;
                } else if (laserCd <= 0.12 && d < 250 && clear && !tgt->charging) {
                    lf = true;
                }
                // aim with lead for hitscan (small latency)
                V3 lp{tc.x + tgt->vel.x * 0.13, tc.y + tgt->vel.y * 0.13, tc.z};
                if (lf || laserChargeNow > 0.01) {
                    double ldx = lp.x - muzzle.x, ldy = lp.y - muzzle.y, ldz = lp.z - muzzle.z;
                    aimYaw = atan2(ldy, ldx);
                    aimPitch = clampd(atan2(ldz, max(1e-6, hypot(ldx, ldy))), -0.7, 1.3);
                }
                if (laserSlot == 0) fire0 = fire0 || lf;
                else fire1 = fire1 || lf;
                if (lf) {
                    moveX *= 0.9;
                    moveY *= 0.9;
                }
            }
            // mortar lob
            if (mortSlot >= 0 && d > 28 && d < 205 && mortCd <= 0.15 && laserSlot < 0 && ltnSlot < 0) {
                V3 tube{pos.x, pos.y, pos.z + eyeH + 1.5};
                V3 pr = tc;
                double dd = d, pitchM = 0.78;
                bool ok = false;
                for (int it = 0; it < 3; it++) {
                    dd = hypot(pr.x - tube.x, pr.y - tube.y);
                    double dz = pr.z - tube.z;
                    double v = mortSpeedNow;
                    double a = mortarG * dd * dd / (2 * v * v);
                    double disc = dd * dd - 4 * a * (dz + a);
                    if (disc >= 0 && a > 1e-9) {
                        double u = (dd - sqrt(disc)) / (2 * a);
                        pitchM = atan(u);
                        ok = true;
                    } else {
                        pitchM = 0.78;
                        ok = false;
                    }
                    double ft = dd / (v * max(0.2, cos(pitchM)));
                    pr = V3{tc.x + tgt->vel.x * ft, tc.y + tgt->vel.y * ft, tc.z};
                }
                if (ok && !clear) {
                    // high arc to clear buildings
                    double v = mortSpeedNow, a = mortarG * dd * dd / (2 * v * v);
                    double dz = pr.z - tube.z, disc = dd * dd - 4 * a * (dz + a);
                    if (disc >= 0) pitchM = atan((dd + sqrt(disc)) / (2 * a));
                }
                if (ok) {
                    double mdx = pr.x - tube.x, mdy = pr.y - tube.y;
                    double wy = atan2(mdy, mdx);
                    if (laserSlot < 0 && ltnSlot < 0) {
                        // mortar is the only special weapon: aim it (gun is otherwise held back)
                        if (!(gunSlot >= 0 && clear && errYaw < thr && gunAmmo > 0 && d < 120)) {
                            aimYaw = wy;
                            aimPitch = clampd(pitchM, -0.7, 1.3);
                            double ey = fabs(angDiff(wy, yaw)), ep = fabs(clampd(pitchM, -0.7, 1.3) - pitch);
                            if (ey < 0.04 && ep < 0.05) {
                                if (mortSlot == 0) fire0 = true;
                                else fire1 = true;
                            }
                        }
                    }
                }
            }
            if (gunAmmo <= 0 && gunSlot >= 0) reload = true;
        } else {
            // ======================= NO ENEMY IN SIGHT =======================
            bool recentHit = t < underFireUntil;
            double dx = 0, dy = 0;
            bool moving = false;
            bool free = (t >= ss - leadT) ? true : false;  // slack exhausted
            vector<Enemy> pseudo;
            double coverWt = 0;
            bool farmFire = false;
            double farmYaw = 0, farmPitch = 0;
            // 1. loot (weapon pickups / cubes) when not pressed by the zone
            double lootX = 0, lootY = 0;
            bool haveLoot = false;
            if (!GHOST && (!zoneUrgent || distGoal < 25)) {
                int wantRank = 0;
                string held[2] = {slotKind[0], slotKind[1]};
                int heldRank = max(weaponRank(held[0]), weaponRank(held[1]));
                bool emptySlot = !S["weapons"][1].is_object() || !S["weapons"][0].is_object();
                double bestD = 80;
                for (auto& pk : o["pickups"]) {
                    int id = (int)num(pk, "id", -1);
                    if (badPickups.count(id)) continue;
                    string k = pk.value("kind", "");
                    int r = weaponRank(k);
                    if (r == 0) continue;
                    if (k == held[0] || k == held[1]) continue;
                    int candDrop = -1;
                    if (!emptySlot) {
                        bool allowed = false;
                        if (gunSlot >= 0) {
                            int ss = 1 - gunSlot;
                            string sk = slotKind[ss];
                            bool pair = (k == "lightning" && sk == "laser") || (k == "laser" && sk == "lightning");
                            if (pair) {
                                allowed = true;
                                candDrop = gunSlot;  // swap the gun out for the lightning+laser pair
                            } else if (r > weaponRank(sk)) {
                                allowed = true;
                                candDrop = ss;
                            }
                        }
                        if (!allowed) continue;
                        if (laserChargeNow > 0 && candDrop == laserSlot) continue;
                    }
                    V3 pp = getV3(pk["pos"]);
                    double dd = hdist(pp, pos);
                    double sc = dd - r * 12;
                    if (sc < bestD && fabs(pp.z - pos.z) < 14) {
                        bestD = sc;
                        lootX = pp.x;
                        lootY = pp.y;
                        haveLoot = true;
                        wantRank = r;
                        take = -1;
                        drop = -1;
                        if (dd < 3.6) {
                            take = id;
                            if (!emptySlot) drop = candDrop;
                        }
                    }
                }
                (void)wantRank;
                if (!haveLoot) {
                    double bd = 45;
                    for (auto& cb : o["cubes"]) {
                        V3 cp = getV3(cb["pos"]);
                        double dd = hdist(cp, pos);
                        if (dd < bd && fabs(cp.z - pos.z) < 12) {
                            bd = dd;
                            lootX = cp.x;
                            lootY = cp.y;
                            haveLoot = true;
                        }
                    }
                }
            }
            // supply drop worth walking to
            bool haveDrop = false;
            V3 dropPos;
            double dropRad = 8, dropLands = 0;
            if (!GHOST && !recentHit && hpFrac > 0.3) {
                double bestDD = 1e9;
                for (auto& dr : o["drops"]) {
                    V3 dp = getV3(dr["pos"]);
                    double landsIn = num(dr, "lands_in", 0);
                    double rad = num(dr, "radius", 8);
                    double dd = hdist(dp, pos);
                    bool someoneElse = false;
                    if (dr.contains("holder") && dr["holder"].is_number()) {
                        int h = (int)dr["holder"].get<double>();
                        if (h != myId && num(dr, "progress", 0) > 0.3) someoneElse = true;
                    }
                    if (someoneElse) continue;
                    if (hypot(dp.x - zc1.x, dp.y - zc1.y) > zr1 - 4 && hypot(dp.x - zcx, dp.y - zcy) > zr - 8) continue;
                    if (dd > 260) continue;
                    if (dd / max(1.0, walk * 0.9) > landsIn + 30) continue;
                    if (dd < bestDD) {
                        bestDD = dd;
                        dropPos = dp;
                        dropRad = rad;
                        dropLands = landsIn;
                        haveDrop = true;
                    }
                }
            }
            // farm target: nearest crate or drone early in the match
            int farmIdx = -1;
            V3 farmPos, farmVel;
            bool farmIsNpc = false;
            if (!GHOST && t < 320 && cubesNow < 10 && hpFrac > 0.6 && !zoneUrgent && !haveLoot && (gunSlot >= 0 || ltnSlot >= 0)) {
                double bd = 140;
                int idx = 0;
                for (auto& cr : o["crates"]) {
                    V3 cp = getV3(cr["pos"]);
                    cp.z += 1.5;
                    double dd = hdist(cp, pos);
                    bool bad = false;
                    for (auto& b : farmBad)
                        if (hdist(b, cp) < 3) bad = true;
                    if (!bad && dd < bd && fabs(cp.z - pos.z) < 14) {
                        bd = dd;
                        farmIdx = idx;
                        farmPos = cp;
                        farmVel = V3{0, 0, 0};
                        farmIsNpc = false;
                    }
                    idx++;
                }
                for (auto& np : npcs) {
                    V3 cp = np.first;
                    cp.z += 1.75;
                    double dd = hdist(cp, pos);
                    bool bad = false;
                    for (auto& b : farmBad)
                        if (hdist(b, cp) < 12) bad = true;
                    if (!bad && dd < bd && fabs(cp.z - pos.z) < 14) {
                        bd = dd;
                        farmIdx = idx;
                        farmPos = cp;
                        farmVel = np.second;
                        farmIsNpc = true;
                    }
                    idx++;
                }
                if (farmIdx >= 0) {
                    if (hdist(farmLast, farmPos) > (farmIsNpc ? 14 : 3)) {
                        farmSince = t;
                    }
                    farmLast = farmPos;
                    if (t - farmSince > 22) {
                        farmBad.push_back(farmPos);
                        farmIdx = -1;
                    }
                }
            }
            double zx = 0, zy = 0;
            zoneDir(zx, zy);
            if (zoneUrgent) {
                mode = "zone";
                dx = zx;
                dy = zy;
                moving = true;
                if (outsideNow && (energy > 30 || boostLatch)) wantBoost = true;
                else if (t >= ss - leadT * 0.4 && energy > 50) wantBoost = true;
            } else if (haveLoot) {
                mode = "loot";
                dx = lootX - pos.x;
                dy = lootY - pos.y;
                double l = hypot(dx, dy);
                if (l > 1e-6 && l < 3.0) {
                    moving = false;
                } else {
                    moving = true;
                    // use flow-free straight line with obstacle-aware chooser
                }
                speed = (l < 8) ? 0.5 : 1.0;
            } else if (recentHit) {
                mode = "hit";
                // we are being shot by something unseen: face it and move across the line
                double px = -sin(lastHitBearing), py = cos(lastHitBearing);
                dx = px * strafeSign + zx * 0.5;
                dy = py * strafeSign + zy * 0.5;
                moving = true;
                aimYaw = lastHitBearing;
            } else if (haveDrop) {
                mode = "drop";
                double ddx = dropPos.x - pos.x, ddy = dropPos.y - pos.y;
                double dd = hypot(ddx, ddy);
                if (dd > dropRad * 0.45) {
                    dx = ddx;
                    dy = ddy;
                    moving = true;
                    if (dd / max(1.0, walk * 0.9) > dropLands + 6 && energy > 50) wantBoost = true;
                } else {
                    moving = false;
                }
            } else if ((GHOST && farSeen) || (t - lastEnemyT < 8.0 && hpFrac < 0.9 && hdist(lastEnemyPos, pos) < 260)) {
                mode = "evade";
                speed = farSeen ? 0.42 : 1.0;
                double ax = pos.x - lastEnemyPos.x, ay = pos.y - lastEnemyPos.y;
                double al = max(1e-6, hypot(ax, ay));
                dx = ax / al + zx * 0.5;
                dy = ay / al + zy * 0.5;
                moving = true;
                Enemy pe;
                pe.pos = lastEnemyPos;
                pseudo.push_back(pe);
                coverWt = 1.5;
                if (!GHOST && energy > 60) wantBoost = true;
            } else if (farmIdx >= 0) {
                mode = "farm";
                V3 ft = farmPos;
                double fdx = ft.x - pos.x, fdy = ft.y - pos.y;
                double fd = max(1e-6, hypot(fdx, fdy));
                bool seen = map.los(eye, ft);
                if (fd > 34 || (!seen && fd > 6)) {
                    dx = fdx;
                    dy = fdy;
                    moving = true;
                } else if (farmIsNpc && fd < 18) {
                    dx = -fdx;
                    dy = -fdy;
                    moving = true;
                }
                V3 ap = farmIsNpc ? lead(muzzle, ft, farmVel, gunSpeedNow) : ft;
                double adx = ap.x - muzzle.x, ady = ap.y - muzzle.y, adz = ap.z - muzzle.z;
                aimYaw = atan2(ady, adx);
                aimPitch = clampd(atan2(adz, max(1e-6, hypot(adx, ady))), -0.7, 1.3);
                bool clr = map.los(eye, ap);
                double ey = fabs(angDiff(aimYaw, yaw));
                double th2 = clampd(atan2(2.5, max(fd, 1.0)), 0.03, 0.2);
                if (clr && ey < th2 && fd < 80) {
                    if (ltnSlot >= 0 && fd < 38 && energy > 30) {
                        if (ltnSlot == 0) fire0 = true; else fire1 = true;
                    } else if (gunSlot >= 0 && gunAmmo > 0) {
                        if (gunSlot == 0) fire0 = true; else fire1 = true;
                    }
                }
                farmFire = true;
                speed = 0.8;
            } else if (!GHOST && hpFrac > 0.5 && !zoneUrgent && ((t < 150 && !(ltnSlot >= 0 && laserSlot >= 0)) || (t < 220 && cubesNow < 5))) {
                mode = "explore";
                bool need = !haveWp || t - wpT > 25 || hdist(wp, pos) < 14;
                if (need) {
                    double bestSc = -1e18;
                    V3 bw = pos;
                    for (int k = 0; k < 16; k++) {
                        double a = 2 * PI * k / 16 + rnd() * 0.3;
                        for (double r : {70.0, 110.0}) {
                            V3 c{pos.x + cos(a) * r, pos.y + sin(a) * r, 0};
                            if (hypot(c.x - zc1.x, c.y - zc1.y) > zr1 - 25) continue;
                            if (hypot(c.x - zcx, c.y - zcy) > zr - 25) continue;
                            if (map.loaded && map.navBlockedXY(c.x, c.y)) continue;
                            double md = 1e9;
                            for (auto& v : visited) md = min(md, hdist(v, c));
                            double sc = min(md, 150.0) - 0.15 * hypot(c.x - zc1.x, c.y - zc1.y) + rnd() * 10;
                            if (sc > bestSc) {
                                bestSc = sc;
                                bw = c;
                            }
                        }
                    }
                    wp = bw;
                    haveWp = bestSc > -1e17;
                    wpT = t;
                }
                if (haveWp) {
                    dx = wp.x - pos.x;
                    dy = wp.y - pos.y;
                    moving = true;
                    speed = 1.0;
                } else {
                    mode = "hold";
                }
            } else if (distGoal > 8 && (GHOST || free || outsideNext)) {
                mode = "travel";
                dx = zx;
                dy = zy;
                moving = true;
                speed = (GHOST && !zoneUrgent) ? 0.42 : 1.0;
            } else {
                mode = "hold";
            }
            if (dodgeStrength > 0) {
                dx = dx * 0.5 + dodgeX * 1.8;
                dy = dy * 0.5 + dodgeY * 1.8;
                moving = true;
                speed = 1.0;
            }
            if (moving) {
                V3 mv = chooseMove(pos, dx, dy, pseudo.empty() ? en : pseudo, zcx, zcy, zr, coverWt);
                moveX = mv.x * speed;
                moveY = mv.y * speed;
            }
            // look direction: sounds first, else slow sweep
            if (mode != "hit" && mode != "farm") {
                bool heard = false;
                double bestAge = 1e9;
                for (auto& s : o["sounds"]) {
                    string k = s.value("kind", "");
                    string dist = s.value("dist", "far");
                    if (k == "step" || k == "boost" || k == "jet" || k == "reload" || k == "drop" || k == "npc_gun" || k == "crate") continue;
                    if (dist == "far" && k != "mortar") continue;
                    double age = num(s, "age", 0);
                    if (age < bestAge) {
                        bestAge = age;
                        aimYaw = num(s, "bearing", yaw);
                        heard = true;
                    }
                }
                if (!heard) {
                    if (moving && (moveX != 0 || moveY != 0)) aimYaw = atan2(moveY, moveX);
                    else {
                        scanPhase += 0.35;
                        aimYaw = wrap(yaw + 0.35);
                    }
                }
            }
            if (mode != "farm") aimPitch = 0;
            if (gunSlot >= 0 && gunAmmo < 6 && !moving && mode != "farm") reload = true;
        }

        // boost handling with hysteresis
        if (wantBoost) {
            if (energy < 8) boostLatch = false;
            else boostLatch = true;
        } else {
            boostLatch = false;
        }
        bool boost = wantBoost && energy >= 8 && (fabs(moveX) + fabs(moveY) > 0.1);
        if (laserChargeNow > 0.01) boost = false;

        prevMove = V3{moveX, moveY, 0};
        lastTick = tick;

        // ---------------- build output
        double ml = hypot(moveX, moveY);
        if (ml > 1.0) {
            moveX /= ml;
            moveY /= ml;
        }
        char buf[640];
        int n = snprintf(buf, sizeof(buf),
                         "{\"tick\":%d,\"move\":[%.3f,%.3f],\"boost\":%s,\"jump\":false,\"aim\":[%.4f,%.4f],\"fire\":[%s,%s]", tick,
                         fin(moveX), fin(moveY), boost ? "true" : "false", fin(aimYaw), fin(aimPitch), fire0 ? "true" : "false",
                         fire1 ? "true" : "false");
        string out(buf, n);
        if (reload) out += ",\"reload\":true";
        if (pick >= 0) out += ",\"pick\":" + to_string(pick);
        if (take >= 0) {
            out += ",\"take\":" + to_string(take);
            if (drop >= 0) out += ",\"drop\":" + to_string(drop);
        }
        out += "}\n";
        (void)mode;
        return out;
    }
};

// ------------------------------------------------------------------------------------ io
static string g_buf;
static bool g_eof = false;

static bool fillBuffer(bool block) {
    struct pollfd pfd;
    pfd.fd = 0;
    pfd.events = POLLIN;
    int r = poll(&pfd, 1, block ? -1 : 0);
    if (r <= 0) return false;
    char tmp[65536];
    ssize_t n = read(0, tmp, sizeof(tmp));
    if (n <= 0) {
        g_eof = true;
        return false;
    }
    g_buf.append(tmp, n);
    return true;
}

static string lineType(const string& line) {
    size_t p = line.find("\"type\"");
    if (p == string::npos) return "";
    p = line.find(':', p);
    if (p == string::npos) return "";
    p = line.find('"', p);
    if (p == string::npos) return "";
    size_t q = line.find('"', p + 1);
    if (q == string::npos) return "";
    return line.substr(p + 1, q - p - 1);
}

// returns false at EOF with nothing to process. Collects every complete line, keeps only the newest obs.
static bool nextMessage(string& msg, string& type) {
    for (;;) {
        // drain whatever is already waiting
        while (fillBuffer(false)) {
        }
        vector<string> lines;
        size_t pos;
        while ((pos = g_buf.find('\n')) != string::npos) {
            lines.push_back(g_buf.substr(0, pos));
            g_buf.erase(0, pos + 1);
        }
        if (!lines.empty()) {
            // an "end" or "hello" wins; otherwise newest obs
            string newestObs;
            for (auto& l : lines) {
                string ty = lineType(l);
                if (ty == "end" || ty == "hello") {
                    msg = l;
                    type = ty;
                    return true;
                }
                if (ty == "obs") newestObs = l;
            }
            if (!newestObs.empty()) {
                msg = newestObs;
                type = "obs";
                return true;
            }
            continue;
        }
        if (g_eof) return false;
        fillBuffer(true);
        if (g_eof && g_buf.empty()) return false;
    }
}

static void sendLine(const string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(1, s.data() + off, s.size() - off);
        if (n <= 0) return;
        off += n;
    }
}

int main() {
    ios::sync_with_stdio(false);
    Bot bot;
    string msg, type;
    bool haveHello = false;
    while (nextMessage(msg, type)) {
        if (type == "hello") {
            try {
                json h = json::parse(msg);
                bot.init(h);
                haveHello = true;
            } catch (...) {
                cerr << "hello parse failed\n";
            }
            sendLine("{\"type\":\"ready\",\"name\":\"osu-cpp-hybrid\"}\n");
        } else if (type == "obs") {
            if (!haveHello) continue;
            try {
                json o = json::parse(msg);
                sendLine(bot.act(o));
            } catch (const std::exception& e) {
                cerr << "act error: " << e.what() << "\n";
                try {
                    json o = json::parse(msg);
                    char b[64];
                    int n = snprintf(b, sizeof(b), "{\"tick\":%d}\n", o["tick"].get<int>());
                    sendLine(string(b, n));
                } catch (...) {
                }
            }
        } else if (type == "end") {
            break;
        }
    }
    return 0;
}
