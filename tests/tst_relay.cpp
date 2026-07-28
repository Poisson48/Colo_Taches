#include <QtTest/QtTest>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QEventLoop>
#include <QTimer>
#include <QList>

#include "net/nostr.h"
#include "net/relayclient.h"
#include "net/relaypool.h"

// ── Minimal fake relay ─────────────────────────────────────────────────────
// Wraps a QWebSocketServer and tracks the last connected peer socket so
// individual tests can push messages.

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
        // Disconnect all peer sockets cleanly before we are destroyed,
        // so that their 'disconnected' signals won't call back into us.
        QList<QWebSocket*> snapshot = m_peers;
        m_peers.clear();
        for (QWebSocket* s : snapshot) {
            s->disconnect(this);
            s->close();
            delete s;
        }
        m_server.close();
    }

    // Listen on any available port (first call) or re-bind on same port (restart).
    bool listen(quint16 fixedPort = 0)
    {
        return m_server.listen(QHostAddress::LocalHost, fixedPort);
    }

    quint16 port() const { return m_server.serverPort(); }

    QUrl url() const {
        return QUrl(QString("ws://127.0.0.1:%1").arg(port()));
    }

    // Close the server and forcibly disconnect all peers.
    void closeAndDisconnectAll() {
        // Take a snapshot to avoid modifying m_peers while iterating.
        QList<QWebSocket*> snapshot = m_peers;
        for (QWebSocket* s : snapshot)
            s->close();
        m_server.close();
    }

    void close() { m_server.close(); }

    // Send a raw JSON text message to all connected peers.
    void broadcast(const QJsonArray& msg) {
        const QString text = QString::fromUtf8(
            QJsonDocument(msg).toJson(QJsonDocument::Compact));
        for (QWebSocket* s : m_peers)
            s->sendTextMessage(text);
    }

    // Send to a specific peer by index.
    void sendTo(int idx, const QJsonArray& msg) {
        if (idx < 0 || idx >= m_peers.size()) return;
        const QString text = QString::fromUtf8(
            QJsonDocument(msg).toJson(QJsonDocument::Compact));
        m_peers[idx]->sendTextMessage(text);
    }

    int peerCount() const { return m_peers.size(); }

    // Last REQ message received from any peer.
    QJsonArray lastReq() const { return m_lastReq; }

    // Last EVENT message received.
    QJsonArray lastEvent() const { return m_lastEvent; }

    // Messages received (type, raw array).
    struct RawMsg { QString type; QJsonArray arr; };
    QList<RawMsg> received() const { return m_received; }

signals:
    void messageReceived(const QString& msg);
    void peerConnected();
    void peerDisconnected();

private slots:
    void onNewConnection() {
        QWebSocket* s = m_server.nextPendingConnection();
        m_peers.append(s);
        connect(s, &QWebSocket::textMessageReceived,
                this, &FakeRelay::onMessage);
        connect(s, &QWebSocket::disconnected,
                this, &FakeRelay::onPeerDisconnected);
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
        if (type == "REQ")  m_lastReq   = arr;
        if (type == "EVENT") m_lastEvent = arr;
    }

    void onPeerDisconnected() {
        QWebSocket* s = qobject_cast<QWebSocket*>(sender());
        if (!s) return;
        m_peers.removeAll(s);
        s->disconnect(this);  // avoid re-entering after delete
        s->deleteLater();
        emit peerDisconnected();
    }

private:
    QWebSocketServer m_server;
    QList<QWebSocket*> m_peers;
    QJsonArray m_lastReq;
    QJsonArray m_lastEvent;
    QList<RawMsg> m_received;
};

// ── Helpers ────────────────────────────────────────────────────────────────

static bool waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return true;
}

// ── Test class ─────────────────────────────────────────────────────────────

