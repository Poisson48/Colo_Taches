#include "webrtctransport.h"
#include "nostr.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QUuid>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <rtc/rtc.hpp>

namespace net {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string peerKey(const QString& deviceId, const QString& channelTag)
{
    return (deviceId + QStringLiteral(":") + channelTag).toStdString();
}

// Construit un IceServer TURN avec credentials. libdatachannel n'accepte pas
// directement une URL TURN + user/pass : il faut séparer host/service/type.
static rtc::IceServer makeIceServer(const QString& url,
                                    const QString& user,
                                    const QString& pass)
{
    const bool tls = url.startsWith(QStringLiteral("turns:"), Qt::CaseInsensitive);
    const bool turn = tls || url.startsWith(QStringLiteral("turn:"), Qt::CaseInsensitive);
    if (!turn || user.isEmpty())
        return rtc::IceServer{url.toStdString()};

    QString rest = url.mid(url.indexOf(QLatin1Char(':')) + 1);
    QString transport = QStringLiteral("udp");
    const int q = rest.indexOf(QStringLiteral("?transport="), 0, Qt::CaseInsensitive);
    if (q >= 0) {
        transport = rest.mid(q + 11);
        rest = rest.left(q);
    }

    QString host;
    int port = 3479;
    if (rest.startsWith(QLatin1Char('['))) {
        const int close = rest.indexOf(QStringLiteral("]:"));
        if (close < 0) return rtc::IceServer{url.toStdString()};
        host = rest.mid(1, close - 1);
        port = rest.mid(close + 2).toInt();
    } else {
        const int colon = rest.lastIndexOf(QLatin1Char(':'));
        if (colon < 0) return rtc::IceServer{url.toStdString()};
        host = rest.left(colon);
        port = rest.mid(colon + 1).toInt();
    }
    if (host.isEmpty() || port <= 0 || port > 65535)
        return rtc::IceServer{url.toStdString()};

    auto relay = tls ? rtc::IceServer::RelayType::TurnTls
                     : (transport.compare(QStringLiteral("tcp"), Qt::CaseInsensitive) == 0
                            ? rtc::IceServer::RelayType::TurnTcp
                            : rtc::IceServer::RelayType::TurnUdp);
    return rtc::IceServer(host.toStdString(), static_cast<uint16_t>(port),
                          user.toStdString(), pass.toStdString(), relay);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WebRTCTransport::WebRTCTransport(QObject* parent)
    : SyncTransport(parent)
{
    // Signaling reconnect backoff.
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &WebRTCTransport::connectSignaling);

    m_turnRefreshTimer.setSingleShot(true);
    connect(&m_turnRefreshTimer, &QTimer::timeout,
            this, &WebRTCTransport::refreshTurnCredentials);

    // Connecter les signaux UNE SEULE FOIS : les reconnecter à chaque tentative
    // dupliquait les handlers après chaque coupure.
    connect(&m_signaling, &QWebSocket::connected,
            this, &WebRTCTransport::onSignalingConnected);
    connect(&m_signaling, &QWebSocket::disconnected,
            this, &WebRTCTransport::onSignalingDisconnected);
    connect(&m_signaling, &QWebSocket::textMessageReceived,
            this, &WebRTCTransport::onSignalingMessage);
    connect(&m_signaling, &QWebSocket::errorOccurred,
            this, &WebRTCTransport::onSignalingError);

    // Default ICE servers (Google public STUN + colo crevettes).
    m_iceUrls = {
        QStringLiteral("stun:stun.l.google.com:19302"),
        QStringLiteral("stun:colo-apps.les-crevettes-cevenoles.fr:3479"),
    };
}

WebRTCTransport::~WebRTCTransport()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void WebRTCTransport::setSignalingUrl(const QUrl& url)
{
    m_signalingUrl = url;
}

void WebRTCTransport::setP2pServerUrl(const QUrl& url)
{
    m_p2pServerUrl = url;
    if (!m_deviceId.isEmpty())
        refreshTurnCredentials();
}

void WebRTCTransport::setAppId(const QString& appId)
{
    if (!appId.isEmpty())
        m_appId = appId;
}

void WebRTCTransport::setIceServers(const QStringList& urls)
{
    m_iceUrls = urls;
}

void WebRTCTransport::setDeviceId(const QString& deviceId)
{
    m_deviceId = deviceId;
}

// ---------------------------------------------------------------------------
// SyncTransport interface
// ---------------------------------------------------------------------------

void WebRTCTransport::connectAll()
{
    if (m_shuttingDown) return;

    // TURN est obligatoire pour la 4G : on laisse une courte fenêtre au serveur
    // pour fournir les credentials avant d'ouvrir le signaling. En cas d'échec
    // (serveur plus ancien, endpoint absent), on continue en STUN + relais.
    if (!m_p2pServerUrl.isEmpty() && m_turnUser.isEmpty() && !m_waitingForTurn) {
        m_waitingForTurn = true;
        refreshTurnCredentials();
        QTimer::singleShot(3000, this, [this]() {
            if (m_waitingForTurn) {
                m_waitingForTurn = false;
                connectSignaling();
            }
        });
        return;
    }
    connectSignaling();
}

void WebRTCTransport::disconnectAll()
{
    disconnectSignaling();
    // Close all peer connections gracefully.
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        auto& peer = it->second;
        if (peer->dc) peer->dc->close();
        if (peer->pc) peer->pc->close();
    }
    m_peers.clear();
    m_waitingForTurn = false;
    setOnline(false);
}

