#include "zip.h"

#include <array>

namespace core {

namespace {

std::array<uint32_t, 256> makeCrcTable() {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[n] = c;
    }
    return t;
}

void putU16(std::string& out, uint16_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
}

void putU32(std::string& out, uint32_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
    out += static_cast<char>((v >> 16) & 0xFF);
    out += static_cast<char>((v >> 24) & 0xFF);
}

uint16_t getU16(const std::string& b, size_t p) {
    return static_cast<uint16_t>(static_cast<uint8_t>(b[p])) |
           static_cast<uint16_t>(static_cast<uint8_t>(b[p + 1])) << 8;
}

uint32_t getU32(const std::string& b, size_t p) {
    return static_cast<uint32_t>(static_cast<uint8_t>(b[p])) |
           static_cast<uint32_t>(static_cast<uint8_t>(b[p + 1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(b[p + 2])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(b[p + 3])) << 24;
}

constexpr uint32_t kSigLocal   = 0x04034b50;
constexpr uint32_t kSigCentral = 0x02014b50;
constexpr uint32_t kSigEocd     = 0x06054b50;

} // namespace

uint32_t zipCrc32(const std::string& data) {
    static const std::array<uint32_t, 256> table = makeCrcTable();
    uint32_t crc = 0xFFFFFFFFu;
    for (char ch : data)
        crc = table[(crc ^ static_cast<uint8_t>(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::string zipWrite(const std::vector<ZipEntry>& entries) {
    std::string out;
    struct Central { std::string name; uint32_t crc; uint32_t size; uint32_t offset; };
    std::vector<Central> central;

    for (const auto& e : entries) {
        const uint32_t crc  = zipCrc32(e.data);
        const uint32_t size = static_cast<uint32_t>(e.data.size());
        const uint32_t offset = static_cast<uint32_t>(out.size());

        putU32(out, kSigLocal);
        putU16(out, 20);   // version needed
        putU16(out, 0);    // flags
        putU16(out, 0);    // method: store
        putU16(out, 0);    // mod time
        putU16(out, 0);    // mod date
        putU32(out, crc);
        putU32(out, size); // compressed
        putU32(out, size); // uncompressed
        putU16(out, static_cast<uint16_t>(e.name.size()));
        putU16(out, 0);    // extra len
        out += e.name;
        out += e.data;

        central.push_back({ e.name, crc, size, offset });
    }

    const uint32_t cdStart = static_cast<uint32_t>(out.size());
    for (const auto& c : central) {
        putU32(out, kSigCentral);
        putU16(out, 20);   // version made by
        putU16(out, 20);   // version needed
        putU16(out, 0);    // flags
        putU16(out, 0);    // method
        putU16(out, 0);    // time
        putU16(out, 0);    // date
        putU32(out, c.crc);
        putU32(out, c.size);
        putU32(out, c.size);
        putU16(out, static_cast<uint16_t>(c.name.size()));
        putU16(out, 0);    // extra
        putU16(out, 0);    // comment
        putU16(out, 0);    // disk
        putU16(out, 0);    // internal attrs
        putU32(out, 0);    // external attrs
        putU32(out, c.offset);
        out += c.name;
    }
    const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdStart;

    putU32(out, kSigEocd);
    putU16(out, 0);   // disk
    putU16(out, 0);   // disk with cd
    putU16(out, static_cast<uint16_t>(central.size()));
    putU16(out, static_cast<uint16_t>(central.size()));
    putU32(out, cdSize);
    putU32(out, cdStart);
    putU16(out, 0);   // comment len

    return out;
}

std::optional<std::vector<ZipEntry>> zipRead(const std::string& bytes) {
    // On lit via le répertoire central : c'est l'index fiable d'un ZIP. On remonte
    // depuis la fin pour trouver l'EOCD (le commentaire final est vide chez nous).
    if (bytes.size() < 22)
        return std::nullopt;

    size_t eocd = std::string::npos;
    for (size_t i = bytes.size() - 22 + 1; i-- > 0; ) {
        if (getU32(bytes, i) == kSigEocd) { eocd = i; break; }
        if (bytes.size() - i > 22 + 65535) break;   // au-delà, plus aucun EOCD valide
    }
    if (eocd == std::string::npos)
        return std::nullopt;

    const uint16_t count   = getU16(bytes, eocd + 10);
    const uint32_t cdOffset = getU32(bytes, eocd + 16);

    std::vector<ZipEntry> out;
    size_t p = cdOffset;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > bytes.size() || getU32(bytes, p) != kSigCentral)
            return std::nullopt;

        const uint16_t method  = getU16(bytes, p + 10);
        const uint32_t size    = getU32(bytes, p + 24);
        const uint16_t nameLen = getU16(bytes, p + 28);
        const uint16_t extraLen= getU16(bytes, p + 30);
        const uint16_t cmtLen  = getU16(bytes, p + 32);
        const uint32_t localOff= getU32(bytes, p + 42);

        if (p + 46 + nameLen > bytes.size())
            return std::nullopt;
        std::string name = bytes.substr(p + 46, nameLen);

        // Méthode « store » seulement : c'est ce qu'on produit.
        if (method != 0)
            return std::nullopt;

        // Les données suivent l'en-tête local, dont on saute nom + extra.
        if (localOff + 30 > bytes.size() || getU32(bytes, localOff) != kSigLocal)
            return std::nullopt;
        const uint16_t lNameLen  = getU16(bytes, localOff + 26);
        const uint16_t lExtraLen = getU16(bytes, localOff + 28);
        const size_t dataStart = localOff + 30 + lNameLen + lExtraLen;
        if (dataStart + size > bytes.size())
            return std::nullopt;

        out.push_back({ std::move(name), bytes.substr(dataStart, size) });

        p += 46 + nameLen + extraLen + cmtLen;
    }
    return out;
}

} // namespace core
