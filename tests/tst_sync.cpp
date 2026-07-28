// tst_sync.cpp — End-to-end sync engine integration tests (task 5.1).
//
// Two instances (A and B) share the same FakeRelay, each with its own
// Database and SyncEngine. Tests:
//   (a) add on A → appears on B
//   (b) concurrent edits on A and B → both converge identically
//   (c) A offline → mods in outbox → reconnect → B catches up
//   (d) corrupted / wrong-key event → ignored silently, no crash
//   (e) duplicate event replayed → merged exactly once
//
// The FakeRelay is imported/replicated from tst_relay.cpp.

#include <QtTest/QtTest>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QList>

#include "../src/store/database.h"
#include "../src/core/types.h"
#include "../src/core/crdt.h"
#include "../src/core/payload.h"
#include "../src/net/nostr.h"
#include "../src/net/relaypool.h"
#include "../src/net/crypto.h"
#include "../src/app/itemmodel.h"
#include "../src/app/syncengine.h"

using namespace store;
using namespace core;
using namespace net;
using namespace app;

// ---------------------------------------------------------------------------
// FakeRelay — same as in tst_relay.cpp
// ---------------------------------------------------------------------------

class FakeRelay : public QObject
{
    Q_OBJECT
public:
    explicit FakeRelay(QObject* parent = nullptr)
        : QObject(parent)
        , m_server("FakeRelay", QWebSocketServer::NonSecureMode, this)
    {
        connect(&m_server, &QWebSocketServer::newConnection,
                this, &FakeRelay::onNewConnection);
    }

    ~FakeRelay() override {
        QList<QWebSocket*> snapshot = m_peers;
        m_peers.clear();
        for (QWebSocket* s : snapshot) {
            s->disconnect(this);
            s->close();
            delete s;
        }
        m_server.close();
    }

    bool listen(quint16 fixedPort = 0) {
        return m_server.listen(QHostAddress::LocalHost, fixedPort);
    }

    quint16 port() const { return m_server.serverPort(); }
    QUrl    url()  const { return QUrl(QString("ws://127.0.0.1:%1").arg(port())); }

    void closeAndDisconnectAll() {
        QList<QWebSocket*> snapshot = m_peers;
        for (QWebSocket* s : snapshot) s->close();
        m_server.close();
    }

    void close() { m_server.close(); }

    // Broadcast raw JSON message to all connected peers.
    void broadcast(const QJsonArray& msg) {
        const QString text = QString::fromUtf8(
            QJsonDocument(msg).toJson(QJsonDocument::Compact));
        for (QWebSocket* s : m_peers)
            s->sendTextMessage(text);
    }

    int peerCount() const { return m_peers.size(); }

    struct RawMsg { QString type; QJsonArray arr; };
    QList<RawMsg> received() const { return m_received; }

    // Count how many EVENT messages arrived at the relay.
    int eventCount() const {
        int n = 0;
        for (const auto& m : m_received) if (m.type == "EVENT") n++;
        return n;
    }

signals:
    void messageReceived(const QString& msg);
    void peerConnected();
    void peerDisconnected();

private slots:
    void onNewConnection() {
        QWebSocket* s = m_server.nextPendingConnection();
        m_peers.append(s);
        connect(s, &QWebSocket::textMessageReceived, this, &FakeRelay::onMessage);
        connect(s, &QWebSocket::disconnected, this, &FakeRelay::onPeerDisconnected);
        emit peerConnected();
    }

