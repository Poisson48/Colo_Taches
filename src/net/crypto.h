#pragma once
#include <string>
#include <vector>
#include <optional>
#include "nostr.h"

namespace net {

// §3.1 — Fresh 32-byte list key (CSPRNG). Returns {} if libsodium fails to init.
// Toute liste doit en avoir une : clé vide = canal et chiffrement partagés par
// tous, et URI d'appairage invalide (parseJoinUri exige 32 octets).
std::vector<uint8_t> generateListKey();

// §3.2 — Derive a 32-hex-char channel tag from listKey.
// channelTag = hex(SHA256("colo-taches/v1/channel" || listKey))[0..31]
std::string deriveChannelTag(const std::vector<uint8_t>& listKey);

// §3.2 — Derive a 32-byte Nostr private key seed from listKey.
// nostrSeed = SHA256("colo-taches/v1/nostrkey" || listKey), re-hash if invalid.
std::vector<uint8_t> deriveNostrSeed(const std::vector<uint8_t>& listKey);

// §3.3 — Encrypt plainJson with XChaCha20-Poly1305.
// Returns base64(nonce24 || ciphertext), or "" on error.
std::string encryptPayload(const std::vector<uint8_t>& listKey,
                           const std::string& channelTag,
                           const std::string& plainJson);

// §3.3 — Decrypt. Returns plainJson or nullopt on any failure.
std::optional<std::string> decryptPayload(const std::vector<uint8_t>& listKey,
                                          const std::string& channelTag,
                                          const std::string& base64CipherText);

// §3.2 — Compute NIP-01 event id: SHA256 of canonical JSON array.
// [0, pubkey, created_at, kind, tags, content]
std::string computeEventId(const NostrEvent& ev);

// §3.2 — Fill ev.id, ev.pubkey, ev.sig using Schnorr BIP340.
// seed must be 32 bytes (from deriveNostrSeed).
// Fills id, pubkey and sig. Returns false (event untouched or partially
// filled, unusable) on any failure — callers must not publish in that case.
bool signEvent(NostrEvent& ev, const std::vector<uint8_t>& seed);

// Verify BIP340 Schnorr signature on a NostrEvent.
bool verifyEvent(const NostrEvent& ev);

} // namespace net
