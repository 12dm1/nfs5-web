// nfs5-web, feature port of the PC wrapper's opponent_similar.cpp (OS1): Quick Race opponents close to the player's car.
// Each car has a performance index from its .sim and parts, stock and with a "full tuning" built from its catalogue. Only the data
// lives here: runtime/pu_ext.c reads the game's accessors and calls its setters through the callbacks of OppGame.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "opp_similar.h"

extern "C" int fileio_resolve_game(const char *win, char *out, size_t outsz);

namespace {
const int kBelow = 3, kAbove = 4, kSlots = 7, kMaxParts = 20, kPartSize = 0x168, kSimSize = 328;
const char *kModeNames[3] = {"similar", "similar stock", "similar tuned"};

uint32_t g_rng;
uint32_t Rand() {
    if (!g_rng) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g_rng = (uint32_t)t.tv_nsec ^ (uint32_t)t.tv_sec ^ 0x9E3779B9u; if (!g_rng) g_rng = 1; }
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
int RandBelow(int n) { return n > 0 ? (int)(Rand() % (uint32_t)n) : 0; }

template <class T> T Rd(const uint8_t *p, int off) { T v; memcpy(&v, p + off, sizeof(T)); return v; }
std::string Str(const uint8_t *p, int off, int len) {
    int n = 0;
    while (n < len && p[off + n]) ++n;
    return std::string((const char *)p + off, (size_t)n);
}

// nfs5.car record (0x670)
const int kRecId = 0x000, kRecName = 0x002, kRecSim = 0x0B1, kRecAi = 0x115, kRecKind = 0x1F4;   // kind: 0 racer, 1 traffic, 2 cop
const int kRecCatalog = 0x330, kRecCatalogEnd = 0x3BC, kRecStockCount = 0x3BC, kRecStock = 0x3BE;
const int kRecColours = 0x3E6, kRecColourTable = 0x3EC, kRecStripes = 0x528, kRecStripeTable = 0x530;
// nfs5.prt record (0x168)
const int kPrtId = 0x00, kPrtFamily = 0x08, kPrtType = 0x26, kPrtName = 0x28, kPrtWeight = 0x54, kPrtPrice = 0x5C, kPrtFinal = 0xC0;
const int kPrtGearCount = 0xEA, kPrtGears = 0xF8, kPrtCurve = 0x110;
// .sim file
const int kSimMass = 0x40, kSimGearCount = 0x50, kSimGears = 0x68, kSimFinal = 0x80, kSimTyre = 0x84, kSimRedline = 0x94, kSimCurve = 0x9C;
const int kCurvePoints = 21;
const float kAddScale = 0.67f;   // 005B4A38

bool NoPartsFamily(const std::string &ai) {
    const char *a = ai.c_str();
    return !strcasecmp(a, "959") || !strcasecmp(a, "928") || !strcasecmp(a, "GT2") || !strcasecmp(a, "GT3");
}
float CurvePeakHp(const float *curve) {
    float best = 0.0f;
    for (int i = 0; i < kCurvePoints; ++i) {
        const float hp = curve[i] * (float)(i * 500) / 5252.0f;   // 500 rpm per point
        if (hp > best) best = hp;
    }
    return best;
}
float PartPeakHp(const uint8_t *part) {
    float c[kCurvePoints];
    memcpy(c, part + kPrtCurve, sizeof c);
    return CurvePeakHp(c);
}

const uint8_t *g_partTable;
int g_partCount;
std::map<int, const uint8_t *> g_partById;
void ClearPerf();
void RefreshParts(const OppGame *g) {
    if (g->part_table == g_partTable && g->part_count == g_partCount) return;
    g_partTable = g->part_table; g_partCount = g->part_count;
    g_partById.clear();
    ClearPerf();
    if (!g_partTable || g_partCount <= 0 || g_partCount > 10000) return;
    for (int i = 0; i < g_partCount; ++i) {
        const uint8_t *p = g_partTable + i * kPartSize;
        g_partById.emplace((int)Rd<int16_t>(p, kPrtId), p);
    }
}
const uint8_t *FindPart(int id) { auto it = g_partById.find(id); return it == g_partById.end() ? nullptr : it->second; }

std::map<std::string, std::vector<uint8_t>> g_sims;
const char *g_simDir = "";
const uint8_t *LoadSim(const std::string &name) {
    auto it = g_sims.find(name);
    if (it != g_sims.end()) return it->second.empty() ? nullptr : it->second.data();
    std::vector<uint8_t> data;
    for (const std::string &file : {name, std::string("default")}) {
        const std::string win = std::string(g_simDir) + file + ".sim";
        char path[1024];
        if (!fileio_resolve_game(win.c_str(), path, sizeof path)) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        std::vector<uint8_t> buf(kSimSize);
        const size_t got = fread(buf.data(), 1, buf.size(), f);
        fclose(f);
        if (got == buf.size()) { data.swap(buf); break; }
    }
    auto &slot = g_sims[name];
    slot.swap(data);
    return slot.empty() ? nullptr : slot.data();
}

struct Key {
    int16_t type; std::string name;
    bool operator==(const Key &o) const { return type == o.type && name == o.name; }
};
Key KeyOf(const uint8_t *p) { return {Rd<int16_t>(p, kPrtType), Str(p, kPrtName, 30)}; }

// Stock parts in the order 004EE9A0 leaves them (type, price, id).
std::vector<const uint8_t *> StockParts(const uint8_t *rec) {
    std::vector<const uint8_t *> out;
    int n = Rd<int16_t>(rec, kRecStockCount);
    n = n < 0 ? 0 : n > kMaxParts ? kMaxParts : n;
    for (int i = 0; i < n; ++i)
        if (const uint8_t *p = FindPart(Rd<uint16_t>(rec, kRecStock + 2 * i))) out.push_back(p);
    std::stable_sort(out.begin(), out.end(), [](const uint8_t *a, const uint8_t *b) {
        const int16_t ta = Rd<int16_t>(a, kPrtType), tb = Rd<int16_t>(b, kPrtType);
        if (ta != tb) return ta < tb;
        const int pa = Rd<int>(a, kPrtPrice), pb = Rd<int>(b, kPrtPrice);
        if (pa != pb) return pa < pb;
        return Rd<int16_t>(a, kPrtId) < Rd<int16_t>(b, kPrtId);
    });
    return out;
}
// What 004ED880 does to the part vector.
void SimulateInstall(std::vector<const uint8_t *> &vec, const uint8_t *part) {
    const Key k = KeyOf(part);
    for (auto it = vec.begin(); it != vec.end(); ++it)
        if (KeyOf(*it) == k) { vec.erase(it); break; }
    vec.push_back(part);
}
// Lower = dropped first when the full build does not fit 20 slots.
int KeepPriority(const Key &k) {
    switch (k.type) {
    case 8: return 0;                               // sway bars (+kg)
    case 6: return 1;                               // dams, spoiler (+kg)
    case 3: return k.name == "Gear Box" ? 8 : 2;    // shifter etc.
    case 5: return 3;
    case 4: return 4;
    case 2: return 5;
    case 9: return 6;                               // tyres
    case 7: return 7;                               // lightweight panels
    case 1: return 9;                               // engine
    default: return 1;
    }
}
// "Full tuning" from the car's catalogue: strongest engine part per category, lightest panel per category, most expensive part otherwise.
std::vector<const uint8_t *> FullBuild(const uint8_t *rec, const std::vector<const uint8_t *> &stock) {
    std::vector<const uint8_t *> b;
    if (NoPartsFamily(Str(rec, kRecAi, 50))) return b;
    // The catalogue holds a few foreign ids; keep the stock parts' family.
    std::map<std::string, int> families;
    for (const uint8_t *p : stock) ++families[Str(p, kPrtFamily, 30)];
    std::string family;
    int best = 0;
    for (auto &f : families) if (f.second > best) { best = f.second; family = f.first; }
    if (family.empty()) return b;

    float stockHp = 0.0f;
    if (const uint8_t *sim = LoadSim(Str(rec, kRecSim, 50))) {
        float c[kCurvePoints];
        memcpy(c, sim + kSimCurve, sizeof c);
        stockHp = CurvePeakHp(c);
    }
    std::vector<std::pair<Key, const uint8_t *>> chosen;
    auto slotFor = [&](const Key &k) -> const uint8_t ** {
        for (auto &c : chosen) if (c.first == k) return &c.second;
        chosen.push_back({k, nullptr});
        return &chosen.back().second;
    };
    for (int off = kRecCatalog; off + 2 <= kRecCatalogEnd; off += 2) {
        const int id = Rd<uint16_t>(rec, off);
        if (id == 0) break;
        const uint8_t *p = FindPart(id);
        if (!p || Str(p, kPrtFamily, 30) != family) continue;
        const Key k = KeyOf(p);
        if (k.type <= 0 || k.type == 10) continue;
        if (k.type == 1) {
            const float hp = PartPeakHp(p);
            if (k.name != "Engine" && stockHp > 0.0f && hp > 2.0f * stockHp) continue;  // nfs5.prt 565
            const uint8_t **s = slotFor(k);
            if (!*s || hp > PartPeakHp(*s)) *s = p;
        } else if (k.type == 7) {
            const uint8_t **s = slotFor(k);
            if (!*s || Rd<float>(p, kPrtWeight) < Rd<float>(*s, kPrtWeight)) *s = p;
        } else {
            const uint8_t **s = slotFor(k);
            if (!*s || Rd<int>(p, kPrtPrice) > Rd<int>(*s, kPrtPrice) ||
                (Rd<int>(p, kPrtPrice) == Rd<int>(*s, kPrtPrice) && Rd<int16_t>(p, kPrtId) > Rd<int16_t>(*s, kPrtId)))
                *s = p;
        }
    }
    // An "Engine" part replaces the whole curve (0049CB50): only if it beats the stock engine, and installed first so the add-ons stack on top.
    std::stable_partition(chosen.begin(), chosen.end(), [](const std::pair<Key, const uint8_t *> &c) { return c.first.name == "Engine"; });
    if (!chosen.empty() && chosen.front().first.name == "Engine" && PartPeakHp(chosen.front().second) <= stockHp)
        chosen.erase(chosen.begin());
    // Slot budget: stock categories are replaced in place, new ones append.
    int stockKeys = 0;
    std::vector<Key> seen;
    for (const uint8_t *p : stock) {
        const Key k = KeyOf(p);
        if (std::find(seen.begin(), seen.end(), k) == seen.end()) { seen.push_back(k); ++stockKeys; }
    }
    for (;;) {
        int fresh = 0;
        for (auto &c : chosen) if (std::find(seen.begin(), seen.end(), c.first) == seen.end()) ++fresh;
        if (stockKeys + fresh <= kMaxParts) break;
        int drop = -1;
        for (int i = 0; i < (int)chosen.size(); ++i) {
            if (std::find(seen.begin(), seen.end(), chosen[i].first) != seen.end()) continue;
            if (drop < 0 || KeepPriority(chosen[i].first) < KeepPriority(chosen[drop].first)) drop = i;
        }
        if (drop < 0) break;
        chosen.erase(chosen.begin() + drop);
    }
    for (auto &c : chosen) b.push_back(c.second);
    return b;
}

struct Perf { float hp, index; };
Perf OppPerf(const uint8_t *sim, const std::vector<const uint8_t *> &parts, bool noParts) {
    Perf r = {0.0f, 0.0f};
    if (!sim) return r;
    float curve[kCurvePoints];
    memcpy(curve, sim + kSimCurve, sizeof curve);
    float mass = Rd<float>(sim, kSimMass);
    const int gears = (int)Rd<uint32_t>(sim, kSimGearCount);
    float top = (gears >= 1 && gears <= 6) ? Rd<float>(sim, kSimGears + 4 * (gears - 1)) : 0.0f;
    float final = Rd<float>(sim, kSimFinal);
    if (!noParts) {
        const int count = (int)std::min<size_t>(parts.size(), kMaxParts);
        for (int i = 0; i < count; ++i) {
            const uint8_t *p = parts[i];
            if (!p) continue;
            const int16_t type = Rd<int16_t>(p, kPrtType);
            if (type == 0) continue;
            mass += Rd<float>(p, kPrtWeight);
            const std::string name = Str(p, kPrtName, 30);
            if (type == 1) {
                float add[kCurvePoints];
                memcpy(add, p + kPrtCurve, sizeof add);
                if (name == "Engine") memcpy(curve, add, sizeof curve);
                else for (int k = 0; k < kCurvePoints; ++k) curve[k] += add[k] * kAddScale;
            } else if (type == 3 && name == "Gear Box") {
                const int n = Rd<int16_t>(p, kPrtGearCount);
                if (n >= 1 && n <= 6) top = Rd<float>(p, kPrtGears + 4 * (n - 1));
                final = Rd<float>(p, kPrtFinal);
            }
        }
    }
    const float width = Rd<float>(sim, kSimTyre), aspect = Rd<float>(sim, kSimTyre + 4), rim = Rd<float>(sim, kSimTyre + 8);
    const float circumference = (rim * 25.4f + aspect * 0.02f * width) * 0.0031416f;
    const float redline = Rd<float>(sim, kSimRedline);
    r.hp = CurvePeakHp(curve);
    if (mass <= 1.0f || top <= 0.0f || final <= 0.0f || redline <= 0.0f || circumference <= 0.0f) return r;
    const float vmaxKmh = redline / 60.0f * circumference / (top * final) * 3.6f;
    const float perTonne = 1000.0f * r.hp / mass;
    if (perTonne > 0.0f) r.index = sqrtf(perTonne * vmaxKmh);
    return r;
}

struct CarPerf {
    bool valid = false;
    int id = 0;
    std::string name;
    float stockIx = 0.0f, tunedIx = 0.0f;
    std::vector<int16_t> build;
};
std::map<const uint8_t *, CarPerf> g_perf;
void ClearPerf() { g_perf.clear(); }
float IndexOf(const uint8_t *rec, const std::vector<const uint8_t *> &parts) {
    return OppPerf(LoadSim(Str(rec, kRecSim, 50)), parts, NoPartsFamily(Str(rec, kRecAi, 50))).index;
}
const CarPerf &PerfFor(const uint8_t *rec) {
    auto it = g_perf.find(rec);
    if (it != g_perf.end() && it->second.id == Rd<uint16_t>(rec, kRecId)) return it->second;
    CarPerf cp;
    cp.id = Rd<uint16_t>(rec, kRecId);
    cp.name = Str(rec, kRecName, 50);
    const std::vector<const uint8_t *> stock = StockParts(rec);
    cp.stockIx = IndexOf(rec, stock);
    std::vector<const uint8_t *> tuned = stock;
    for (const uint8_t *p : FullBuild(rec, stock)) {
        SimulateInstall(tuned, p);
        cp.build.push_back(Rd<int16_t>(p, kPrtId));
    }
    cp.tunedIx = IndexOf(rec, tuned);
    if (cp.tunedIx < cp.stockIx) cp.tunedIx = cp.stockIx;
    cp.valid = cp.stockIx > 0.0f;
    return g_perf[rec] = cp;
}
int Pick(const float *values, int count, float player, int below, int above, int *out, int cap) {
    std::vector<int> lo, hi;
    for (int i = 0; i < count; ++i) (values[i] < player ? lo : hi).push_back(i);
    std::stable_sort(lo.begin(), lo.end(), [&](int a, int b) { return values[a] > values[b]; });
    std::stable_sort(hi.begin(), hi.end(), [&](int a, int b) { return values[a] < values[b]; });
    int nb = below, na = above;
    if ((int)hi.size() < na) { nb += na - (int)hi.size(); na = (int)hi.size(); }
    if ((int)lo.size() < nb) { na += nb - (int)lo.size(); nb = (int)lo.size(); }
    na = std::min(na, (int)hi.size());
    int n = 0;
    for (int i = nb - 1; i >= 0 && n < cap; --i) out[n++] = lo[i];
    for (int i = 0; i < na && n < cap; ++i) out[n++] = hi[i];
    return n;
}
} // namespace