    void onMessage(const QString& msg) {
        emit messageReceived(msg);
        QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (!doc.isArray()) return;
        QJsonArray arr = doc.array();
        if (arr.isEmpty()) return;
        const QString type = arr[0].toString();
        m_received.append({type, arr});

        // Auto-forward EVENT messages to all peers (act as store-and-forward relay).
        if (type == "EVENT" && arr.size() >= 2) {
            // Send back as ["EVENT", subId, eventObj] to all peers.
            // We need to wrap it in the relay→client format.
            QJsonObject evObj = arr[1].toObject();
            QString subId = m_activeSubId.isEmpty() ? "sub1" : m_activeSubId;
            QJsonArray fwd;
            fwd.append("EVENT");
            fwd.append(subId);
            fwd.append(evObj);
            broadcast(fwd);

            // Also send OK ack to the publisher.
            QWebSocket* sender_sock = qobject_cast<QWebSocket*>(sender());
            if (sender_sock) {
                QJsonArray ok;
                ok.append("OK");
                ok.append(evObj["id"].toString());
                ok.append(true);
                ok.append("stored");
                const QString okText = QString::fromUtf8(
                    QJsonDocument(ok).toJson(QJsonDocument::Compact));
                sender_sock->sendTextMessage(okText);
            }
        }

        // Track last subscription id.
        if (type == "REQ" && arr.size() >= 2)
            m_activeSubId = arr[1].toString();
    }

    void onPeerDisconnected() {
        QWebSocket* s = qobject_cast<QWebSocket*>(sender());
        if (!s) return;
        m_peers.removeAll(s);
        s->disconnect(this);
        s->deleteLater();
        emit peerDisconnected();
    }

private:
    QWebSocketServer m_server;
    QList<QWebSocket*> m_peers;
    QList<RawMsg> m_received;
    QString m_activeSubId;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Wait until condition() is true or timeout_ms elapses.
static bool waitFor(std::function<bool()> cond, int timeout_ms = 3000, int poll_ms = 50)
{
    int elapsed = 0;
    while (!cond() && elapsed < timeout_ms) {
        waitMs(poll_ms);
        elapsed += poll_ms;
    }
    return cond();
}

// Create a list meta with a fixed 32-byte key derived from listKey seed.
static ListMeta makeListMeta(const std::string& listId, uint8_t keySeed = 0xAB)
{
    ListMeta m;
    m.listId   = listId;
    m.key      = std::vector<uint8_t>(32, keySeed);
    m.title    = "Test List";
    m.titleVer = {1, "devA"};
    m.lamport  = 1;
    m.lastSync = 0;
    m.created  = 1000;
    return m;
}

// Peer: encapsulates a full sync stack (DB + RelayPool + SyncEngine + ItemModel).
// All QObjects are heap-allocated so we control destruction order explicitly.
struct Peer {
    QTemporaryDir           dir;
    Database                db;
    std::unique_ptr<RelayPool>   pool;
    std::unique_ptr<SyncEngine>  engine;
    std::unique_ptr<ItemModel>   model;
    std::string             deviceId;
    std::string             listId;

    Peer() = default;
    // Non-copyable, non-movable.
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    ~Peer() { shutdown(); }

    // Explicit clean shutdown: disconnect everything before destroying.
    void shutdown()
    {
        if (engine) {
            engine->unregisterItemModel(listId);
            engine.reset(); // disconnect from pool
        }
        if (pool) {
            pool->disconnectAll();
            pool.reset();
        }
        model.reset();
        db.close();
    }

    // Initialise the peer, add it to the FakeRelay, and subscribe.
    bool init(const QUrl& relayUrl, const std::string& devId, const ListMeta& meta)
    {
        if (!dir.isValid()) return false;
        if (!db.open(dir.filePath("test.db"))) return false;
        if (!db.createList(meta)) return false;

        deviceId = devId;
        listId   = meta.listId;

        pool   = std::make_unique<RelayPool>();
        engine = std::make_unique<SyncEngine>();
        model  = std::make_unique<ItemModel>();

        pool->setRelays({relayUrl});

        engine->init(&db, pool.get(),
                     QString::fromStdString(devId),
                     QString::fromStdString(devId));
        model->load(db, listId, deviceId);
        engine->registerItemModel(listId, model.get());

        pool->connectAll();
        engine->subscribeAllLists();

        return true;
    }

    // Trigger a local item addition and wait for debounce to fire.
    void addItem(const QString& name, const QString& qty = "1")
    {
        model->addItem(name, qty);
        engine->onLocalChange(listId);
        waitMs(500);
    }

