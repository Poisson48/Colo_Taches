#pragma once

#include "types.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace core {

// §4 — Parsed representation of a delta, snap, or img payload.
struct Payload {
    enum class Type { delta, snap, image };

    Type        type = Type::delta;
    std::string listId;

    // deviceId de l'émetteur. Sans lui, le receveur devine l'auteur en prenant la
    // première entrée de members — arbitraire dans un snap, qui les porte toutes.
    // Champ optionnel : absent des payloads émis par les versions antérieures.
    std::string by;

    std::vector<Item> items;

    // snap-only fields (also allowed in delta when changed)
    std::optional<std::string> title;
    std::optional<Ver>         titleVer;

    // Mode de classement répliqué (voir ListMeta::sortMode). Optionnel : absent des
    // payloads des versions antérieures, et omis tant que personne ne l'a choisi.
    std::optional<std::string> sortMode;
    std::optional<Ver>         sortModeVer;

    // deviceId -> (displayName, ver)
    std::map<std::string, std::pair<std::string, Ver>> members;

    // img-only : blob JPEG d'une photo de tâche, adressé par contenu. `imageSha` est
    // le sha256 hex du blob — le même que porte le champ `image` des items. Le blob
    // voyage à part des deltas pour ne pas les alourdir ; l'ordre d'arrivée est
    // indifférent (l'UI affiche la photo dès que le blob est là).
    std::string          imageSha;
    std::vector<uint8_t> imageData;
};

// Deserialize a JSON string into a Payload.
// Returns nullopt if "v" != 1, type is unrecognised, or JSON is malformed at
// the top level. Individual malformed items are silently skipped (§4 rule).
// Unknown fields are ignored (forward-compat).
std::optional<Payload> parsePayload(const std::string& json);

// Serialize a Payload to compact JSON.
std::string serializePayload(const Payload& p);

// Sérialise un événement « img » : {"v":1,"t":"img","list":…,"by":…,"sha":…,
// "data":base64}. Le blob est le JPEG compressé tel qu'il sera stocké chez les pairs.
std::string serializeImagePayload(const std::string& listId,
                                  const std::string& by,
                                  const std::string& sha,
                                  const std::vector<uint8_t>& data);

} // namespace core
