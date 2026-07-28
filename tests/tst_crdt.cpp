// tst_crdt.cpp — Exhaustive CRDT invariant tests (SPEC §9)
// Pure C++ / STL, no Qt dependency.

#include "core/crdt.h"
#include "core/types.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace core;

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int g_total  = 0;
static int g_passed = 0;
static int g_failed = 0;

#define EXPECT_TRUE(expr)                                               \
    do {                                                                \
        ++g_total;                                                      \
        if (!(expr)) {                                                  \
            ++g_failed;                                                 \
            std::fprintf(stderr, "FAIL  %s:%d  %s\n",                  \
                         __FILE__, __LINE__, #expr);                    \
        } else {                                                        \
            ++g_passed;                                                 \
        }                                                               \
    } while (0)

#define EXPECT_EQ(a, b)  EXPECT_TRUE((a) == (b))
#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Ver makeVer(int64_t l, const std::string& dev) {
    Ver v;
    v.lamport  = l;
    v.deviceId = dev;
    return v;
}

static Item makeItem(const std::string& listId,
                     const std::string& itemId,
                     int64_t            created,
                     const std::string& by,
                     const std::string& name,
                     const Ver&         nameVer,
                     const std::string& qty  = "",
                     const Ver&         qtyVer  = {},
                     bool               done = false,
                     const Ver&         doneVer = {},
                     bool               del  = false,
                     const Ver&         delVer  = {}) {
    Item i;
    i.listId  = listId;
    i.itemId  = itemId;
    i.created = created;
    i.by      = by;
    i.name    = name;
    i.nameVer = nameVer;
    i.qty     = qty;
    i.qtyVer  = qtyVer;
    i.done    = done;
    i.doneVer = doneVer;
    i.del     = del;
    i.delVer  = delVer;
    return i;
}

// Deep equality of two Item maps.
static bool mapsEqual(const std::map<std::string, Item>& a,
                      const std::map<std::string, Item>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [id, ia] : a) {
        auto it = b.find(id);
        if (it == b.end()) return false;
        const Item& ib = it->second;
        if (ia.listId   != ib.listId)   return false;
        if (ia.itemId   != ib.itemId)   return false;
        if (ia.created  != ib.created)  return false;
        if (ia.by       != ib.by)       return false;
        if (ia.name     != ib.name)     return false;
        if (ia.nameVer  != ib.nameVer)  return false;
        if (ia.qty      != ib.qty)      return false;
        if (ia.qtyVer   != ib.qtyVer)   return false;
        if (ia.done     != ib.done)     return false;
        if (ia.doneVer  != ib.doneVer)  return false;
        if (ia.del      != ib.del)      return false;
        if (ia.delVer   != ib.delVer)   return false;
    }
    return true;
}