    // Get visible items from model.
    QStringList itemNames() const
    {
        QStringList names;
        for (int i = 0; i < model->rowCount(); ++i)
            names << model->data(model->index(i), ItemModel::NameRole).toString();
        return names;
    }

    // Reload model from DB.
    void reloadModel()
    {
        model->load(db, listId, deviceId);
    }
};

// ---------------------------------------------------------------------------
// Test class
// ---------------------------------------------------------------------------

class TstSync : public QObject
{
    Q_OBJECT

private slots:

    void initTestCase()
    {
        qRegisterMetaType<net::NostrEvent>("net::NostrEvent");
    }

    // ── (a) Add on A → appears on B ────────────────────────────────────────
    void test_addPropagates()
    {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        const std::string listId = "list-sync-a";
        ListMeta meta = makeListMeta(listId, 0xAB);

        Peer A, B;
        QVERIFY(A.init(relay.url(), "devA", meta));
        QVERIFY(B.init(relay.url(), "devB", meta));

        // Wait for both to connect.
        QVERIFY(waitFor([&]{ return relay.peerCount() >= 2; }, 3000));
        waitMs(100);

        QSignalSpy spyB(B.engine.get(), &SyncEngine::remoteChanges);

        // A adds an item.
        A.addItem("Lait");

        // Wait for B to receive remote changes.
        QVERIFY(waitFor([&]{ return spyB.count() >= 1; }, 4000));

        B.reloadModel();
        QVERIFY(B.itemNames().contains("Lait"));
    }

    // ── Photo : le champ (delta) et le blob (événement img) arrivent chez B ──
    void test_imagePropagates()
    {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        const std::string listId = "list-sync-img";
        ListMeta meta = makeListMeta(listId, 0xCD);

        Peer A, B;
        QVERIFY(A.init(relay.url(), "devA", meta));
        QVERIFY(B.init(relay.url(), "devB", meta));
        QVERIFY(waitFor([&]{ return relay.peerCount() >= 2; }, 3000));
        waitMs(100);

        // A crée la tâche, y attache un blob et écrit le champ image.
        A.addItem("Repeindre le volet");
        const QString itemId =
            A.model->data(A.model->index(0), ItemModel::ItemIdRole).toString();
        const QByteArray blob("\xFF\xD8\xFF pretend-jpeg", 16);
        QVERIFY(A.db.putImage("sha-test", blob));

        QSignalSpy imgSpyB(B.engine.get(), &SyncEngine::imageArrived);

        A.model->setItemImage(itemId, "sha-test");
        A.engine->onLocalChange(listId);
        A.engine->publishImage(listId, "sha-test");
        waitMs(500);

        // B reçoit le blob (événement img) ET le champ (delta).
        QVERIFY(waitFor([&]{ return imgSpyB.count() >= 1; }, 4000));
        QCOMPARE(B.db.getImage("sha-test"), blob);

        QVERIFY(waitFor([&]{
            for (const auto& it : B.db.getItems(listId))
                if (it.image == "sha-test") return true;
            return false;
        }, 4000));
    }

    // ── Étapes : un item enfant créé chez A arrive chez B avec son parent ──
    void test_stepPropagates()
    {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        const std::string listId = "list-sync-steps";
        ListMeta meta = makeListMeta(listId, 0xEF);

        Peer A, B;
        QVERIFY(A.init(relay.url(), "devA", meta));
        QVERIFY(B.init(relay.url(), "devB", meta));
        QVERIFY(waitFor([&]{ return relay.peerCount() >= 2; }, 3000));
        waitMs(100);

        QSignalSpy spyB(B.engine.get(), &SyncEngine::remoteChanges);

        // A crée une tâche puis une étape dessous (second modèle, mode étapes).
        A.addItem("Repeindre le volet");
        const QString taskId =
            A.model->data(A.model->index(0), ItemModel::ItemIdRole).toString();
        ItemModel steps;
        steps.load(A.db, listId, A.deviceId, taskId.toStdString());
        steps.addItem("Poncer", "");
        A.engine->onLocalChange(listId);
        waitMs(500);

        QVERIFY(waitFor([&]{ return spyB.count() >= 1; }, 4000));
        QVERIFY(waitFor([&]{
            for (const auto& it : B.db.getItems(listId))
                if (it.name == "Poncer" && it.parentId == taskId.toStdString())
                    return true;
            return false;
        }, 4000));

        // Chez B, la liste des tâches ne montre pas l'étape, mais sa progression.
        B.reloadModel();
        QCOMPARE(B.model->count(), 1);
        QCOMPARE(B.model->data(B.model->index(0), ItemModel::StepsTotalRole).toInt(), 1);
    }