extern "C" int opp_similar_run(const OppGame *g, int mode) {
    g_simDir = g->sim_dir;
    RefreshParts(g);
    if (g_partById.empty()) { fprintf(stderr, "[os1] part table not loaded\n"); return 0; }
    const uint8_t *playerRec = g->player_rec;
    std::vector<const uint8_t *> playerParts;
    if (g->owned_obj) {
        const uint32_t vec = Rd<uint32_t>(g->owned_obj, 0x758);
        if (vec) {
            const uint32_t b = Rd<uint32_t>(g->mem + vec, 0), e = Rd<uint32_t>(g->mem + vec, 4);
            if (b && e >= b && (e - b) / 4 <= 64)
                for (uint32_t a = b; a != e; a += 4)
                    if (const uint32_t part = Rd<uint32_t>(g->mem + a, 0)) playerParts.push_back(g->mem + part);
        }
    } else {
        playerParts = StockParts(playerRec);
    }
    const float playerIx = IndexOf(playerRec, playerParts);
    if (playerIx <= 0.0f) { fprintf(stderr, "[os1] no performance data for the player's car\n"); return 0; }

    const int playerId = Rd<uint16_t>(playerRec, kRecId);
    const std::string playerName = Str(playerRec, kRecName, 50);
    std::vector<const uint8_t *> recs;
    std::vector<float> stockValues, tunedValues;
    std::vector<std::string> names;
    for (int i = 0; i < g->list_size; ++i) {
        if (!g->list[i]) continue;
        const uint8_t *rec = g->mem + g->list[i];
        if (Rd<int16_t>(rec, kRecKind) != 0) continue;     // traffic and police cars are not opponents
        const CarPerf &cp = PerfFor(rec);
        if (!cp.valid || cp.id == playerId || cp.name == playerName) continue;
        if (std::find(names.begin(), names.end(), cp.name) != names.end()) continue;
        names.push_back(cp.name);
        recs.push_back(rec);
        stockValues.push_back(cp.stockIx);
        tunedValues.push_back(cp.tunedIx);
    }
    // One candidate = a car plus how it will be built. The mixed mode draws from both lists, so the same car can qualify stock or tuned.
    struct Candidate { int rec; bool tuned; };
    std::vector<Candidate> order;
    int picks[32];
    for (int pass = 0; pass < 2; ++pass) {
        const bool tuned = pass != 0;
        if (mode == (tuned ? OPP_MODE_STOCK : OPP_MODE_TUNED)) continue;
        const std::vector<float> &values = tuned ? tunedValues : stockValues;
        const int n = Pick(values.data(), (int)values.size(), playerIx, kBelow, kAbove, picks, 32);
        for (int i = 0; i < n; ++i) order.push_back({picks[i], tuned});
    }
    if (order.empty()) { fprintf(stderr, "[os1] no candidate matched the player's index\n"); return 0; }
    for (int i = (int)order.size() - 1; i > 0; --i) std::swap(order[i], order[RandBelow(i + 1)]);
    if (mode == OPP_MODE_MIXED) {                      // one entry per car, either build
        std::vector<Candidate> unique;
        for (const Candidate &c : order) {
            bool seen = false;
            for (const Candidate &u : unique) seen = seen || u.rec == c.rec;
            if (!seen) unique.push_back(c);
        }
        order.swap(unique);
    }
    const int n = (int)order.size();
    fprintf(stderr, "[os1] mode %s, player '%s' index %.0f (%d parts), %d candidates, %d picked\n",
            kModeNames[mode], playerName.c_str(), playerIx, (int)playerParts.size(), (int)recs.size(), n);
    for (int slot = 0; slot < kSlots; ++slot) {
        const Candidate pick = slot < n ? order[slot] : order[RandBelow(n)];
        const uint8_t *rec = recs[pick.rec];
        const CarPerf &cp = PerfFor(rec);
        uint32_t ext = 0xFFFF0000u, trim = 0xFFFF0000u, extra = 0xFFFF0000u, stripe = 0xFFFFFFFFu;
        const int colours = Rd<int16_t>(rec, kRecColours);
        if (colours > 0) {
            const int k = RandBelow(colours) * 16;
            ext   = __builtin_bswap32(Rd<uint32_t>(rec, kRecColourTable + k));
            trim  = __builtin_bswap32(Rd<uint32_t>(rec, kRecColourTable + k + 4));
            extra = __builtin_bswap32(Rd<uint32_t>(rec, kRecColourTable + k + 8));
        }
        const int stripes = Rd<int16_t>(rec, kRecStripes);
        if (stripes > 0) stripe = __builtin_bswap32(Rd<uint32_t>(rec, kRecStripeTable + RandBelow(stripes) * 16));
        int package = 0, number = -1;          // stock look: no stripes, no number
        if (pick.tuned) {
            package = g->packages > 0 && g->packages < 64 ? RandBelow(g->packages) : 0;
            if (RandBelow(2) == 0) number = 1 + RandBelow(99);
        }
        g->set_slot(g->ctx, slot, rec, ext, stripe, trim, extra, package, number);
        if (pick.tuned && !cp.build.empty()) g->install(g->ctx, slot, cp.build.data(), (int)cp.build.size());
        fprintf(stderr, "[os1]   slot %d '%s' %s index %.0f%s\n", slot, cp.name.c_str(), pick.tuned ? "tuned" : "stock",
                pick.tuned ? cp.tunedIx : cp.stockIx, slot < n ? "" : " repeat");
    }
    return 1;
}
