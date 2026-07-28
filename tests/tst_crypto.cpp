#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonValue>

#include "net/crypto.h"
#include "net/nostr.h"

// Helper: build a fixed 32-byte key
static std::vector<uint8_t> makeKey(uint8_t fill = 0x42)
{
    return std::vector<uint8_t>(32, fill);
}

class TstCrypto : public QObject {
    Q_OBJECT

private slots:

    // ── Clé de liste ────────────────────────────────────────────────────────

    // Une clé vide rendrait le canal et le chiffrement identiques pour tous, et
    // l'URI d'appairage invalide : la génération ne doit jamais faillir en silence.
    void generatedKeyIs32RandomBytes()
    {
        const auto k1 = net::generateListKey();
        const auto k2 = net::generateListKey();

        QCOMPARE(k1.size(), size_t(32));
        QCOMPARE(k2.size(), size_t(32));
        QVERIFY(k1 != k2);
        QVERIFY(k1 != std::vector<uint8_t>(32, 0));

        // Une clé fraîche doit donner un canal distinct de celui d'une autre liste.
        QVERIFY(net::deriveChannelTag(k1) != net::deriveChannelTag(k2));
    }

    // ── Encryption ──────────────────────────────────────────────────────────

    void roundTripEncryptDecrypt()
    {
        auto key = makeKey();
        std::string ct = net::deriveChannelTag(key);
        std::string plain = R"({"action":"addItem","name":"pain"})";

        std::string cipher = net::encryptPayload(key, ct, plain);
        QVERIFY(!cipher.empty());

        auto result = net::decryptPayload(key, ct, cipher);
        QVERIFY(result.has_value());
        QCOMPARE(*result, plain);
    }

    void alteredPayloadReturnsNullopt()
    {
        auto key = makeKey();
        std::string ct = net::deriveChannelTag(key);
        std::string plain = "hello world";

        std::string cipher = net::encryptPayload(key, ct, plain);
        QVERIFY(!cipher.empty());

        // Flip a char near the middle (base64 data)
        if (cipher.size() > 10) {
            cipher[cipher.size() / 2] ^= 0x01;
        }

        auto result = net::decryptPayload(key, ct, cipher);
        QVERIFY(!result.has_value());
    }

    void differentADReturnsNullopt()
    {
        auto key = makeKey();
        std::string ct = net::deriveChannelTag(key);
        std::string plain = "test payload";

        std::string cipher = net::encryptPayload(key, ct, plain);
        QVERIFY(!cipher.empty());

        // Use a different channelTag for decryption
        std::string wrongCt = "deadbeefdeadbeefdeadbeefdeadbeef"; // 32 hex chars
        auto result = net::decryptPayload(key, wrongCt, cipher);
        QVERIFY(!result.has_value());
    }

    void deterministicDerivations()
    {
        auto key = makeKey(0x11);

        std::string ct1 = net::deriveChannelTag(key);
        std::string ct2 = net::deriveChannelTag(key);
        QCOMPARE(ct1, ct2);
        QCOMPARE((int)ct1.size(), 32);

        auto seed1 = net::deriveNostrSeed(key);
        auto seed2 = net::deriveNostrSeed(key);
        QCOMPARE(seed1, seed2);
        QCOMPARE((int)seed1.size(), 32);
    }

    // ── BIP340 Schnorr ───────────────────────────────────────────────────────

    void signAndVerify()
    {
        auto seed = makeKey(0x77);
        seed = net::deriveNostrSeed(seed); // ensure valid secp256k1 key

        net::NostrEvent ev;
        ev.created_at = 1700000000;
        ev.kind = 4545;
        ev.tags = QJsonArray{ QJsonArray{ "t", "testchannel" } };
        ev.content = "dGVzdA=="; // base64("test")

        net::signEvent(ev, seed);

        QVERIFY(!ev.id.isEmpty());
        QVERIFY(!ev.pubkey.isEmpty());
        QVERIFY(!ev.sig.isEmpty());
        QCOMPARE((int)ev.id.size(), 64);
        QCOMPARE((int)ev.pubkey.size(), 64);
        QCOMPARE((int)ev.sig.size(), 128);

        QVERIFY(net::verifyEvent(ev));
    }

    void modifiedEventFails()
    {
        auto seed = makeKey(0x55);
        seed = net::deriveNostrSeed(seed);

        net::NostrEvent ev;
        ev.created_at = 1700000001;
        ev.kind = 4545;
        ev.tags = QJsonArray{};
        ev.content = "c29tZWNvbnRlbnQ=";

        net::signEvent(ev, seed);
        QVERIFY(net::verifyEvent(ev));

        // Tamper with the id directly — signature no longer matches
        // (flip a character in the id hex)
        QString origId = ev.id;
        QString badId = origId;
        // Flip the last character
        QChar last = badId[badId.size() - 1];
        badId[badId.size() - 1] = (last == QChar('0')) ? QChar('1') : QChar('0');
        ev.id = badId;
        // Now the sig was made over origId, but id is badId → verify should fail
        QVERIFY(!net::verifyEvent(ev));
    }

    void bip340KnownVector()
    {
        // BIP340 test vector #0 (from BIP-340 test-vectors.csv):
        // secret key: 0000000000000000000000000000000000000000000000000000000000000003
        // public key: F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9
        // aux_rand:   0000000000000000000000000000000000000000000000000000000000000000
        // msg:        0000000000000000000000000000000000000000000000000000000000000000
        // sig:        E907831F80848D1069A5371B402410364BDF1C5F8307B0084C55F1CE2DCA8215
        //             25F66A4A85EA8B71E482A74F382D2CE5EBEEE8FDB2172F477DF4900D310536C0

        QString knownPubkey = "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";
        QString knownSig =
            "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca821525f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0";
        // msg is 32 zero bytes → hex is 64 zeros (this is the event "id")
        QString knownId = QString(64, '0');

        net::NostrEvent ev;
        ev.id     = knownId;
        ev.pubkey = knownPubkey;
        ev.sig    = knownSig;

        QVERIFY(net::verifyEvent(ev));
    }

    // ── URI (pairing) ────────────────────────────────────────────────────────
    // These are also tested in tst_pairing (pure C++), but a quick sanity here:

    void channelTagLength()
    {
        auto key = makeKey(0xAB);
        std::string tag = net::deriveChannelTag(key);
        QCOMPARE((int)tag.size(), 32); // 16 bytes → 32 hex chars
    }

    void emptyKeyGivesNonEmptyTag()
    {
        std::vector<uint8_t> empty;
        // Even an empty key should produce a 32-char tag (SHA256 of prefix+nothing)
        std::string tag = net::deriveChannelTag(empty);
        QCOMPARE((int)tag.size(), 32);
    }
};

QTEST_MAIN(TstCrypto)
#include "tst_crypto.moc"