    // ── (b) Concurrent edits on A and B → convergence ──────────────────────
    void test_concurrentConvergence()
    {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        const std::string listId = "list-sync-b";
        ListMeta meta = makeListMeta(listId, 0xCD);

        Peer A, B;
        QVERIFY(A.init(relay.url(), "devA", meta));
        QVERIFY(B.init(relay.url(), "devB", meta));

        QVERIFY(waitFor([&]{ return relay.peerCount() >= 2; }, 3000));
        waitMs(100);

        QSignalSpy spyA(A.engine.get(), &SyncEngine::remoteChanges);
        QSignalSpy spyB(B.engine.get(), &SyncEngine::remoteChanges);

        // Both add different items simultaneously.
        A.model->addItem(QStringLiteral("Pain"), QStringLiteral("2"));
        A.engine->onLocalChange(listId);
        B.model->addItem(QStringLiteral("Beurre"), QStringLiteral("500g"));
        B.engine->onLocalChange(listId);

        // Wait for cross-propagation.
        QVERIFY(waitFor([&]{ return spyA.count() >= 1 && spyB.count() >= 1; }, 5000));

        A.reloadModel();
        B.reloadModel();

        // Both should have both items.
        QVERIFY(A.itemNames().contains("Pain"));
        QVERIFY(A.itemNames().contains("Beurre"));
        QVERIFY(B.itemNames().contains("Pain"));
        QVERIFY(B.itemNames().contains("Beurre"));

        // Same item sets.
        QStringList aSorted = A.itemNames();
        QStringList bSorted = B.itemNames();
        aSorted.sort();
        bSorted.sort();
        QCOMPARE(aSorted, bSorted);
    }

    // ── (c) Offline → outbox → reconnect → B catches up ───────────────────
    void test_offlineOutboxFlush()
    {
        FakeRelay relay;
        QVERIFY(relay.listen(0));
        const quint16 port = relay.port();

        const std::string listId = "list-sync-c";
        ListMeta meta = makeListMeta(listId, 0x12);

        Peer A, B;
        QVERIFY(A.init(relay.url(), "devA", meta));
        QVERIFY(B.init(relay.url(), "devB", meta));

        QVERIFY(waitFor([&]{ return relay.peerCount() >= 2; }, 3000));
        waitMs(100);

        // Disconnect the relay (simulate offline for A — A loses relay connection).
        relay.closeAndDisconnectAll();
        waitMs(300);

        // A adds item while offline.
        A.model->addItem(QStringLiteral("Fromage"), QStringLiteral("1"));
        A.engine->onLocalChange(listId);
        waitMs(400); // debounce fires → goes to outbox

        // Verify the item ended up in outbox for A.
        const auto outboxEntries = A.db.outboxPeekAll(listId);
        QVERIFY(!outboxEntries.empty());

        // Bring relay back on same port.
        QVERIFY(relay.listen(port));

        // B needs to reconnect too.
        QSignalSpy spyB(B.engine.get(), &SyncEngine::remoteChanges);

        // Wait for A and B to reconnect and A to flush outbox.
        QVERIFY(waitFor([&]{ return relay.peerCount() >= 2; }, 8000));
        waitMs(300); // let flush + forward happen

        // B should eventually see the item.
        QVERIFY(waitFor([&]{ return spyB.count() >= 1; }, 5000));

        B.reloadModel();
        QVERIFY(B.itemNames().contains("Fromage"));
    }