void WebRTCTransport::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    m_reconnectTimer.stop();
    disconnectAll();
}

void WebRTCTransport::publishToAll(const NostrEvent& ev)
{
    // Extract channel tag from event tags.
    QString channelTag;
    for (const auto& tagVal : ev.tags) {
        QJsonArray tag = tagVal.toArray();
        if (tag.size() >= 2 && tag[0].toString() == QStringLiteral("t")) {
            channelTag = tag[1].toString();
            break;
        }
    }
    if (channelTag.isEmpty()) {
        qWarning() << "[WebRTC] publishToAll: no channel tag in event" << ev.id;
        return;
    }

    // Send the event JSON over data channels to all peers on this channel.
    QJsonObject eventJson;
    eventJson[QStringLiteral("type")] = QStringLiteral("event");
    eventJson[QStringLiteral("id")] = ev.id;
    eventJson[QStringLiteral("pubkey")] = ev.pubkey;
    eventJson[QStringLiteral("created_at")] = static_cast<qint64>(ev.created_at);
    eventJson[QStringLiteral("kind")] = ev.kind;
    eventJson[QStringLiteral("tags")] = ev.tags;
    eventJson[QStringLiteral("content")] = ev.content;
    eventJson[QStringLiteral("sig")] = ev.sig;

    const QJsonDocument doc(eventJson);
    const QByteArray data = doc.toJson(QJsonDocument::Compact);

    int sentCount = 0;
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        auto& peer = it->second;
        if (peer->channelTag != channelTag) continue;
        if (!peer->dcOpen || !peer->dc) continue;
        try {
            peer->dc->send(data.toStdString());
            ++sentCount;
        } catch (const std::exception& e) {
            qWarning() << "[WebRTC] send failed to" << peer->peerDeviceId
                       << ":" << e.what();
        }
    }

    // Un ack immédiat n'est valable que si au moins un pair a effectivement
    // reçu le message. Sans pair, on n'acquitte RIEN : l'outbox doit rester
    // en attente (le relais hybride, lui, acquittera de façon durable).
    if (sentCount > 0) {
        emit publishAck(ev.id, true, QStringLiteral("p2p"));
        emit publishAckKind(ev.id, AckKind::PeerDelivered, QStringLiteral("p2p"));
    } else {
        qDebug() << "[WebRTC] no peer on channel" << channelTag
                 << "— event kept pending" << ev.id.left(12);
    }
}

