#pragma once

#include "types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace core {

// §1 — Lamport clock (one per list).
class LamportClock {
public:
    explicit LamportClock(int64_t initial = 0) : clock_(initial) {}

    // Local modification: increment and return the new value.
    int64_t tick() { return ++clock_; }

    // Remote observation: advance clock to at least seen.
    void observe(int64_t seen) {
        if (seen > clock_) clock_ = seen;
    }

    int64_t value() const { return clock_; }

private:
    int64_t clock_ = 0;
};

// §2.3 — Merge a single remote item into a local item.
// Returns true if any field changed.
bool mergeItem(Item& local, const Item& remote);

// §2.3 — Merge a vector of remote items into the local map.
// Unknown items are inserted as-is.
// Returns the list of itemIds that were added or modified.
std::vector<std::string> mergeItems(std::map<std::string, Item>& local,
                                    const std::vector<Item>& remote);

// §2.3 — Merge list title.
// Returns true if title changed.
bool mergeTitle(ListMeta& local, const std::string& remoteTitle, const Ver& remoteTitleVer);

// Merge le mode de classement (LWW), même règle que le titre.
// Returns true if sortMode changed.
bool mergeSortMode(ListMeta& local, const std::string& remoteMode, const Ver& remoteVer);

// §2.3 — Merge a single members entry.
// Returns true if the entry was inserted or updated.
bool mergeMember(std::map<std::string, std::pair<std::string, Ver>>& members,
                 const std::string& deviceId,
                 const std::string& displayName,
                 const Ver& ver);

// §2.4 — Returns true iff item is eligible for tombstone GC (del=true, lamport age >= 10000, wall age > 30 days).
bool gcEligible(const Item& item, int64_t currentLamport, int64_t touchedMs, int64_t nowMs);

} // namespace core
