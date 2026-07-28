#include "crypto.h"

#include <sodium.h>
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QByteArray>
#include <QString>

#include <cstring>
#include <cassert>

namespace net {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string bytesToHex(const uint8_t* data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[(data[i] >> 4) & 0xf]);
        out.push_back(hex[ data[i]       & 0xf]);
    }
    return out;
}

static bool hexToBytes(const std::string& hex, uint8_t* out, size_t expectedLen)
{
    if (hex.size() != expectedLen * 2) return false;
    for (size_t i = 0; i < expectedLen; ++i) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[2*i]);
        int lo = nibble(hex[2*i+1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// Compute SHA256 of (prefix || data)
static void sha256WithPrefix(const char* prefix, size_t prefixLen,
                              const uint8_t* data, size_t dataLen,
                              uint8_t out[crypto_hash_sha256_BYTES])
{
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, reinterpret_cast<const uint8_t*>(prefix), prefixLen);
    crypto_hash_sha256_update(&st, data, dataLen);
    crypto_hash_sha256_final(&st, out);
}

static secp256k1_context* getSecp256k1Ctx()
{
    static secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    return ctx;
}

// ---------------------------------------------------------------------------
// §3.1 — Clé de liste
// ---------------------------------------------------------------------------

std::vector<uint8_t> generateListKey()
{
    // sodium_init() est idempotent ; il doit précéder tout appel à randombytes_buf.
    if (sodium_init() < 0)
        return {};
    std::vector<uint8_t> key(32);
    randombytes_buf(key.data(), key.size());
    return key;
}

// ---------------------------------------------------------------------------
// §3.2 — Channel tag
// ---------------------------------------------------------------------------

std::string deriveChannelTag(const std::vector<uint8_t>& listKey)
{
    static const char prefix[] = "colo-taches/v1/channel";
    uint8_t hash[crypto_hash_sha256_BYTES];
    sha256WithPrefix(prefix, sizeof(prefix) - 1,
                     listKey.data(), listKey.size(),
                     hash);
    // Take first 16 bytes → 32 hex chars
    return bytesToHex(hash, 16);
}

// ---------------------------------------------------------------------------
// §3.2 — Nostr seed
// ---------------------------------------------------------------------------

std::vector<uint8_t> deriveNostrSeed(const std::vector<uint8_t>& listKey)
{
    static const char prefix[] = "colo-taches/v1/nostrkey";
    uint8_t hash[crypto_hash_sha256_BYTES];
    sha256WithPrefix(prefix, sizeof(prefix) - 1,
                     listKey.data(), listKey.size(),
                     hash);

    secp256k1_context* ctx = getSecp256k1Ctx();

    // Validate; re-hash (with iteration suffix) on the (astronomically rare) invalid case
    int iter = 0;
    while (!secp256k1_ec_seckey_verify(ctx, hash)) {
        // Re-hash: SHA256(previous_hash || iter_byte)
        uint8_t iterBuf[crypto_hash_sha256_BYTES + 1];
        std::memcpy(iterBuf, hash, crypto_hash_sha256_BYTES);
        iterBuf[crypto_hash_sha256_BYTES] = static_cast<uint8_t>(iter++);
        crypto_hash_sha256(hash, iterBuf, sizeof(iterBuf));
    }

    return std::vector<uint8_t>(hash, hash + 32);
}

// ---------------------------------------------------------------------------
// §3.3 — Encryption / Decryption
// ---------------------------------------------------------------------------

std::string encryptPayload(const std::vector<uint8_t>& listKey,
                           const std::string& channelTag,
                           const std::string& plainJson)
{
    if (listKey.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES)
        return "";

    // Random nonce
    uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    randombytes_buf(nonce, sizeof(nonce));

    const size_t plainLen  = plainJson.size();
    const size_t cipherLen = plainLen + crypto_aead_xchacha20poly1305_ietf_ABYTES;

    std::vector<uint8_t> cipher(cipherLen);
    unsigned long long actualLen = 0;

    int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        cipher.data(), &actualLen,
        reinterpret_cast<const uint8_t*>(plainJson.data()), plainLen,
        reinterpret_cast<const uint8_t*>(channelTag.data()), channelTag.size(),
        nullptr,
        nonce,
        listKey.data());

    if (rc != 0) return "";

    // Build nonce || ciphertext
    std::vector<uint8_t> blob;
    blob.reserve(sizeof(nonce) + actualLen);
    blob.insert(blob.end(), nonce, nonce + sizeof(nonce));
    blob.insert(blob.end(), cipher.begin(), cipher.begin() + actualLen);

    // base64-encode
    size_t b64MaxLen = sodium_base64_encoded_len(blob.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string b64(b64MaxLen, '\0');
    sodium_bin2base64(b64.data(), b64MaxLen,
                      blob.data(), blob.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    // Remove trailing null
    while (!b64.empty() && b64.back() == '\0') b64.pop_back();
    return b64;
}

std::optional<std::string> decryptPayload(const std::vector<uint8_t>& listKey,
                                          const std::string& channelTag,
                                          const std::string& base64CipherText)
{
    if (listKey.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES)
        return std::nullopt;

    // Decode base64
    const size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    const size_t minBlobLen = nonceLen + crypto_aead_xchacha20poly1305_ietf_ABYTES;

    std::vector<uint8_t> blob(base64CipherText.size()); // upper bound
    size_t blobLen = 0;
    if (sodium_base642bin(blob.data(), blob.size(),
                          base64CipherText.data(), base64CipherText.size(),
                          nullptr, &blobLen, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
        return std::nullopt;

    if (blobLen < minBlobLen) return std::nullopt;

    const uint8_t* nonce  = blob.data();
    const uint8_t* cipher = blob.data() + nonceLen;
    size_t cipherLen      = blobLen - nonceLen;

    std::vector<uint8_t> plain(cipherLen); // upper bound (actual is cipherLen - ABYTES)
    unsigned long long plainLen = 0;

    int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        plain.data(), &plainLen,
        nullptr,
        cipher, cipherLen,
        reinterpret_cast<const uint8_t*>(channelTag.data()), channelTag.size(),
        nonce,
        listKey.data());

    if (rc != 0) return std::nullopt;

    return std::string(reinterpret_cast<char*>(plain.data()), plainLen);
}

// ---------------------------------------------------------------------------
// §3.2 — NIP-01 event id
// ---------------------------------------------------------------------------

// NIP-01 string escaping for the canonical array. Our content is base64
// (no escapable chars today), but the id must stay correct if that changes.
static QString escapeNip01(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (QChar qc : s) {
        ushort c = qc.unicode();
        switch (c) {
        case '"':  out += QStringLiteral("\\\""); break;
        case '\\': out += QStringLiteral("\\\\"); break;
        case '\n': out += QStringLiteral("\\n");  break;
        case '\r': out += QStringLiteral("\\r");  break;
        case '\t': out += QStringLiteral("\\t");  break;
        case '\b': out += QStringLiteral("\\b");  break;
        case '\f': out += QStringLiteral("\\f");  break;
        default:
            if (c < 0x20)
                out += QStringLiteral("\\u%1").arg(c, 4, 16, QLatin1Char('0'));
            else
                out += qc;
        }
    }
    return out;
}

std::string computeEventId(const NostrEvent& ev)
{
    // Serialize tags with Qt JSON
    QJsonDocument tagsDoc(ev.tags);
    QString tagsJson = QString::fromUtf8(tagsDoc.toJson(QJsonDocument::Compact));

    // Build: [0,"<pubkey>",<created_at>,<kind>,<tags_json>,"<content>"]
    // We build the string manually to ensure exact format
    QString canonical = QStringLiteral("[0,\"") + ev.pubkey
        + QStringLiteral("\",") + QString::number(ev.created_at)
        + QStringLiteral(",")   + QString::number(ev.kind)
        + QStringLiteral(",")   + tagsJson
        + QStringLiteral(",\"") + escapeNip01(ev.content) + QStringLiteral("\"]");

    QByteArray utf8 = canonical.toUtf8();

    uint8_t hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash,
                       reinterpret_cast<const uint8_t*>(utf8.constData()),
                       static_cast<size_t>(utf8.size()));

    return bytesToHex(hash, 32);
}

// ---------------------------------------------------------------------------
// §3.2 — signEvent / verifyEvent
// ---------------------------------------------------------------------------

bool signEvent(NostrEvent& ev, const std::vector<uint8_t>& seed)
{
    if (seed.size() != 32) return false;

    secp256k1_context* ctx = getSecp256k1Ctx();

    // Create keypair
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, seed.data())) return false;

    // Get x-only public key
    secp256k1_xonly_pubkey xonly;
    if (!secp256k1_keypair_xonly_pub(ctx, &xonly, nullptr, &keypair)) return false;

    uint8_t pubkeyBytes[32];
    if (!secp256k1_xonly_pubkey_serialize(ctx, pubkeyBytes, &xonly)) return false;

    ev.pubkey = QString::fromStdString(bytesToHex(pubkeyBytes, 32));

    // Compute event id (now that pubkey is set)
    ev.id = QString::fromStdString(computeEventId(ev));

    // Sign the id (32 bytes)
    uint8_t msgBytes[32];
    if (!hexToBytes(ev.id.toStdString(), msgBytes, 32)) return false;

    uint8_t sig[64];
    // NULL aux_rand → deterministic signing
    if (!secp256k1_schnorrsig_sign32(ctx, sig, msgBytes, &keypair, nullptr)) return false;

    ev.sig = QString::fromStdString(bytesToHex(sig, 64));
    return true;
}

bool verifyEvent(const NostrEvent& ev)
{
    secp256k1_context* ctx = getSecp256k1Ctx();

    // Parse pubkey
    uint8_t pubkeyBytes[32];
    if (!hexToBytes(ev.pubkey.toStdString(), pubkeyBytes, 32)) return false;

    secp256k1_xonly_pubkey xonly;
    if (!secp256k1_xonly_pubkey_parse(ctx, &xonly, pubkeyBytes)) return false;

    // Parse sig
    uint8_t sigBytes[64];
    if (!hexToBytes(ev.sig.toStdString(), sigBytes, 64)) return false;

    // Parse msg (id)
    uint8_t msgBytes[32];
    if (!hexToBytes(ev.id.toStdString(), msgBytes, 32)) return false;

    return secp256k1_schnorrsig_verify(ctx, sigBytes, msgBytes, 32, &xonly) == 1;
}

} // namespace net
