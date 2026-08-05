#include "payload.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

using json = nlohmann::json;

namespace core {

namespace {
// Base64 standard (RFC 4648, avec padding) — core est pur STL, pas de QByteArray.
const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64Encode(const std::vector<uint8_t>& data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kB64Alphabet[(n >> 18) & 63];
        out += kB64Alphabet[(n >> 12) & 63];
        out += kB64Alphabet[(n >> 6) & 63];
        out += kB64Alphabet[n & 63];
        i += 3;
    }
    const size_t rest = data.size() - i;
    if (rest == 1) {
        uint32_t n = data[i] << 16;
        out += kB64Alphabet[(n >> 18) & 63];
        out += kB64Alphabet[(n >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out += kB64Alphabet[(n >> 18) & 63];
        out += kB64Alphabet[(n >> 12) & 63];
        out += kB64Alphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

int b64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Retourne false si la chaîne n'est pas du base64 valide.
bool b64Decode(const std::string& in, std::vector<uint8_t>& out)
{
    out.clear();
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int v = b64Value(c);
        if (v < 0) return false;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return true;
}
} // anonymous namespace

// Helper: parse a versioned field array [value, [lamport, deviceId]].
// Returns false if the structure is malformed.
template <typename T>
static bool parseVersionedField(const json& arr, T& value, Ver& ver) {
    if (!arr.is_array() || arr.size() != 2) return false;
    const json& verArr = arr[1];
    if (!verArr.is_array() || verArr.size() != 2) return false;
    if (!verArr[0].is_number_integer()) return false;
    if (!verArr[1].is_string()) return false;

    try {
        if constexpr (std::is_same_v<T, std::string>) {
            if (!arr[0].is_string()) return false;
            value = arr[0].get<std::string>();
        } else if constexpr (std::is_same_v<T, bool>) {
            if (!arr[0].is_boolean()) return false;
            value = arr[0].get<bool>();
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (!arr[0].is_number_integer()) return false;
            value = arr[0].get<int64_t>();
        } else {
            return false;
        }
        ver.lamport  = verArr[0].get<int64_t>();
        ver.deviceId = verArr[1].get<std::string>();
    } catch (...) {
        return false;
    }
    return true;
}

std::optional<Payload> parsePayload(const std::string& jsonStr) {
    json j;
    try {
        j = json::parse(jsonStr);
    } catch (...) {
        return std::nullopt;
    }

    if (!j.is_object()) return std::nullopt;

    // "v" must be 1
    if (!j.contains("v") || j["v"] != 1) return std::nullopt;

    // "t" must be "delta", "snap", or "img"
    if (!j.contains("t") || !j["t"].is_string()) return std::nullopt;
    const std::string typeStr = j["t"].get<std::string>();
    Payload::Type type;
    if (typeStr == "delta") {
        type = Payload::Type::delta;
    } else if (typeStr == "snap") {
        type = Payload::Type::snap;
    } else if (typeStr == "img") {
        type = Payload::Type::image;
    } else {
        return std::nullopt;
    }

    // "list"
    if (!j.contains("list") || !j["list"].is_string()) return std::nullopt;
    std::string listId = j["list"].get<std::string>();

    Payload p;
    p.type   = type;
    p.listId = std::move(listId);

    // Événement img : un blob et son empreinte, rien d'autre à parser.
    if (type == Payload::Type::image) {
        if (!j.contains("sha") || !j["sha"].is_string()) return std::nullopt;
        if (!j.contains("data") || !j["data"].is_string()) return std::nullopt;
        p.imageSha = j["sha"].get<std::string>();
        if (!b64Decode(j["data"].get<std::string>(), p.imageData))
            return std::nullopt;
        if (j.contains("by") && j["by"].is_string())
            p.by = j["by"].get<std::string>();
        return p;
    }

    // Parse items array (individual malformed items are silently skipped)
    if (j.contains("items") && j["items"].is_array()) {
        for (const auto& jitem : j["items"]) {
            if (!jitem.is_object()) continue;

            Item item;

            // Required fields
            if (!jitem.contains("id") || !jitem["id"].is_string()) continue;
            item.itemId = jitem["id"].get<std::string>();
            item.listId = p.listId;

            if (jitem.contains("created") && jitem["created"].is_number_integer()) {
                item.created = jitem["created"].get<int64_t>();
            }
            if (jitem.contains("by") && jitem["by"].is_string()) {
                item.by = jitem["by"].get<std::string>();
            }
            // Absent des payloads antérieurs → "" = tâche de premier niveau.
            if (jitem.contains("parent") && jitem["parent"].is_string()) {
                item.parentId = jitem["parent"].get<std::string>();
            }
            if (jitem.contains("doneAt") && jitem["doneAt"].is_number_integer()) {
                item.doneAt = jitem["doneAt"].get<int64_t>();
            }

            if (!jitem.contains("f") || !jitem["f"].is_object()) continue;
            const json& f = jitem["f"];

            // Parse each versioned field; skip the entire item only if we can't
            // get *any* useful field (but spec says malformed items are ignored individually).
            bool anyField = false;

            if (f.contains("name")) {
                if (parseVersionedField(f["name"], item.name, item.nameVer)) anyField = true;
            }
            if (f.contains("qty")) {
                if (parseVersionedField(f["qty"], item.qty, item.qtyVer)) anyField = true;
            }
            // Absent des payloads émis par les versions antérieures : la note reste vide
            // en version {0,""}, que toute note réelle bat au merge (jamais d'écrasement).
            if (f.contains("note")) {
                if (parseVersionedField(f["note"], item.note, item.noteVer)) anyField = true;
            }
            if (f.contains("aisle")) {
                if (parseVersionedField(f["aisle"], item.aisle, item.aisleVer)) anyField = true;
            }
            if (f.contains("image")) {
                if (parseVersionedField(f["image"], item.image, item.imageVer)) anyField = true;
            }
            if (f.contains("due")) {
                if (parseVersionedField(f["due"], item.due, item.dueVer)) anyField = true;
            }
            // Absent des payloads antérieurs : échéance texte vide en {0,""}.
            if (f.contains("dueNote")) {
                if (parseVersionedField(f["dueNote"], item.dueNote, item.dueNoteVer)) anyField = true;
            }
            if (f.contains("order")) {
                if (parseVersionedField(f["order"], item.order, item.orderVer)) anyField = true;
            }
            if (f.contains("done")) {
                if (parseVersionedField(f["done"], item.done, item.doneVer)) anyField = true;
            }
            if (f.contains("del")) {
                if (parseVersionedField(f["del"], item.del, item.delVer)) anyField = true;
            }

            if (!anyField && !f.empty()) {
                // All known fields malformed — skip item
                continue;
            }

            p.items.push_back(std::move(item));
        }
    }

    // Parse optional author deviceId.
    if (j.contains("by") && j["by"].is_string())
        p.by = j["by"].get<std::string>();

    // Parse optional title (snap or delta-with-title)
    if (j.contains("title")) {
        std::string title;
        Ver         ver;
        if (parseVersionedField(j["title"], title, ver)) {
            p.title    = std::move(title);
            p.titleVer = ver;
        }
    }

    // Parse optional sortMode (mode de classement répliqué)
    if (j.contains("sortMode")) {
        std::string mode;
        Ver         ver;
        if (parseVersionedField(j["sortMode"], mode, ver)) {
            p.sortMode    = std::move(mode);
            p.sortModeVer = ver;
        }
    }

    // Parse optional members
    if (j.contains("members") && j["members"].is_object()) {
        for (const auto& [devId, mval] : j["members"].items()) {
            std::string name;
            Ver         ver;
            if (parseVersionedField(mval, name, ver)) {
                p.members[devId] = {std::move(name), ver};
            }
        }
    }

    return p;
}

static json verToJson(const Ver& ver) {
    return json::array({ver.lamport, ver.deviceId});
}

std::string serializeImagePayload(const std::string& listId,
                                  const std::string& by,
                                  const std::string& sha,
                                  const std::vector<uint8_t>& data) {
    json j;
    j["v"]    = 1;
    j["t"]    = "img";
    j["list"] = listId;
    if (!by.empty())
        j["by"] = by;
    j["sha"]  = sha;
    j["data"] = b64Encode(data);
    return j.dump();
}

std::string serializePayload(const Payload& p) {
    if (p.type == Payload::Type::image)
        return serializeImagePayload(p.listId, p.by, p.imageSha, p.imageData);
    json j;
    j["v"]    = 1;
    j["t"]    = (p.type == Payload::Type::delta) ? "delta" : "snap";
    j["list"] = p.listId;
    if (!p.by.empty())
        j["by"] = p.by;

    json items = json::array();
    for (const auto& item : p.items) {
        json ji;
        ji["id"]      = item.itemId;
        ji["created"] = item.created;
        ji["by"]      = item.by;
        ji["doneAt"]  = item.doneAt;
        // Omis pour une tâche de premier niveau : payload plus court, et les pairs
        // d'une version antérieure au champ le lisent comme "".
        if (!item.parentId.empty())
            ji["parent"] = item.parentId;

        json f;
        f["name"] = json::array({item.name, verToJson(item.nameVer)});
        f["qty"]  = json::array({item.qty,  verToJson(item.qtyVer)});
        f["note"]  = json::array({item.note,  verToJson(item.noteVer)});
        f["aisle"] = json::array({item.aisle, verToJson(item.aisleVer)});
        f["image"] = json::array({item.image, verToJson(item.imageVer)});
        f["due"]     = json::array({item.due,     verToJson(item.dueVer)});
        f["dueNote"] = json::array({item.dueNote, verToJson(item.dueNoteVer)});
        f["order"]   = json::array({item.order,   verToJson(item.orderVer)});
        f["done"] = json::array({item.done, verToJson(item.doneVer)});
        f["del"]  = json::array({item.del,  verToJson(item.delVer)});
        ji["f"]   = f;

        items.push_back(ji);
    }
    j["items"] = items;

    if (p.title.has_value() && p.titleVer.has_value()) {
        j["title"] = json::array({*p.title, verToJson(*p.titleVer)});
    }

    if (p.sortMode.has_value() && p.sortModeVer.has_value()) {
        j["sortMode"] = json::array({*p.sortMode, verToJson(*p.sortModeVer)});
    }

    if (!p.members.empty()) {
        json members;
        for (const auto& [devId, nv] : p.members) {
            members[devId] = json::array({nv.first, verToJson(nv.second)});
        }
        j["members"] = members;
    }

    return j.dump();
}

} // namespace core