void WebRTCTransport::publishToPeer(const QString& peerId, const NostrEvent& ev)
{
    QString channelTag;
    for (const auto& tagVal : ev.tags) {
        const QJsonArray tag = tagVal.toArray();
        if (tag.size() >= 2 && tag[0].toString() == QStringLiteral("t")) {
            channelTag = tag[1].toString();
            break;
        }
    }
    if (channelTag.isEmpty()) return;

    auto it = m_peers.find(peerKey(peerId, channelTag));
    if (it == m_peers.end() || !it->second->dcOpen || !it->second->dc)
        return;

    QJsonObject eventJson;
    eventJson[QStringLiteral("type")] = QStringLiteral("event");
    eventJson[QStringLiteral("id")] = ev.id;
    eventJson[QStringLiteral("pubkey")] = ev.pubkey;
    eventJson[QStringLiteral("created_at")] = static_cast<qint64>(ev.created_at);
    eventJson[QStringLiteral("kind")] = ev.kind;
    eventJson[QStringLiteral("tags")] = ev.tags;
    eventJson[QStringLiteral("content")] = ev.content;
    eventJson[QStringLiteral("sig")] = ev.sig;

    try {
        it->second->dc->send(QJsonDocument(eventJson)
                                 .toJson(QJsonDocument::Compact)
                                 .toStdString());
    } catch (const std::exception& e) {
        qWarning() << "[WebRTC] targeted send failed to" << peerId << ":" << e.what();
    }
}

void WebRTCTransport::requestImage(const QString& channelTag, const QString& sha,
                                   const QString& variant)
{
    if (channelTag.isEmpty() || sha.isEmpty()) return;
    QJsonObject msg;
    msg[QStringLiteral("type")] = QStringLiteral("want_image");
    msg[QStringLiteral("channel_tag")] = channelTag;
    msg[QStringLiteral("sha")] = sha;
    msg[QStringLiteral("variant")] = variant.isEmpty()
        ? QStringLiteral("hd") : variant;
    broadcastToChannel(channelTag, msg);
}

void WebRTCTransport::subscribeAll(const QString& channelTag, int64_t /*since*/)
{
    if (m_subscribedChannels.contains(channelTag))
        return;
    m_subscribedChannels.insert(channelTag);

    // Announce on signaling server that we're interested in this channel.
    if (m_signaling.isValid()) {
        QJsonObject msg;
        msg[QStringLiteral("type")] = QStringLiteral("join");
        msg[QStringLiteral("channel_tag")] = channelTag;
        sendSignaling(msg);
    }
}

bool WebRTCTransport::isOnline() const
{
    return m_online;
}

// ---------------------------------------------------------------------------
// Signaling
// ---------------------------------------------------------------------------

void WebRTCTransport::connectSignaling()
{
    if (m_signalingUrl.isEmpty() || m_shuttingDown)
        return;

    if (m_signaling.state() == QAbstractSocket::ConnectedState ||
        m_signaling.state() == QAbstractSocket::ConnectingState)
        return;

    qDebug() << "[WebRTC] connecting signaling" << m_signalingUrl;

    m_signaling.open(m_signalingUrl);
}

void WebRTCTransport::disconnectSignaling()
{
    m_reconnectTimer.stop();
    m_signaling.close();
}