    // ── (d) Corrupted / wrong-key event → ignored silently, no crash ───────
    void test_corruptedEventIgnored()
    {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        const std::string listId = "list-sync-d";
        ListMeta meta = makeListMeta(listId, 0x55);

        Peer A;
        QVERIFY(A.init(relay.url(), "devA", meta));

        QVERIFY(waitFor([&]{ return relay.peerCount() >= 1; }, 3000));
        waitMs(100);

        QSignalSpy spy(A.engine.get(), &SyncEngine::remoteChanges);

        // Inject a fake event with same channel tag but wrong ciphertext (not decryptable).
        const std::string channelTag = net::deriveChannelTag(meta.key);

        QJsonObject evObj;
        evObj["id"]         = "fakedeadbeef0000000000000000000000000000000000000000000000000001";
        evObj["pubkey"]     = "0000000000000000000000000000000000000000000000000000000000000001";
        evObj["created_at"] = QDateTime::currentSecsSinceEpoch();
        evObj["kind"]       = 4545;
        QJsonArray tagArr;
        tagArr.append(QString("t"));
        tagArr.append(QString::fromStdString(channelTag));
        QJsonArray tagsArr;
        tagsArr.append(tagArr);
        evObj["tags"]       = tagsArr;
        evObj["content"]    = "dGhpcyBpcyBub3QgdmFsaWQgY2lwaGVydGV4dA=="; // garbage
        evObj["sig"]        = "0000000000000000000000000000000000000000000000000000000000000000"
                              "0000000000000000000000000000000000000000000000000000000000000000";

        // Broadcast the corrupted event to A via relay.
        QJsonArray fwd;
        fwd.append("EVENT");
        fwd.append("sub1");
        fwd.append(evObj);
        relay.broadcast(fwd);

        waitMs(300);

        // Engine should have ignored it — no remoteChanges emitted, no crash.
        QCOMPARE(spy.count(), 0);
    }

    // ── (e) Duplicate event replayed → merged exactly once ──────────────────
    void test_deduplicate()
    {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        const std::string listId = "list-sync-e";
        ListMeta meta = makeListMeta(listId, 0x77);

        Peer A, B;
        QVERIFY(A.init(relay.url(), "devA", meta));
        QVERIFY(B.init(relay.url(), "devB", meta));

        QVERIFY(waitFor([&]{ return relay.peerCount() >= 2; }, 3000));
        waitMs(100);

        QSignalSpy spyB(B.engine.get(), &SyncEngine::remoteChanges);

        // A adds an item → relay broadcasts the event once.
        A.addItem("Yaourt");

        QVERIFY(waitFor([&]{ return spyB.count() >= 1; }, 4000));
        B.reloadModel();
        QVERIFY(B.itemNames().contains("Yaourt"));
        const int countAfterFirst = B.db.getItems(listId).size();

        // Replay the same event from the relay to B directly.
        // Find the last EVENT message received at the relay.
        QJsonArray replayMsg;
        const auto& msgs = relay.received();
        for (int i = 0; i < msgs.size(); ++i) {
            if (msgs[i].type == "EVENT") replayMsg = msgs[i].arr;
        }
        QVERIFY(!replayMsg.isEmpty());

        // Reset spy to detect any new emission.
        spyB.clear();

        // Send the same event again as if from the relay.
        if (replayMsg.size() >= 2) {
            QJsonObject evObj = replayMsg[1].toObject();
            QJsonArray fwd;
            fwd.append("EVENT");
            fwd.append("sub1");
            fwd.append(evObj);
            relay.broadcast(fwd);
        }

        waitMs(300);

        // No new remoteChanges should be emitted (dedup).
        QCOMPARE(spyB.count(), 0);

        // Item count should not have increased.
        B.reloadModel();
        QCOMPARE((int)B.db.getItems(listId).size(), countAfterFirst);
    }
};

QTEST_GUILESS_MAIN(TstSync)
#include "tst_sync.moc"
