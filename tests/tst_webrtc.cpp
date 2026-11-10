// tst_webrtc.cpp — deux WebRTCTransport en loopback, avec un signaling local.
//
// Vérifie que le data channel s'ouvre, que l'initiateur déterministe évite le
// glare, qu'un événement traverse et qu'un ack PeerDelivered remonte.

#include <QtTest/QtTest>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>

#include "../src/net/webrtctransport.h"

using namespace net;

class FakeSignaling : public QObject
{
    Q_OBJECT
public:
    explicit FakeSignaling(QObject* parent = nullptr)
        : QObject(parent)
        , m_server(QStringLiteral("fake-signaling"), QWebSocketServer::NonSecureMode, this)
    {
        connect(&m_server, &QWebSocketServer::newConnection,
                this, &FakeSignaling::onNewConnection);
    }

    ~FakeSignaling() override {
        // Les sockets sont enfants du serveur ; sans ça, leur destruction pendant
        // ~QWebSocketServer rappelle des lambdas alors que nos QHash sont déjà morts.
        const auto sockets = m_sockets.keys();
        for (QWebSocket* ws : sockets)
            ws->disconnect(this);
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const {
        return QUrl(QStringLiteral("ws://127.0.0.1:%1/signal").arg(m_server.serverPort()));
    }

private slots:
    void onNewConnection() {
        while (QWebSocket* ws = m_server.nextPendingConnection()) {
            m_sockets.insert(ws, QString());
            connect(ws, &QWebSocket::textMessageReceived, this, [this, ws](const QString& text) {
                handle(ws, text);
            });
            connect(ws, &QWebSocket::disconnected, this, [this, ws]() {
                leave(ws);
                ws->deleteLater();
            });
        }
    }

private:
    void send(QWebSocket* ws, const QJsonObject& obj) {
        if (ws) ws->sendTextMessage(QString::fromUtf8(
            QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    }

    void notifyPeers(const QString& channel) {
        const auto members = m_rooms.value(channel);
        for (const QString& dev : members) {
            for (const QString& other : members) {
                if (dev == other) continue;
                QJsonObject j;
                j[QStringLiteral("type")] = QStringLiteral("peer_joined");
                j[QStringLiteral("device_id")] = other;
                j[QStringLiteral("channel_tag")] = channel;
                send(m_sockets.key(dev, nullptr), j);
            }
        }
    }

    void handle(QWebSocket* ws, const QString& text) {
        const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString type = msg.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("register")) {
            const QString device = msg.value(QStringLiteral("device_id")).toString();
            m_sockets[ws] = device;
            m_devices.insert(device, ws);
            QJsonObject ok;
            ok[QStringLiteral("type")] = QStringLiteral("registered");
            ok[QStringLiteral("device_id")] = device;
            send(ws, ok);
        } else if (type == QStringLiteral("join")) {
            const QString channel = msg.value(QStringLiteral("channel_tag")).toString();
            const QString device = m_sockets.value(ws);
            if (device.isEmpty() || channel.isEmpty()) return;
            m_rooms[channel].insert(device);
            QJsonObject state;
            state[QStringLiteral("type")] = QStringLiteral("room_state");
            state[QStringLiteral("channel_tag")] = channel;
            QJsonArray peers;
            for (const QString& d : m_rooms.value(channel))
                if (d != device) peers.append(QJsonObject{{QStringLiteral("device_id"), d}});
            state[QStringLiteral("peers")] = peers;
            send(ws, state);
            notifyPeers(channel);
        } else if (type == QStringLiteral("offer")
                   || type == QStringLiteral("answer")
                   || type == QStringLiteral("candidate")) {
            const QString to = msg.value(QStringLiteral("to")).toString();
            const QString device = m_sockets.value(ws);
            QWebSocket* target = m_devices.value(to, nullptr);
            if (!target) return;
            QJsonObject out = msg;
            out[QStringLiteral("from")] = device;
            send(target, out);
        } else if (type == QStringLiteral("ping")) {
            send(ws, QJsonObject{{QStringLiteral("type"), QStringLiteral("pong")}});
        }
    }

    void leave(QWebSocket* ws) {
        const QString device = m_sockets.take(ws);
        if (device.isEmpty()) return;
        m_devices.remove(device);
        for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it)
            it->remove(device);
    }

    QWebSocketServer m_server;
    QHash<QWebSocket*, QString> m_sockets;
    QHash<QString, QWebSocket*> m_devices;
    QHash<QString, QSet<QString>> m_rooms;
};

class TstWebRtc : public QObject
{
    Q_OBJECT
private slots:
    void loopbackDataChannel() {
        qRegisterMetaType<net::AckKind>("net::AckKind");
        qRegisterMetaType<net::NostrEvent>("net::NostrEvent");

        FakeSignaling signaling;
        QVERIFY(signaling.listen());

        const QString channel = QStringLiteral("0123456789abcdef0123456789abcdef");

        WebRTCTransport a;
        WebRTCTransport b;
        a.setDeviceId(QStringLiteral("device-a"));
        b.setDeviceId(QStringLiteral("device-b"));
        a.setSignalingUrl(signaling.url());
        b.setSignalingUrl(signaling.url());
        a.setIceServers({});
        b.setIceServers({});

        QSignalSpy ackSpy(&a, &SyncTransport::publishAckKind);
        QSignalSpy eventSpy(&b, &SyncTransport::eventReceived);

        a.connectAll();
        b.connectAll();
        a.subscribeAll(channel, 0);
        b.subscribeAll(channel, 0);

        QTRY_VERIFY_WITH_TIMEOUT(a.peerCount(channel) > 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(b.peerCount(channel) > 0, 15000);

        NostrEvent ev;
        ev.id = QString(64, QLatin1Char('a'));
        ev.created_at = 1234;
        ev.kind = 4545;
        ev.content = QStringLiteral("hello-p2p");
        ev.tags.append(QJsonArray{QStringLiteral("t"), channel});
        a.publishToAll(ev);

        QTRY_VERIFY_WITH_TIMEOUT(eventSpy.count() > 0, 10000);
        const auto received = eventSpy.takeFirst().at(0).value<net::NostrEvent>();
        QCOMPARE(received.id, ev.id);
        QCOMPARE(received.content, ev.content);

        QTRY_VERIFY_WITH_TIMEOUT(ackSpy.count() > 0, 10000);
        bool sawPeerAck = false;
        for (const auto& args : ackSpy) {
            if (args.at(1).value<net::AckKind>() == net::AckKind::PeerDelivered)
                sawPeerAck = true;
        }
        QVERIFY(sawPeerAck);

        a.shutdown();
        b.shutdown();
    }
};

QTEST_GUILESS_MAIN(TstWebRtc)
#include "tst_webrtc.moc"