void WebRTCTransport::sendSignaling(const QJsonObject& msg)
{
    if (m_signaling.state() != QAbstractSocket::ConnectedState)
        return;
    const QJsonDocument doc(msg);
    m_signaling.sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void WebRTCTransport::onSignalingConnected()
{
    qDebug() << "[WebRTC] signaling connected";
    m_reconnectDelay = 1000;  // reset backoff

    // Register our device.
    QJsonObject reg;
    reg[QStringLiteral("type")] = QStringLiteral("register");
    reg[QStringLiteral("device_id")] = m_deviceId;
    sendSignaling(reg);

    // Re-join all subscribed channels.
    for (const QString& tag : m_subscribedChannels) {
        QJsonObject join;
        join[QStringLiteral("type")] = QStringLiteral("join");
        join[QStringLiteral("channel_tag")] = tag;
        sendSignaling(join);
    }
}

void WebRTCTransport::onSignalingDisconnected()
{
    qDebug() << "[WebRTC] signaling disconnected";
    setOnline(false);

    // Exponential backoff reconnect.
    if (!m_shuttingDown) {
        m_reconnectTimer.start(m_reconnectDelay);
        m_reconnectDelay = std::min(m_reconnectDelay * 2, 30000);
    }
}

void WebRTCTransport::onSignalingError(QAbstractSocket::SocketError error)
{
    qWarning() << "[WebRTC] signaling error" << error << m_signaling.errorString();
    // Un échec de connexion ne déclenche pas toujours disconnected() : il faut
    // reprogrammer explicitement, sinon le P2P ne revient jamais après un
    // serveur temporairement injoignable.
    if (!m_shuttingDown
        && m_signaling.state() != QAbstractSocket::ConnectedState
        && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start(m_reconnectDelay);
        m_reconnectDelay = std::min(m_reconnectDelay * 2, 30000);
    }
}

void WebRTCTransport::onSignalingMessage(const QString& text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject msg = doc.object();
    const QString type = msg[QStringLiteral("type")].toString();

    if (type == QStringLiteral("peer_joined")) {
        handlePeerJoined(msg);
    } else if (type == QStringLiteral("offer")) {
        handleOffer(msg);
    } else if (type == QStringLiteral("answer")) {
        handleAnswer(msg);
    } else if (type == QStringLiteral("candidate")) {
        handleCandidate(msg);
    } else if (type == QStringLiteral("peer_left")) {
        handlePeerLeft(msg);
    }
}

// ---------------------------------------------------------------------------
// Signaling handlers
// ---------------------------------------------------------------------------

void WebRTCTransport::handlePeerJoined(const QJsonObject& msg)
{
    const QString peerId = msg[QStringLiteral("device_id")].toString();
    const QString channelTag = msg[QStringLiteral("channel_tag")].toString();
    if (peerId.isEmpty() || channelTag.isEmpty() || peerId == m_deviceId)
        return;

    qDebug() << "[WebRTC] peer joined" << peerId << "on" << channelTag;

    // Un seul côté offre : le deviceId lexicographiquement le plus petit.
    // Les deux côtés reçoivent peer_joined ; sans ce tie-break, les deux
    // créaient une offre et se croisaient (glare).
    if (m_deviceId > peerId) {
        qDebug() << "[WebRTC] waiting for offer from" << peerId;
        return;
    }

    auto* peer = ensurePeer(peerId, channelTag, /*offerer=*/true);

    // Create SDP offer. The onLocalDescription callback (set up in ensurePeer)
    // will send it to the signaling server automatically.
    try {
        peer->pc->setLocalDescription(rtc::Description::Type::Offer);
    } catch (const std::exception& e) {
        qWarning() << "[WebRTC] createOffer failed:" << e.what();
    }
}

void WebRTCTransport::handleOffer(const QJsonObject& msg)
{
    const QString fromPeer = msg[QStringLiteral("from")].toString();
    const QString channelTag = msg[QStringLiteral("channel_tag")].toString();
    const QString sdp = msg[QStringLiteral("sdp")].toString();
    if (fromPeer.isEmpty() || sdp.isEmpty()) return;

    qDebug() << "[WebRTC] received offer from" << fromPeer;

    auto* peer = ensurePeer(fromPeer, channelTag, /*polite=*/false);
    try {
        peer->pc->setRemoteDescription(rtc::Description(sdp.toStdString(),
                                                        rtc::Description::Type::Offer));
    } catch (const std::exception& e) {
        qWarning() << "[WebRTC] setRemoteDescription(offer) failed:" << e.what();
    }
}

void WebRTCTransport::handleAnswer(const QJsonObject& msg)
{
    const QString fromPeer = msg[QStringLiteral("from")].toString();
    const QString channelTag = msg[QStringLiteral("channel_tag")].toString();
    const QString sdp = msg[QStringLiteral("sdp")].toString();
    if (fromPeer.isEmpty() || sdp.isEmpty()) return;

    const std::string key = peerKey(fromPeer, channelTag);
    auto it = m_peers.find(key);
    if (it == m_peers.end()) return;

    try {
        it->second->pc->setRemoteDescription(rtc::Description(sdp.toStdString(),
                                                               rtc::Description::Type::Answer));
    } catch (const std::exception& e) {
        qWarning() << "[WebRTC] setRemoteDescription(answer) failed:" << e.what();
    }
}

void WebRTCTransport::handleCandidate(const QJsonObject& msg)
{
    const QString fromPeer = msg[QStringLiteral("from")].toString();
    const QString channelTag = msg[QStringLiteral("channel_tag")].toString();
    const QString candidate = msg[QStringLiteral("candidate")].toString();
    const QString sdpMid = msg[QStringLiteral("sdpMid")].toString();
    if (fromPeer.isEmpty() || candidate.isEmpty()) return;

    const std::string key = peerKey(fromPeer, channelTag);
    auto it = m_peers.find(key);
    if (it == m_peers.end()) return;

    try {
        it->second->pc->addRemoteCandidate(rtc::Candidate(candidate.toStdString(),
                                                            sdpMid.toStdString()));
    } catch (const std::exception& e) {
        qWarning() << "[WebRTC] addRemoteCandidate failed:" << e.what();
    }
}

void WebRTCTransport::handlePeerLeft(const QJsonObject& msg)
{
    const QString peerId = msg[QStringLiteral("device_id")].toString();
    const QString channelTag = msg[QStringLiteral("channel_tag")].toString();
    if (peerId.isEmpty()) return;

    qDebug() << "[WebRTC] peer left" << peerId << "on" << channelTag;
    removePeer(peerId, channelTag);
}

// ---------------------------------------------------------------------------
// Peer connection management
// ---------------------------------------------------------------------------

std::vector<rtc::IceServer> WebRTCTransport::buildIceServers() const
{
    std::vector<rtc::IceServer> servers;
    QSet<QString> seen;
    const auto add = [&](const QString& url) {
        if (url.isEmpty() || seen.contains(url)) return;
        seen.insert(url);
        servers.push_back(makeIceServer(url, m_turnUser, m_turnPassword));
    };
    for (const QString& url : m_iceUrls)
        add(url);
    for (const QString& url : m_fetchedTurnUrls)
        add(url);
    return servers;
}

void WebRTCTransport::refreshTurnCredentials()
{
    if (m_p2pServerUrl.isEmpty() || m_deviceId.isEmpty()) {
        if (m_waitingForTurn) {
            m_waitingForTurn = false;
            connectSignaling();
        }
        return;
    }

    QUrl u = m_p2pServerUrl;
    QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("wss")) u.setScheme(QStringLiteral("https"));
    else if (scheme == QLatin1String("ws")) u.setScheme(QStringLiteral("http"));
    else if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
        if (m_waitingForTurn) { m_waitingForTurn = false; connectSignaling(); }
        return;
    }
    QString path = u.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    u.setPath(path + QStringLiteral("/turn-credentials"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("app"), m_appId);
    query.addQueryItem(QStringLiteral("device_id"), m_deviceId);
    u.setQuery(query);

    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = m_net.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[WebRTC] TURN credentials indisponibles:"
                       << reply->errorString();
            if (m_waitingForTurn) {
                m_waitingForTurn = false;
                connectSignaling();
            }
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            if (m_waitingForTurn) { m_waitingForTurn = false; connectSignaling(); }
            return;
        }
        const QJsonObject obj = doc.object();
        const QString user = obj.value(QStringLiteral("username")).toString();
        const QString pass = obj.value(QStringLiteral("credential")).toString();
        if (!user.isEmpty() && !pass.isEmpty()) {
            m_turnUser = user;
            m_turnPassword = pass;
            m_fetchedTurnUrls.clear();
            for (const QJsonValue& v : obj.value(QStringLiteral("urls")).toArray()) {
                const QString s = v.toString();
                if (!s.isEmpty()) m_fetchedTurnUrls.append(s);
            }
            const int ttl = obj.value(QStringLiteral("ttl")).toInt(43200);
            m_turnRefreshTimer.start(std::max(3600, ttl - 1800) * 1000);
            qInfo() << "[WebRTC] TURN credentials reçues, urls="
                    << m_fetchedTurnUrls.size();
        }
        if (m_waitingForTurn) {
            m_waitingForTurn = false;
            connectSignaling();
        }
    });
}

