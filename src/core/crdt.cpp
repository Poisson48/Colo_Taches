#include "crdt.h"

namespace core {

bool mergeItem(Item& local, const Item& remote) {
    bool changed = false;

    // Immutable fields: created and by are only set from the first seen item.
    // They are already set when the item exists locally. If local item is being
    // initialized (e.g., itemId just set), these are left as-is from insertion.

    // LWW per field: adopt remote value iff ver_remote > ver_local
    if (remote.nameVer > local.nameVer) {
        local.name    = remote.name;
        local.nameVer = remote.nameVer;
        changed = true;
    }
    if (remote.qtyVer > local.qtyVer) {
        local.qty    = remote.qty;
        local.qtyVer = remote.qtyVer;
        changed = true;
    }
    if (remote.noteVer > local.noteVer) {
        local.note    = remote.note;
        local.noteVer = remote.noteVer;
        changed = true;
    }
    if (remote.aisleVer > local.aisleVer) {
        local.aisle    = remote.aisle;
        local.aisleVer = remote.aisleVer;
        changed = true;
    }
    if (remote.imageVer > local.imageVer) {
        local.image    = remote.image;
        local.imageVer = remote.imageVer;
        changed = true;
    }
    if (remote.orderVer > local.orderVer) {
        local.order    = remote.order;
        local.orderVer = remote.orderVer;
        changed = true;
    }
    if (remote.doneVer > local.doneVer) {
        local.done    = remote.done;
        local.doneVer = remote.doneVer;
        local.doneAt  = remote.doneAt;
        changed = true;
    }
    if (remote.delVer > local.delVer) {
        local.del    = remote.del;
        local.delVer = remote.delVer;
        changed = true;
    }

    return changed;
}

std::vector<std::string> mergeItems(std::map<std::string, Item>& local,
                                    const std::vector<Item>& remote) {
    std::vector<std::string> changed;

    for (const Item& rem : remote) {
        auto it = local.find(rem.itemId);
        if (it == local.end()) {
            // Unknown item: insert as-is (preserving created and by).
            local[rem.itemId] = rem;
            changed.push_back(rem.itemId);
        } else {
            if (mergeItem(it->second, rem)) {
                changed.push_back(rem.itemId);
            }
        }
    }

    return changed;
}

bool mergeTitle(ListMeta& local,
                const std::string& remoteTitle,
                const Ver& remoteTitleVer) {
    if (remoteTitleVer > local.titleVer) {
        local.title    = remoteTitle;
        local.titleVer = remoteTitleVer;
        return true;
    }
    return false;
}

bool mergeSortMode(ListMeta& local,
                   const std::string& remoteMode,
                   const Ver& remoteVer) {
    if (remoteVer > local.sortModeVer) {
        local.sortMode    = remoteMode;
        local.sortModeVer = remoteVer;
        return true;
    }
    return false;
}

bool mergeMember(std::map<std::string, std::pair<std::string, Ver>>& members,
                 const std::string& deviceId,
                 const std::string& displayName,
                 const Ver& ver) {
    auto it = members.find(deviceId);
    if (it == members.end()) {
        members[deviceId] = {displayName, ver};
        return true;
    }
    if (ver > it->second.second) {
        it->second = {displayName, ver};
        return true;
    }
    return false;
}

// §2.4 — GC eligible: tombstone must be old enough in both Lamport and wall-clock time.
bool gcEligible(const Item& item, int64_t currentLamport, int64_t touchedMs, int64_t nowMs) {
    if (!item.del) return false;
    constexpr int64_t GC_LAMPORT_AGE   = 10000;
    constexpr int64_t THIRTY_DAYS_MS   = 30LL * 24 * 3600 * 1000;
    if (currentLamport - item.delVer.lamport < GC_LAMPORT_AGE) return false;
    if (nowMs - touchedMs <= THIRTY_DAYS_MS) return false;
    return true;
}

} // namespace core
