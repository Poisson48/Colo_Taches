#include "pairing.h"

#include <sstream>
#include <iomanip>
#include <cctype>
#include <cstdint>

namespace core {

// ---------------------------------------------------------------------------
// Base64url encoding (no padding, + → -, / → _)
// ---------------------------------------------------------------------------

static const char kB64UrlTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string base64urlEncode(const std::vector<uint8_t>& data)
{
    std::string out;
    size_t i = 0;
    const size_t n = data.size();
    while (i < n) {
        uint32_t val = 0;
        int bytes = 0;
        while (bytes < 3 && i < n) {
            val = (val << 8) | data[i++];
            ++bytes;
        }
        val <<= (8 * (3 - bytes)); // pad to 24 bits

        out.push_back(kB64UrlTable[(val >> 18) & 0x3f]);
        out.push_back(kB64UrlTable[(val >> 12) & 0x3f]);
        if (bytes >= 2) out.push_back(kB64UrlTable[(val >> 6) & 0x3f]);
        if (bytes >= 3) out.push_back(kB64UrlTable[(val      ) & 0x3f]);
    }
    return out;
}

static int b64urlCharToVal(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static std::optional<std::vector<uint8_t>> base64urlDecode(const std::string& s)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    const size_t n = s.size();
    // n must be divisible by 4 (with padding) or we handle no-padding
    // For no-padding base64url: we process groups of 4 chars
    while (i < n) {
        int c0 = (i < n) ? b64urlCharToVal(s[i++]) : 0;
        int c1 = (i < n) ? b64urlCharToVal(s[i++]) : 0;
        int c2 = (i < n) ? b64urlCharToVal(s[i++]) : -2; // -2 = absent
        int c3 = (i < n) ? b64urlCharToVal(s[i++]) : -2;

        if (c0 < 0 || c1 < 0) return std::nullopt;

        out.push_back(static_cast<uint8_t>((c0 << 2) | (c1 >> 4)));

        if (c2 == -2) break; // only 2 chars in last group → 1 byte output
        if (c2 < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>(((c1 & 0xf) << 4) | (c2 >> 2)));

        if (c3 == -2) break; // 3 chars in last group → 2 bytes output
        if (c3 < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>(((c2 & 0x3) << 6) | c3));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Percent-encoding (RFC 3986 unreserved chars not encoded)
// ---------------------------------------------------------------------------

static bool isUnreserved(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static std::string percentEncode(const std::string& s)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if (isUnreserved(static_cast<char>(c))) {
            oss << static_cast<char>(c);
        } else {
            oss << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return oss.str();
}

static std::string percentDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = -1, lo = -1;
            char h = s[i+1], l = s[i+2];
            if (h >= '0' && h <= '9') hi = h - '0';
            else if (h >= 'A' && h <= 'F') hi = h - 'A' + 10;
            else if (h >= 'a' && h <= 'f') hi = h - 'a' + 10;
            if (l >= '0' && l <= '9') lo = l - '0';
            else if (l >= 'A' && l <= 'F') lo = l - 'A' + 10;
            else if (l >= 'a' && l <= 'f') lo = l - 'a' + 10;
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// buildJoinUri / parseJoinUri
// ---------------------------------------------------------------------------

std::string buildJoinUri(const std::string& listId,
                         const std::vector<uint8_t>& key,
                         const std::string& title)
{
    std::string uri = "colotaches://join/1/";
    uri += listId;
    uri += '/';
    uri += base64urlEncode(key);
    uri += '/';
    uri += percentEncode(title);
    return uri;
}

std::optional<JoinInfo> parseJoinUri(const std::string& uri)
{
    static const std::string scheme = "colotaches://join/1/";
    if (uri.size() <= scheme.size()) return std::nullopt;
    if (uri.substr(0, scheme.size()) != scheme) return std::nullopt;

    std::string rest = uri.substr(scheme.size());

    // Split on '/' — expect exactly 3 parts: listId, keyB64url, title
    auto pos1 = rest.find('/');
    if (pos1 == std::string::npos) return std::nullopt;
    std::string listId = rest.substr(0, pos1);
    if (listId.empty()) return std::nullopt;

    rest = rest.substr(pos1 + 1);
    auto pos2 = rest.find('/');
    if (pos2 == std::string::npos) return std::nullopt;
    std::string keyB64 = rest.substr(0, pos2);
    if (keyB64.empty()) return std::nullopt;

    std::string titleEnc = rest.substr(pos2 + 1);

    // Decode key
    auto keyOpt = base64urlDecode(keyB64);
    if (!keyOpt || keyOpt->size() != 32) return std::nullopt;

    std::string title = percentDecode(titleEnc);

    JoinInfo info;
    info.listId = listId;
    info.key    = *keyOpt;
    info.title  = title;
    return info;
}

} // namespace core