WebRTCTransport::PeerConnection*
WebRTCTransport::ensurePeer(const QString& peerDeviceId,
                            const QString& channelTag,
                            bool polite)
{
    const std::string key = peerKey(peerDeviceId, channelTag);
    auto it = m_peers.find(key);
    if (it != m_peers.end())
        return it->second.get();

    auto peer = std::make_unique<PeerConnection>();
    peer->peerDeviceId = peerDeviceId;
    peer->channelTag = channelTag;

    // Configure WebRTC.
    rtc::Configuration config;
    config.iceServers = buildIceServers();
    // Prefer relay (TURN) for NAT traversal reliability.
    config.iceTransportPolicy = rtc::TransportPolicy::All;

    peer->pc = std::make_shared<rtc::PeerConnection>(config);

    // Capture values for callbacks (Qt thread marshaling via invokeMethod).
    auto* transport = this;
    const QString pId = peerDeviceId;
    const QString cTag = channelTag;
    const std::string pKey = key;

    // ICE candidate generated → send to signaling server.
    peer->pc->onLocalCandidate([transport, pId, cTag](rtc::Candidate candidate) {
        QMetaObject::invokeMethod(transport, [transport, pId, cTag, candidateStr = std::string(candidate)]() {
            QJsonObject msg;
            msg[QStringLiteral("type")] = QStringLiteral("candidate");
            msg[QStringLiteral("to")] = pId;
            msg[QStringLiteral("channel_tag")] = cTag;
            msg[QStringLiteral("candidate")] = QString::fromStdString(candidateStr);
            msg[QStringLiteral("sdpMid")] = QString();  // libdatachannel handles this
            transport->sendSignaling(msg);
        });
    });

    // Local description generated → send offer/answer to signaling server.
    peer->pc->onLocalDescription([transport, pId, cTag](rtc::Description desc) {
        QMetaObject::invokeMethod(transport, [transport, pId, cTag,
                                              sdp = std::string(desc),
                                              type = desc.typeString()]() {
            QJsonObject msg;
            if (type == "offer") {
                msg[QStringLiteral("type")] = QStringLiteral("offer");
            } else {
                msg[QStringLiteral("type")] = QStringLiteral("answer");
            }
            msg[QStringLiteral("to")] = pId;
            msg[QStringLiteral("channel_tag")] = cTag;
            msg[QStringLiteral("sdp")] = QString::fromStdString(sdp);
            transport->sendSignaling(msg);
        });
    });

    // Peer connection state changed.
    peer->pc->onStateChange([transport, pKey](rtc::PeerConnection::State state) {
        QMetaObject::invokeMethod(transport, [transport, pKey, state]() {
            qDebug() << "[WebRTC] peer" << QString::fromStdString(pKey) << "state:" << static_cast<int>(state);
            if (state == rtc::PeerConnection::State::Disconnected ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed) {
                auto it = transport->m_peers.find(pKey);
                if (it != transport->m_peers.end()) {
                    it->second->dcOpen = false;
                    emit transport->peerStateChanged(it->second->channelTag,
                                                     transport->peerCount(it->second->channelTag));
                }
                transport->updateOnlineState();
            }
        });
    });

    // If we're NOT polite (we received the offer), the data channel is created
    // by the remote side — we handle it in onDataChannel.
    // If we ARE polite (we sent the offer), we create the data channel ourselves.
    if (polite) {
        const std::string dcLabel = cTag.toStdString();
        peer->dc = peer->pc->createDataChannel(dcLabel);
        wireDataChannel(peer.get(), transport, pKey);
    } else {
        // Accept data channel created by the remote peer.
        peer->pc->onDataChannel([transport, pKey](std::shared_ptr<rtc::DataChannel> dc) {
            QMetaObject::invokeMethod(transport, [transport, pKey, dc]() {
                auto it = transport->m_peers.find(pKey);
                if (it == transport->m_peers.end()) return;
                it->second->dc = dc;
                transport->wireDataChannel(it->second.get(), transport, pKey);
            });
        });
    }

    auto* rawPtr = peer.get();
    m_peers[key] = std::move(peer);
    return rawPtr;
}