// Converge N replicas: for each pair, merge each into the other.
// Repeat until stable (handles multi-hop).
static void fullMesh(std::vector<std::map<std::string, Item>>& replicas) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < replicas.size(); ++i) {
            for (size_t j = 0; j < replicas.size(); ++j) {
                if (i == j) continue;
                std::vector<Item> remote;
                for (const auto& [id, item] : replicas[j]) remote.push_back(item);
                auto c = mergeItems(replicas[i], remote);
                if (!c.empty()) changed = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// §9 Invariant 1 — Commutativity, associativity, idempotency via fuzzing
// ---------------------------------------------------------------------------

// Replica state for fuzzing.
struct Replica {
    std::string                id;   // deviceId
    std::map<std::string, Item> items;
    LamportClock               clock;
};

// Symbolic operations.
enum class OpType { Create, EditName, EditQty, EditDone, Delete };

struct Op {
    OpType      type;
    std::string itemId;   // target (or new) item UUID
    std::string value;    // for string fields
    bool        boolVal;  // for done/del
};

static std::string genId(std::mt19937_64& rng) {
    uint64_t a = rng(), b = rng();
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08llx-%04llx-%04llx-%04llx-%012llx",
                  (unsigned long long)(a >> 32),
                  (unsigned long long)((a >> 16) & 0xffff),
                  (unsigned long long)(a & 0xffff),
                  (unsigned long long)((b >> 48) & 0xffff),
                  (unsigned long long)(b & 0xffffffffffff));
    return buf;
}

static void applyOp(Replica& r, const Op& op) {
    const std::string& listId = "fuzz-list";
    int64_t t = r.clock.tick();
    Ver ver   = makeVer(t, r.id);

    if (op.type == OpType::Create) {
        if (r.items.find(op.itemId) != r.items.end()) return; // already exists
        Item item;
        item.listId  = listId;
        item.itemId  = op.itemId;
        // created and by must be deterministic per itemId so that any replica
        // independently creating the same item gets the same immutable fields.
        // We derive them from the itemId string itself.
        item.created = static_cast<int64_t>(std::hash<std::string>{}(op.itemId) & 0x7FFFFFFFFFFFFFFFULL);
        item.by      = "creator-of-" + op.itemId;
        item.name    = op.value.empty() ? "item" : op.value;
        item.nameVer = ver;
        r.items[op.itemId] = item;
    } else {
        auto it = r.items.find(op.itemId);
        if (it == r.items.end()) return; // item doesn't exist yet on this replica
        Item& item = it->second;
        switch (op.type) {
            case OpType::EditName:
                item.name    = op.value;
                item.nameVer = ver;
                break;
            case OpType::EditQty:
                item.qty    = op.value;
                item.qtyVer = ver;
                break;
            case OpType::EditDone:
                item.done    = op.boolVal;
                item.doneVer = ver;
                break;
            case OpType::Delete:
                item.del    = op.boolVal;
                item.delVer = ver;
                break;
            default: break;
        }
    }
}

static void runFuzzIteration(uint64_t seed, bool logSeed = false) {
    if (logSeed) {
        std::printf("[crdt_fuzz] seed=%" PRIu64 "\n", seed);
    }

    std::mt19937_64 rng(seed);

    // 3 replicas
    const int N = 3;
    std::vector<Replica> replicas(N);
    for (int i = 0; i < N; ++i) {
        replicas[i].id = "dev" + std::to_string(i);
    }

    // Pre-generate a pool of item IDs so operations can refer to existing items.
    const int POOL = 8;
    std::vector<std::string> itemPool;
    for (int i = 0; i < POOL; ++i) itemPool.push_back(genId(rng));

    // Generate >= 500 operations, assigned to replicas randomly.
    const int NOPS = 500;
    std::vector<std::pair<int, Op>> ops; // (replicaIndex, op)

    std::uniform_int_distribution<int> repDist(0, N - 1);
    std::uniform_int_distribution<int> poolDist(0, POOL - 1);
    std::uniform_int_distribution<int> opDist(0, 4);

    for (int k = 0; k < NOPS; ++k) {
        Op op;
        int repIdx  = repDist(rng);
        int opType  = opDist(rng);
        op.itemId   = itemPool[poolDist(rng)];
        op.boolVal  = (rng() & 1) != 0;
        op.value    = "val" + std::to_string(rng() % 100);

        switch (opType) {
            case 0: op.type = OpType::Create;   break;
            case 1: op.type = OpType::EditName; break;
            case 2: op.type = OpType::EditQty;  break;
            case 3: op.type = OpType::EditDone; break;
            case 4: op.type = OpType::Delete;   break;
        }
        ops.push_back({repIdx, op});
        applyOp(replicas[repIdx], op);
    }

    // Simulate random delivery: for each op, deliver to a random subset of replicas
    // in a random order (possibly duplicated).
    // We materialise the item state at each op as a delta, then deliver.
    // Simpler: just do full mesh convergence N times with random intermediate states.

    // Partial sync: each replica sends its state to one random other, repeat 5 times.
    std::uniform_int_distribution<int> rp2(0, N - 1);
    for (int round = 0; round < 5; ++round) {
        int src = rp2(rng);
        int dst = rp2(rng);
        if (src == dst) dst = (dst + 1) % N;
        std::vector<Item> remote;
        for (const auto& [id, item] : replicas[src].items) remote.push_back(item);
        mergeItems(replicas[dst].items, remote);
    }

    // Full mesh: exchange everything.
    std::vector<std::map<std::string, Item>> maps(N);
    for (int i = 0; i < N; ++i) maps[i] = replicas[i].items;
    fullMesh(maps);

    // All replicas must now be identical.
    for (int i = 1; i < N; ++i) {
        if (!mapsEqual(maps[0], maps[i])) {
            std::fprintf(stderr,
                "FAIL [fuzz seed=%" PRIu64 "] replica 0 and %d diverged after full mesh\n",
                seed, i);
            EXPECT_TRUE(false);
            return;
        }
    }

    // Idempotency: re-merging the same state changes nothing.
    std::vector<Item> all;
    for (const auto& [id, item] : maps[0]) all.push_back(item);
    auto changed = mergeItems(maps[0], all);
    EXPECT_TRUE(changed.empty());

    // Commutativity: merge(A,B) == merge(B,A) for first two replicas.
    {
        std::map<std::string, Item> ab = replicas[0].items;
        std::vector<Item> bItems;
        for (const auto& [id, item] : replicas[1].items) bItems.push_back(item);
        mergeItems(ab, bItems);

        std::map<std::string, Item> ba = replicas[1].items;
        std::vector<Item> aItems;
        for (const auto& [id, item] : replicas[0].items) aItems.push_back(item);
        mergeItems(ba, aItems);

        // Both should match maps[0]/[1] after full mesh, so just compare ab == ba.
        if (!mapsEqual(ab, ba)) {
            std::fprintf(stderr,
                "FAIL [fuzz seed=%" PRIu64 "] commutativity violated\n", seed);
            EXPECT_TRUE(false);
        }
    }

    EXPECT_TRUE(true); // iteration passed
}

// ---------------------------------------------------------------------------
// Directed tests
// ---------------------------------------------------------------------------

static void test_LamportClock() {
    LamportClock c;
    EXPECT_EQ(c.value(), 0);
    EXPECT_EQ(c.tick(), 1);
    EXPECT_EQ(c.tick(), 2);
    c.observe(10);
    EXPECT_EQ(c.value(), 10);
    EXPECT_EQ(c.tick(), 11);
    c.observe(5);   // no-op
    EXPECT_EQ(c.value(), 11);
    c.observe(100);
    EXPECT_EQ(c.value(), 100);
}

// §9 invariant 2 — same-field conflict: highest ver wins
static void test_SameFieldConflict() {
    Item local = makeItem("l","i1",1,"devA","Lait",makeVer(5,"devA"));
    Item remote = makeItem("l","i1",1,"devA","Milk",makeVer(10,"devB"));

    bool changed = mergeItem(local, remote);
    EXPECT_TRUE(changed);
    EXPECT_EQ(local.name, "Milk");
    EXPECT_EQ(local.nameVer.lamport, 10);

    // Lower ver: ignored
    Item remote2 = makeItem("l","i1",1,"devA","Chocolat",makeVer(3,"devA"));
    changed = mergeItem(local, remote2);
    EXPECT_FALSE(changed);
    EXPECT_EQ(local.name, "Milk");

    // Equal lamport: deviceId tiebreak (lexicographic).
    // "devC" > "devB" → wins
    Item remote3 = makeItem("l","i1",1,"devA","Beurre",makeVer(10,"devC"));
    changed = mergeItem(local, remote3);
    EXPECT_TRUE(changed);
    EXPECT_EQ(local.name, "Beurre");

    // "devA" < "devC" → loses
    Item remote4 = makeItem("l","i1",1,"devA","Fromage",makeVer(10,"devA"));
    changed = mergeItem(local, remote4);
    EXPECT_FALSE(changed);
    EXPECT_EQ(local.name, "Beurre");
}

// §9 invariant 3 — different fields: both survive
static void test_DifferentFieldsConflict() {
    Item base = makeItem("l","i1",1,"devA","Lait",makeVer(1,"devA"),
                         "1L",makeVer(1,"devA"));

    // Device A edits name; device B edits qty concurrently (same lamport, different devs)
    Item editA = base;
    editA.name    = "Lait entier";
    editA.nameVer = makeVer(5, "devA");

    Item editB = base;
    editB.qty    = "2L";
    editB.qtyVer = makeVer(5, "devB");

    // Start from editA, merge editB
    Item merged = editA;
    mergeItem(merged, editB);
    EXPECT_EQ(merged.name, "Lait entier");
    EXPECT_EQ(merged.qty,  "2L");

    // Start from editB, merge editA → same result
    Item merged2 = editB;
    mergeItem(merged2, editA);
    EXPECT_EQ(merged2.name, "Lait entier");
    EXPECT_EQ(merged2.qty,  "2L");
}

// §9 invariant 4 — del concurrent with done: both fields apply independently
static void test_DelVsDoneConcurrent() {
    Item base = makeItem("l","i1",1,"devA","Lait",makeVer(1,"devA"),
                         "",{},false,makeVer(1,"devA"),false,makeVer(1,"devA"));

    // devA marks done=true at lamport 5
    Item itemA = base;
    itemA.done    = true;
    itemA.doneVer = makeVer(5, "devA");

    // devB marks del=true at lamport 5 (same lamport, devB wins over devA for del field)
    Item itemB = base;
    itemB.del    = true;
    itemB.delVer = makeVer(5, "devB");

    // Merge A into B's copy
    Item merged = itemA;
    mergeItem(merged, itemB);
    EXPECT_TRUE(merged.done);   // done from devA
    EXPECT_TRUE(merged.del);    // del from devB
    EXPECT_EQ(merged.doneVer.deviceId, "devA");
    EXPECT_EQ(merged.delVer.deviceId,  "devB");

    // Commutativity
    Item merged2 = itemB;
    mergeItem(merged2, itemA);
    EXPECT_TRUE(merged2.done);
    EXPECT_TRUE(merged2.del);
}

// del=false after del=true → resurrection
static void test_Resurrection() {
    Item item = makeItem("l","i1",1,"devA","Lait",makeVer(1,"devA"),
                         "",{},false,{},true,makeVer(5,"devA"));

    // Later: del=false (undo)
    Item undo = item;
    undo.del    = false;
    undo.delVer = makeVer(10, "devA");

    mergeItem(item, undo);
    EXPECT_FALSE(item.del);
    EXPECT_EQ(item.delVer.lamport, 10);
}

// Unknown item on remote → inserted as-is
static void test_UnknownItemInserted() {
    std::map<std::string, Item> local;
    Item rem = makeItem("l","i-new",42,"devX","Beurre",makeVer(3,"devX"));

    auto changed = mergeItems(local, {rem});
    EXPECT_EQ(changed.size(), size_t(1));
    EXPECT_EQ(local.size(), size_t(1));
    EXPECT_EQ(local["i-new"].name, "Beurre");
    EXPECT_EQ(local["i-new"].created, int64_t(42));
    EXPECT_EQ(local["i-new"].by, "devX");
}

// Duplicate delivery → idempotent
static void test_IdempotentMerge() {
    std::map<std::string, Item> local;
    Item item = makeItem("l","i1",1,"devA","Lait",makeVer(5,"devA"));
    mergeItems(local, {item});
    auto changed = mergeItems(local, {item}); // second delivery
    EXPECT_TRUE(changed.empty());
}

// §9 invariant 5 — item created offline on A appears on B after flush, exactly once
static void test_OfflineItemAppears() {
    std::map<std::string, Item> mapA, mapB;
    Item item = makeItem("l","i-offline",100,"devA","Offline-Item",makeVer(1,"devA"));
    mapA["i-offline"] = item;

    // Flush A → B
    std::vector<Item> flush;
    for (const auto& [id, it] : mapA) flush.push_back(it);
    auto changed = mergeItems(mapB, flush);

    EXPECT_EQ(changed.size(), size_t(1));
    EXPECT_EQ(mapB["i-offline"].name, "Offline-Item");

    // Second flush: no change
    auto changed2 = mergeItems(mapB, flush);
    EXPECT_TRUE(changed2.empty());
}

// §9 invariant 6 — late-joining replica converges via replay
static void test_LateJoinConvergence() {
    std::map<std::string, Item> early1, early2, late;

    Item i1 = makeItem("l","i1",1,"devA","Lait",   makeVer(1,"devA"));
    Item i2 = makeItem("l","i2",2,"devB","Beurre",  makeVer(2,"devB"));
    Item i3 = makeItem("l","i3",3,"devA","Fromage", makeVer(3,"devA"));

    early1["i1"] = i1;
    early1["i2"] = i2;
    early2["i2"] = i2;
    early2["i3"] = i3;

    // Full mesh early1 ↔ early2
    std::vector<std::map<std::string, Item>> maps = {early1, early2};
    fullMesh(maps);

    // Late replica receives full history (snap from early1 + deltas from early2)
    std::vector<Item> snap;
    for (const auto& [id, it] : maps[0]) snap.push_back(it);
    mergeItems(late, snap);

    EXPECT_TRUE(mapsEqual(late, maps[0]));
    EXPECT_TRUE(mapsEqual(late, maps[1]));
}

// GC eligibility — SPEC §2.4: del=true AND lamport age >= 10000 AND wall age > 30 days
static void test_GcEligibility() {
    constexpr int64_t THIRTY_DAYS_MS = 30LL * 24 * 3600 * 1000;
    // Item deleted at lamport=1, touched at t=0.
    Item item = makeItem("l","i1",0,"devA","X",makeVer(1,"devA"),
                         "",{},false,{},true,makeVer(1,"devA"));

    // Both conditions met: lamport age >= 10000 AND wall age > 30 days → true
    int64_t nowOld = THIRTY_DAYS_MS + 1;
    EXPECT_TRUE(gcEligible(item, 10001, /*touchedMs=*/0, nowOld));

    // Lamport too recent (age < 10000) → false, even if wall age is OK
    EXPECT_FALSE(gcEligible(item, 5000, /*touchedMs=*/0, nowOld));

    // Wall clock too recent (age <= 30 days) → false, even if lamport age is OK
    int64_t nowRecent = THIRTY_DAYS_MS; // exactly 30 days, not strictly greater
    EXPECT_FALSE(gcEligible(item, 10001, /*touchedMs=*/0, nowRecent));

    // del=false → never eligible, regardless of ages
    Item notDeleted = makeItem("l","i2",0,"devA","Y",makeVer(1,"devA"),
                               "",{},false,{},false,makeVer(1,"devA"));
    EXPECT_FALSE(gcEligible(notDeleted, 10001, /*touchedMs=*/0, nowOld));
}

// mergeTitle
static void test_MergeTitle() {
    ListMeta meta;
    meta.title    = "Old";
    meta.titleVer = makeVer(1, "devA");

    EXPECT_TRUE(mergeTitle(meta, "New", makeVer(2, "devA")));
    EXPECT_EQ(meta.title, "New");

    EXPECT_FALSE(mergeTitle(meta, "Stale", makeVer(1, "devA")));
    EXPECT_EQ(meta.title, "New");
}

// mergeSortMode — même règle LWW que le titre.
static void test_MergeSortMode() {
    ListMeta meta;   // défaut : "" en version {0,""}
    // Un premier choix (lamport 1) bat le défaut.
    EXPECT_TRUE(mergeSortMode(meta, "manual", makeVer(1, "devA")));
    EXPECT_EQ(meta.sortMode, "manual");
    // Une version plus récente gagne (retour au tri par rayon).
    EXPECT_TRUE(mergeSortMode(meta, "aisle", makeVer(2, "devB")));
    EXPECT_EQ(meta.sortMode, "aisle");
    // Une version périmée est ignorée.
    EXPECT_FALSE(mergeSortMode(meta, "manual", makeVer(1, "devA")));
    EXPECT_EQ(meta.sortMode, "aisle");
}

// mergeMember
// La description suit la même règle LWW que les autres champs, indépendamment d'eux.
static void test_MergeNote() {
    Item local;
    local.itemId  = "i1";
    local.name    = "Pq";
    local.nameVer = makeVer(3, "devA");
    local.note    = "";
    local.noteVer = makeVer(0, "");

    Item remote = local;
    remote.note    = "6 couches épaisses";
    remote.noteVer = makeVer(4, "devB");

    EXPECT_TRUE(mergeItem(local, remote));
    EXPECT_EQ(local.note, "6 couches épaisses");
    // Le nom n'était pas concerné : sa version ne bouge pas.
    EXPECT_EQ(local.nameVer.lamport, 3);

    // Une note plus ancienne ne réécrit pas la plus récente.
    Item stale = local;
    stale.note    = "n'importe quoi";
    stale.noteVer = makeVer(2, "devC");
    EXPECT_FALSE(mergeItem(local, stale));
    EXPECT_EQ(local.note, "6 couches épaisses");

    // Un pair qui ignore le champ (version {0,""}) ne l'efface pas non plus.
    Item legacy = local;
    legacy.note    = "";
    legacy.noteVer = makeVer(0, "");
    EXPECT_FALSE(mergeItem(local, legacy));
    EXPECT_EQ(local.note, "6 couches épaisses");
}

// doneAt est un satellite de `done` : la date suit celui qui gagne le merge sur done.
static void test_MergeDoneAt() {
    Item local;
    local.itemId  = "i1";
    local.done    = false;
    local.doneVer = makeVer(2, "devA");
    local.doneAt  = 0;

    Item remote = local;
    remote.done    = true;
    remote.doneVer = makeVer(5, "devB");
    remote.doneAt  = 1'700'000'000'000;

    EXPECT_TRUE(mergeItem(local, remote));
    EXPECT_TRUE(local.done);
    EXPECT_EQ(local.doneAt, 1'700'000'000'000);

    // Décochage plus récent : done repasse à false, et la date de cochage disparaît.
    Item uncheck = local;
    uncheck.done    = false;
    uncheck.doneVer = makeVer(7, "devA");
    uncheck.doneAt  = 0;

    EXPECT_TRUE(mergeItem(local, uncheck));
    EXPECT_FALSE(local.done);
    EXPECT_EQ(local.doneAt, 0);
}

// Rayon et position sont des champs LWW comme les autres : deux personnes qui
// réordonnent en même temps convergent (le dernier gagne) au lieu de diverger.
static void test_MergeAisleAndOrder() {
    Item local;
    local.itemId   = "i1";
    local.aisle    = "";
    local.aisleVer = makeVer(0, "");
    local.order    = 1000;
    local.orderVer = makeVer(2, "devA");

    Item remote = local;
    remote.aisle    = "Crèmerie";
    remote.aisleVer = makeVer(5, "devB");
    remote.order    = 3000;
    remote.orderVer = makeVer(5, "devB");

    EXPECT_TRUE(mergeItem(local, remote));
    EXPECT_EQ(local.aisle, "Crèmerie");
    EXPECT_EQ(local.order, 3000);

    // Un déplacement plus ancien ne défait pas le plus récent.
    Item stale = local;
    stale.order    = 9999;
    stale.orderVer = makeVer(3, "devC");
    EXPECT_FALSE(mergeItem(local, stale));
    EXPECT_EQ(local.order, 3000);

    // Un pair qui ignore ces champs (version {0,""}) ne les efface pas.
    Item legacy = local;
    legacy.aisle    = "";
    legacy.aisleVer = makeVer(0, "");
    legacy.order    = 0;
    legacy.orderVer = makeVer(0, "");
    EXPECT_FALSE(mergeItem(local, legacy));
    EXPECT_EQ(local.aisle, "Crèmerie");
    EXPECT_EQ(local.order, 3000);
}

// La photo est un champ LWW comme les autres : la plus récente gagne, un pair qui
// ignore le champ (version {0,""}) ne l'efface pas, et parentId ne se merge jamais
// (immuable, fixé à l'insertion).
static void test_MergeImageAndParent() {
    Item local;
    local.itemId   = "i1";
    local.parentId = "task-1";
    local.image    = "";
    local.imageVer = makeVer(0, "");

    Item remote = local;
    remote.image    = "sha-recente";
    remote.imageVer = makeVer(5, "devB");

    EXPECT_TRUE(mergeItem(local, remote));
    EXPECT_EQ(local.image, "sha-recente");

    // Une photo plus ancienne ne défait pas la plus récente.
    Item stale = local;
    stale.image    = "sha-vieille";
    stale.imageVer = makeVer(3, "devC");
    EXPECT_FALSE(mergeItem(local, stale));
    EXPECT_EQ(local.image, "sha-recente");

    // Retirer la photo est une écriture comme une autre : "" avec version plus haute.
    Item removal = local;
    removal.image    = "";
    removal.imageVer = makeVer(8, "devA");
    EXPECT_TRUE(mergeItem(local, removal));
    EXPECT_EQ(local.image, "");

    // L'échéance suit la même règle LWW ; la retirer (0) est une écriture comme
    // une autre.
    Item withDue = local;
    withDue.due    = 1721000000000LL;
    withDue.dueVer = makeVer(9, "devB");
    EXPECT_TRUE(mergeItem(local, withDue));
    EXPECT_EQ(local.due, 1721000000000LL);
    Item staleDue = local;
    staleDue.due    = 42;
    staleDue.dueVer = makeVer(4, "devC");
    EXPECT_FALSE(mergeItem(local, staleDue));
    EXPECT_EQ(local.due, 1721000000000LL);

    // Un pair qui ignore le champ (version {0,""}) ne peut rien effacer.
    Item legacy = local;
    legacy.image    = "";
    legacy.imageVer = makeVer(0, "");
    legacy.due      = 0;
    legacy.dueVer   = makeVer(0, "");
    legacy.parentId = "";   // et un parentId divergent n'est jamais adopté
    EXPECT_FALSE(mergeItem(local, legacy));
    EXPECT_EQ(local.parentId, "task-1");
    EXPECT_EQ(local.due, 1721000000000LL);

    // Un item inconnu est inséré tel quel, parentId compris.
    std::map<std::string, Item> map;
    Item step;
    step.itemId   = "step-1";
    step.parentId = "task-9";
    step.name     = "Poncer";
    step.nameVer  = makeVer(1, "devA");
    auto changed = mergeItems(map, { step });
    EXPECT_EQ(changed.size(), size_t(1));
    EXPECT_EQ(map["step-1"].parentId, "task-9");
}

static void test_MergeMember() {
    std::map<std::string, std::pair<std::string, Ver>> members;

    EXPECT_TRUE(mergeMember(members, "devA", "Alice", makeVer(1, "devA")));
    EXPECT_EQ(members["devA"].first, "Alice");

    // Higher ver → update
    EXPECT_TRUE(mergeMember(members, "devA", "Alice2", makeVer(5, "devA")));
    EXPECT_EQ(members["devA"].first, "Alice2");

    // Lower ver → no change
    EXPECT_FALSE(mergeMember(members, "devA", "OldAlice", makeVer(2, "devA")));
    EXPECT_EQ(members["devA"].first, "Alice2");
}

// Associativity: merge(A,merge(B,C)) == merge(merge(A,B),C)
static void test_Associativity() {
    auto A = makeItem("l","i1",1,"devA","A", makeVer(3,"devA"),"",{},false,{},false,{});
    auto B = makeItem("l","i1",1,"devA","B", makeVer(7,"devB"),"",{},false,{},false,{});
    auto C = makeItem("l","i1",1,"devA","C", makeVer(5,"devC"),"",{},true, makeVer(5,"devC"),false,{});

    // merge(B,C)
    Item BC = B;
    mergeItem(BC, C);

    // merge(A, merge(B,C))
    Item A_BC = A;
    mergeItem(A_BC, BC);

    // merge(A,B)
    Item AB = A;
    mergeItem(AB, B);

    // merge(merge(A,B), C)
    Item AB_C = AB;
    mergeItem(AB_C, C);

    EXPECT_EQ(A_BC.name, AB_C.name);
    EXPECT_EQ(A_BC.nameVer.lamport, AB_C.nameVer.lamport);
    EXPECT_EQ(A_BC.done, AB_C.done);
    EXPECT_EQ(A_BC.doneVer.deviceId, AB_C.doneVer.deviceId);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== tst_crdt ===\n");

    test_LamportClock();
    test_SameFieldConflict();
    test_DifferentFieldsConflict();
    test_DelVsDoneConcurrent();
    test_Resurrection();
    test_UnknownItemInserted();
    test_IdempotentMerge();
    test_OfflineItemAppears();
    test_LateJoinConvergence();
    test_GcEligibility();
    test_MergeTitle();
    test_MergeSortMode();
    test_MergeNote();
    test_MergeDoneAt();
    test_MergeAisleAndOrder();
    test_MergeImageAndParent();
    test_MergeMember();
    test_Associativity();

    // Fixed-seed fuzz (reproducible CI)
    std::printf("Running 500-op fuzz (fixed seed)...\n");
    for (int i = 0; i < 20; ++i) {
        runFuzzIteration(0xDEAD'BEEF'0000 + i);
    }

    // Random seed (logged for reproduction)
    uint64_t randSeed = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::printf("Running fuzz with random seed...\n");
    runFuzzIteration(randSeed, /*logSeed=*/true);

    std::printf("\nResults: %d/%d passed, %d failed\n", g_passed, g_total, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
