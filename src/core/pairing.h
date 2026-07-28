#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace core {

struct JoinInfo {
    std::string listId;
    std::vector<uint8_t> key; // 32 bytes
    std::string title;
};

// Build colotaches://join/1/<listId>/<base64url(key)>/<urlencode(title)>
std::string buildJoinUri(const std::string& listId,
                         const std::vector<uint8_t>& key,
                         const std::string& title);

// Parse the URI. Returns nullopt on any malformation.
std::optional<JoinInfo> parseJoinUri(const std::string& uri);

} // namespace core