void WebRTCTransport::removePeer(const QString& peerDeviceId, const QString& channelTag)
{
    const std::string key = peerKey(peerDeviceId, channelTag);
    auto it = m_peers.find(key);
    if (it == m_peers.end()) return;

    const QString tag = it->second->channelTag;
    if (it->second->dc) it->second->dc->close();
    if (it->second->pc) it->second->pc->close();
    m_peers.erase(it);
    emit peerStateChanged(tag, peerCount(tag));
    updateOnlineState();
}

void WebRTCTransport::sendToPeer(PeerConnection& peer, const QJsonObject& msg)
{
    if (!peer.dcOpen || !peer.dc) return;
    try {
        const QJsonDocument doc(msg);
        const QByteArray data = doc.toJson(QJsonDocument::Compact);
        peer.dc->send(data.toStdString());
    } catch (const std::exception& e) {
        qWarning() << "[WebRTC] sendToPeer failed:" << e.what();
    }
}

void WebRTCTransport::broadcastToChannel(const QString& channelTag, const QJsonObject& msg)
{
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        auto& peer = it->second;
        if (peer->channelTag != channelTag) continue;
        sendToPeer(*peer, msg);
    }
}

// ---------------------------------------------------------------------------
// Data channel wiring
// ---------------------------------------------------------------------------