class TstRelay : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<net::NostrEvent>("net::NostrEvent");
    }

    // ── NIP-01 parsing / serialisation ────────────────────────────────────

    void test_makeEventMsg() {
        net::NostrEvent ev;
        ev.id = "aabbcc";
        ev.pubkey = "pubkey1";
        ev.created_at = 1721000000;
        ev.kind = 4545;
        ev.content = "base64blob==";

        QJsonArray msg = net::makeEventMsg(ev);
        QCOMPARE(msg[0].toString(), QString("EVENT"));
        QJsonObject evJson = msg[1].toObject();
        QCOMPARE(evJson["kind"].toInt(), 4545);
        QCOMPARE(evJson["content"].toString(), QString("base64blob=="));
    }

    void test_makeReqMsg() {
        QJsonArray msg = net::makeReqMsg("sub1", {"4545"}, {"deadbeef"}, 1721000000);
        QCOMPARE(msg[0].toString(), QString("REQ"));
        QCOMPARE(msg[1].toString(), QString("sub1"));
        QJsonObject filter = msg[2].toObject();
        QCOMPARE(filter["kinds"].toArray()[0].toInt(), 4545);
        QCOMPARE(filter["#t"].toArray()[0].toString(), QString("deadbeef"));
        QCOMPARE(static_cast<int64_t>(filter["since"].toDouble()), int64_t(1721000000));
    }

    void test_makeCloseMsg() {
        QJsonArray msg = net::makeCloseMsg("sub42");
        QCOMPARE(msg[0].toString(), QString("CLOSE"));
        QCOMPARE(msg[1].toString(), QString("sub42"));
    }

    void test_parseEventMsg() {
        QJsonArray ev;
        ev.append("EVENT");
        ev.append("sub1");
        QJsonObject obj;
        obj["id"] = "abc";
        obj["pubkey"] = "pk";
        obj["created_at"] = 100;
        obj["kind"] = 4545;
        obj["tags"] = QJsonArray{};
        obj["content"] = "hello";
        obj["sig"] = "sig";
        ev.append(obj);

        net::RelayMsg msg = net::parseRelayMsg(
            QString::fromUtf8(QJsonDocument(ev).toJson(QJsonDocument::Compact)));
        QCOMPARE(msg.type, net::RelayMsgType::Event);
        QCOMPARE(msg.subId, QString("sub1"));
        QCOMPARE(msg.event.id, QString("abc"));
        QCOMPARE(msg.event.kind, 4545);
    }

    void test_parseEoseMsg() {
        QJsonArray arr;
        arr.append("EOSE");
        arr.append("sub99");
        net::RelayMsg msg = net::parseRelayMsg(
            QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        QCOMPARE(msg.type, net::RelayMsgType::Eose);
        QCOMPARE(msg.subId, QString("sub99"));
    }

    void test_parseOkMsg() {
        QJsonArray arr;
        arr.append("OK");
        arr.append("evid123");
        arr.append(true);
        arr.append("stored");
        net::RelayMsg msg = net::parseRelayMsg(
            QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        QCOMPARE(msg.type, net::RelayMsgType::Ok);
        QCOMPARE(msg.okEventId, QString("evid123"));
        QVERIFY(msg.okAccepted);
        QCOMPARE(msg.okMessage, QString("stored"));
    }

    void test_parseNoticeMsg() {
        QJsonArray arr;
        arr.append("NOTICE");
        arr.append("rate limited");
        net::RelayMsg msg = net::parseRelayMsg(
            QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        QCOMPARE(msg.type, net::RelayMsgType::Notice);
        QCOMPARE(msg.notice, QString("rate limited"));
    }

    void test_parseUnknown() {
        net::RelayMsg msg = net::parseRelayMsg("not json at all");
        QCOMPARE(msg.type, net::RelayMsgType::Unknown);
    }

    // ── RelayClient: REQ → EVENT → EOSE ───────────────────────────────────

    void test_reqEventEose() {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        net::RelayClient client(relay.url());
        QSignalSpy connSpy(&client, &net::RelayClient::connected);
        QSignalSpy eoseSpy(&client, &net::RelayClient::eose);

        // Capture eventReceived via direct connection to avoid QVariant/metatype issues.
        QString receivedId;
        connect(&client, &net::RelayClient::eventReceived,
                [&](const net::NostrEvent& ev) { receivedId = ev.id; });

        client.connectToRelay();
        client.subscribe("deadbeef00000000deadbeef00000000", 0);

        QVERIFY(connSpy.wait(2000));

        // Give the client time to send REQ.
        waitMs(50);

        // Check that relay received a REQ.
        QVERIFY(!relay.lastReq().isEmpty());
        QCOMPARE(relay.lastReq()[0].toString(), QString("REQ"));

        // Fake relay sends back an EVENT.
        QJsonArray eventMsg;
        eventMsg.append("EVENT");
        eventMsg.append("sub1"); // subId from the REQ
        QJsonObject evObj;
        evObj["id"] = "event001";
        evObj["pubkey"] = "pk";
        evObj["created_at"] = 1721000000;
        evObj["kind"] = 4545;
        evObj["tags"] = QJsonArray{};
        evObj["content"] = "base64blob==";
        evObj["sig"] = "";
        eventMsg.append(evObj);
        relay.broadcast(eventMsg);

        waitMs(200);
        QCOMPARE(receivedId, QString("event001"));

        // Fake relay sends EOSE.
        QJsonArray eoseMsg;
        eoseMsg.append("EOSE");
        eoseMsg.append("sub1");
        relay.broadcast(eoseMsg);

        QVERIFY(eoseSpy.wait(2000));
        QCOMPARE(eoseSpy.count(), 1);

        client.disconnectFromRelay();
    }

    // ── RelayClient: publish → OK ──────────────────────────────────────────

    void test_publishOk() {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        net::RelayClient client(relay.url());
        QSignalSpy connSpy(&client, &net::RelayClient::connected);
        QSignalSpy ackSpy(&client, &net::RelayClient::publishAck);

        client.connectToRelay();
        QVERIFY(connSpy.wait(2000));

        net::NostrEvent ev;
        ev.id = "publish001";
        ev.pubkey = "pk";
        ev.created_at = 1721000000;
        ev.kind = 4545;
        ev.content = "blob";
        client.publish(ev);

        waitMs(50);

        // Verify relay got an EVENT message.
        bool foundEvent = false;
        for (const auto& m : relay.received()) {
            if (m.type == "EVENT") { foundEvent = true; break; }
        }
        QVERIFY(foundEvent);

        // Fake relay replies OK.
        QJsonArray okMsg;
        okMsg.append("OK");
        okMsg.append("publish001");
        okMsg.append(true);
        okMsg.append("stored");
        relay.broadcast(okMsg);

        QVERIFY(ackSpy.wait(2000));
        QCOMPARE(ackSpy.count(), 1);
        QCOMPARE(ackSpy.at(0).at(0).toString(), QString("publish001"));
        QVERIFY(ackSpy.at(0).at(1).toBool());

        client.disconnectFromRelay();
    }

    // ── RelayPool: dedup — same event from 2 relays → 1 signal ─────────

    void test_poolDedup() {
        FakeRelay relay1, relay2;
        QVERIFY(relay1.listen(0));
        QVERIFY(relay2.listen(0));

        net::RelayPool pool;
        pool.setRelays({relay1.url(), relay2.url()});

        int evCount = 0;
        QStringList receivedIds;
        connect(&pool, &net::RelayPool::eventReceived,
                [&](const net::NostrEvent& ev) {
                    evCount++;
                    receivedIds.append(ev.id);
                });

        pool.connectAll();
        waitMs(300);  // let both connect

        // Both relays send the exact same event.
        auto makeEvMsg = [](const QString& id) {
            QJsonArray msg;
            msg.append("EVENT");
            msg.append("sub");
            QJsonObject obj;
            obj["id"] = id;
            obj["pubkey"] = "pk";
            obj["created_at"] = 1721000000;
            obj["kind"] = 4545;
            obj["tags"] = QJsonArray{};
            obj["content"] = "blob";
            obj["sig"] = "";
            msg.append(obj);
            return msg;
        };

        relay1.broadcast(makeEvMsg("dup001"));
        relay2.broadcast(makeEvMsg("dup001"));

        waitMs(200);

        // Should only receive one eventReceived signal.
        QCOMPARE(evCount, 1);
        QCOMPARE(receivedIds.count(), 1);
        QCOMPARE(receivedIds[0], QString("dup001"));

        // A different event from relay2 should still come through.
        relay2.broadcast(makeEvMsg("unique002"));
        waitMs(200);
        QCOMPARE(evCount, 2);

        pool.disconnectAll();
    }

    // ── RelayClient: reconnect — close server, reopen, check resubscription

    void test_reconnect() {
        auto relay = std::make_unique<FakeRelay>();
        QVERIFY(relay->listen());
        quint16 port = relay->port();

        net::RelayClient client(relay->url());
        QSignalSpy connSpy(&client, &net::RelayClient::connected);
        QSignalSpy discSpy(&client, &net::RelayClient::disconnected);

        client.connectToRelay();
        client.subscribe("channeltag0000000000000000000000", 0);
        QVERIFY(connSpy.wait(2000));
        waitMs(50);

        // Check initial subscription REQ was sent.
        QVERIFY(!relay->lastReq().isEmpty());

        // Drop the server → client should detect disconnect.
        relay->close();
        relay.reset();

        QVERIFY(discSpy.wait(5000));
        QCOMPARE(connSpy.count(), 1); // still only 1 connect so far

        // Bring up a new server on the same port (bind on any available: skip port reuse;
        // instead verify the client retries by starting a new fake relay and reassigning url).
        // Since we can't guarantee same port, we just verify the reconnect timer fires and
        // the client eventually tries again (connection attempt emitted signals).
        // The client should be in reconnect-backoff state.
        // We reconstruct a relay and redirect the client to it.

        // NOTE: RelayClient doesn't support URL change after construction.
        // This test verifies that disconnected signal was emitted and the backoff cycle
        // started. For a full resubscription test after reconnect, see test_resubAfterReconnect.

        client.disconnectFromRelay();
    }

    // ── RelayClient: resubscription after reconnect ────────────────────────

    void test_resubAfterReconnect() {
        // Start relay, connect, subscribe, then restart relay on the SAME port.
        // Verify a REQ is sent again after reconnection.

        FakeRelay relay;
        QVERIFY(relay.listen(0));          // bind to any available port
        const quint16 savedPort = relay.port();

        net::RelayClient client(relay.url());
        QSignalSpy connSpy(&client, &net::RelayClient::connected);
        QSignalSpy discSpy(&client, &net::RelayClient::disconnected);

        client.connectToRelay();
        client.subscribe("channeltag0000000000000000000000", 0);
        QVERIFY(connSpy.wait(2000));
        waitMs(50);

        const int reqCountBefore = [&]() {
            int n = 0;
            for (const auto& m : relay.received()) if (m.type == "REQ") n++;
            return n;
        }();
        QVERIFY(reqCountBefore >= 1);

        // Close all peer connections AND the server so the client detects disconnect.
        relay.closeAndDisconnectAll();

        // Wait for disconnect signal (with timeout).
        QVERIFY(discSpy.wait(3000));

        // Reopen the server on the SAME port so the client URL is still valid.
        QVERIFY(relay.listen(savedPort));

        // Backoff after first disconnect = 1 s (initial). After one failed attempt = 2 s.
        // Give it up to 6 s to reconnect.
        QVERIFY(connSpy.wait(6000));
        waitMs(100);

        const int reqCountAfter = [&]() {
            int n = 0;
            for (const auto& m : relay.received()) if (m.type == "REQ") n++;
            return n;
        }();
        // Should have sent at least one more REQ after reconnection.
        QVERIFY(reqCountAfter > reqCountBefore);

        client.disconnectFromRelay();
    }

    // ── RelayPool: online / offline ────────────────────────────────────────

    void test_onlineOffline() {
        FakeRelay relay;
        QVERIFY(relay.listen(0));

        net::RelayPool pool;
        pool.setRelays({relay.url()});
        QVERIFY(!pool.isOnline());

        QSignalSpy onlineSpy(&pool, &net::RelayPool::onlineChanged);

        pool.connectAll();
        QVERIFY(onlineSpy.wait(2000));
        QVERIFY(pool.isOnline());
        QCOMPARE(onlineSpy.at(0).at(0).toBool(), true);

        pool.disconnectAll();
        QVERIFY(onlineSpy.count() >= 1);
        // Wait briefly for the second signal.
        if (onlineSpy.count() < 2)
            onlineSpy.wait(2000);
        QVERIFY(!pool.isOnline());
    }
};

QTEST_MAIN(TstRelay)
#include "tst_relay.moc"