// Static helper called from ensurePeer to wire data channel callbacks.
// Declared here, defined as a member helper with the peer pointer.
void WebRTCTransport::wireDataChannel(PeerConnection* peer,
                                       WebRTCTransport* transport,
                                       const std::string& peerKey)
{
    if (!peer || !peer->dc) return;

    auto* dc = peer->dc.get();
    const QString pId = peer->peerDeviceId;
    const QString cTag = peer->channelTag;

    dc->onOpen([transport, peerKey]() {
        QMetaObject::invokeMethod(transport, [transport, peerKey]() {
            auto it = transport->m_peers.find(peerKey);
            if (it == transport->m_peers.end()) return;
            it->second->dcOpen = true;
            qDebug() << "[WebRTC] data channel open with" << QString::fromStdString(peerKey);
            transport->setOnline(true);
            emit transport->peerStateChanged(it->second->channelTag,
                                             transport->peerCount(it->second->channelTag));

            // Demander le rattrapage uniquement pour le canal de ce pair.
            QJsonObject sub;
            sub[QStringLiteral("type")] = QStringLiteral("subscribe");
            sub[QStringLiteral("channel_tag")] = it->second->channelTag;
            sub[QStringLiteral("since")] = 0;  // catch up
            transport->sendToPeer(*it->second, sub);
        });
    });

    dc->onClosed([transport, peerKey]() {
        QMetaObject::invokeMethod(transport, [transport, peerKey]() {
            auto it = transport->m_peers.find(peerKey);
            if (it != transport->m_peers.end()) {
                it->second->dcOpen = false;
                emit transport->peerStateChanged(it->second->channelTag,
                                                 transport->peerCount(it->second->channelTag));
            }
            transport->updateOnlineState();
        });
    });

    dc->onMessage([transport, pId, cTag](std::variant<rtc::binary, rtc::string> message) {
        // Only handle string messages (JSON).
        if (!std::holds_alternative<rtc::string>(message)) return;
        const rtc::string text = std::get<rtc::string>(message);
        QMetaObject::invokeMethod(transport, [transport, pId, cTag, text]() {
            transport->onDataChannelMessage(pId, cTag, text);
        });
    });
}

// ---------------------------------------------------------------------------
// Data channel message handling
// ---------------------------------------------------------------------------

void WebRTCTransport::onDataChannelMessage(const QString& peerDeviceId,
                                           const QString& channelTag,
                                           const std::string& msg)
{
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(msg));
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString type = obj[QStringLiteral("type")].toString();

    if (type == QStringLiteral("event")) {
        handleEvent(obj);
        // Accusé de réception applicatif : le pair a bien reçu l'événement.
        const std::string key = peerKey(peerDeviceId, channelTag);
        auto it = m_peers.find(key);
        if (it != m_peers.end() && it->second->dcOpen) {
            QJsonObject ack;
            ack[QStringLiteral("type")] = QStringLiteral("ack");
            ack[QStringLiteral("event_id")] = obj.value(QStringLiteral("id"));
            sendToPeer(*it->second, ack);
        }
    } else if (type == QStringLiteral("ack")) {
        handleAck(obj);
    } else if (type == QStringLiteral("want_image")) {
        const QString sha = obj[QStringLiteral("sha")].toString();
        const QString variant = obj[QStringLiteral("variant")].toString();
        if (!sha.isEmpty())
            emit imageRequested(channelTag, peerDeviceId, sha,
                                variant.isEmpty() ? QStringLiteral("hd") : variant);
    } else if (type == QStringLiteral("subscribe")) {
        const QString requested = obj[QStringLiteral("channel_tag")].toString();
        if (!requested.isEmpty() && requested != channelTag)
            return;
        const int64_t since = obj[QStringLiteral("since")].toVariant().toLongLong();
        emit catchUpRequested(channelTag, peerDeviceId, since);
    }
}

void WebRTCTransport::handleEvent(const QJsonObject& obj)
{
    // Reconstruct NostrEvent from the data channel JSON.
    NostrEvent ev;
    ev.id = obj[QStringLiteral("id")].toString();
    ev.pubkey = obj[QStringLiteral("pubkey")].toString();
    ev.created_at = obj[QStringLiteral("created_at")].toVariant().toLongLong();
    ev.kind = obj[QStringLiteral("kind")].toInt();
    ev.tags = obj[QStringLiteral("tags")].toArray();
    ev.content = obj[QStringLiteral("content")].toString();
    ev.sig = obj[QStringLiteral("sig")].toString();

    if (ev.id.isEmpty() || ev.content.isEmpty())
        return;

    emit eventReceived(ev);
}

void WebRTCTransport::handleAck(const QJsonObject& obj)
{
    const QString eventId = obj[QStringLiteral("event_id")].toString();
    if (!eventId.isEmpty()) {
        emit publishAck(eventId, true, QStringLiteral("p2p"));
        emit publishAckKind(eventId, AckKind::PeerDelivered, QStringLiteral("p2p"));
    }
}

void WebRTCTransport::setOnline(bool online)
{
    if (m_online == online) return;
    m_online = online;
    emit onlineChanged(online);
}

void WebRTCTransport::updateOnlineState()
{
    bool anyOpen = false;
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (it->second->dcOpen) { anyOpen = true; break; }
    }
    setOnline(anyOpen);
}

int WebRTCTransport::peerCount(const QString& channelTag) const
{
    int count = 0;
    for (const auto& [key, peer] : m_peers) {
        Q_UNUSED(key);
        if (!peer->dcOpen) continue;
        if (!channelTag.isEmpty() && peer->channelTag != channelTag) continue;
        ++count;
    }
    return count;
}

} // namespace net