#include "updater.h"
#include "platform.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QDebug>

namespace app {

namespace {

// Le dépôt qui publie les releases. Changer de dépôt = changer cette ligne.
constexpr const char* kLatestReleaseApi =
    "https://api.github.com/repos/Poisson48/Colo_Taches/releases/latest";

#ifndef COLO_APP_VERSION
#  define COLO_APP_VERSION "0.0.0"
#endif

} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent)
{}

QString Updater::currentVersion() const
{
    return QStringLiteral(COLO_APP_VERSION);
}

bool Updater::canInstall() const
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

QString Updater::notesFromBody(const QString &body)
{
    QStringList kept;
    for (const QString &line : body.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed == QStringLiteral("---"))
            break;   // à partir d'ici, c'est la page GitHub qui parle, pas l'app
        // Les titres Markdown (« ## Nouveautés ») n'ont pas de rendu ici : le « # »
        // s'afficherait tel quel.
        QString clean = line;
        while (clean.startsWith(QLatin1Char('#')))
            clean.remove(0, 1);
        kept << clean.trimmed();
    }

    // Lignes vides en trop en fin de bloc.
    while (!kept.isEmpty() && kept.last().isEmpty())
        kept.removeLast();

    return kept.join(QLatin1Char('\n')).trimmed();
}

bool Updater::isNewer(const QString &candidate, const QString &current)
{
    const auto parts = [](QString v) {
        if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V')))
            v.remove(0, 1);
        QList<int> out;
        for (const QString &p : v.split(QLatin1Char('.'))) {
            // "0-rc1" → 0 : on ne garde que les chiffres de tête du composant.
            int digits = 0;
            while (digits < p.size() && p.at(digits).isDigit())
                ++digits;
            out << p.left(digits).toInt();
        }
        return out;
    };

    const QList<int> a = parts(candidate);
    const QList<int> b = parts(current);
    if (a.isEmpty())
        return false;

    // Comparaison composant par composant : "0.10.0" bat "0.9.1", ce qu'un simple
    // `>` sur les chaînes n'aurait pas vu.
    for (int i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y)
            return x > y;
    }
    return false;
}

void Updater::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;

    if (s == Available)
        qInfo() << "[Updater] version" << m_latestVersion
                << "disponible (nous sommes en" << currentVersion() << ")";
    else if (s == Failed)
        qWarning() << "[Updater] échec du téléchargement de" << m_apkUrl;

    emit stateChanged();
}

void Updater::check()
{
    if (m_state == Checking || m_state == Downloading)
        return;

    setState(Checking);

    QNetworkRequest req{ QUrl(QString::fromLatin1(kLatestReleaseApi)) };
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "ColoTaches");

    QNetworkReply *reply = m_net.get(req);
    m_reply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        // Hors ligne, quota GitHub atteint, release absente : on retombe simplement
        // dans l'état « rien à signaler ». Ce n'est pas une panne de l'app.
        if (reply->error() != QNetworkReply::NoError) {
            setState(Idle);
            return;
        }

        const QJsonObject obj =
            QJsonDocument::fromJson(reply->readAll()).object();

        const QString tag = obj.value(QStringLiteral("tag_name")).toString();
        m_releaseUrl = obj.value(QStringLiteral("html_url")).toString();

        // Le corps de la release porte d'abord les nouveautés, puis une ligne « --- »,
        // puis les consignes d'installation (utiles sur la page GitHub, hors sujet
        // dans l'app : on est déjà en train d'installer). On coupe au séparateur.
        m_releaseNotes = notesFromBody(obj.value(QStringLiteral("body")).toString());

        m_apkUrl.clear();
        for (const QJsonValue &v : obj.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject asset = v.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (name.endsWith(QStringLiteral(".apk"), Qt::CaseInsensitive)) {
                m_apkUrl = asset.value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        }

        if (tag.isEmpty() || !isNewer(tag, currentVersion())) {
            setState(Idle);
            return;
        }

        m_latestVersion = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
        setState(Available);
    });
}

void Updater::download()
{
    if (m_apkUrl.isEmpty()) {
        // Release sans APK (ou plateforme sans installation) : au moins ouvrir la page.
        install();
        return;
    }
    if (m_state == Downloading)
        return;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    m_apkPath = dir + QStringLiteral("/colotaches-") + m_latestVersion
              + QStringLiteral(".apk");

    QNetworkRequest req{ QUrl(m_apkUrl) };
    req.setRawHeader("User-Agent", "ColoTaches");
    // browser_download_url redirige vers le CDN : sans suivre la redirection, on
    // téléchargerait une page de 0 octet.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    m_progress = 0.0;
    emit progressChanged();
    setState(Downloading);

    QNetworkReply *reply = m_net.get(req);
    m_reply = reply;

    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        m_progress = (total > 0) ? qreal(received) / qreal(total) : 0.0;
        emit progressChanged();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            setState(Failed);
            return;
        }

        QFile out(m_apkPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            setState(Failed);
            return;
        }
        const QByteArray body = reply->readAll();
        const qint64 written = out.write(body);
        out.close();

        // Un APK tronqué serait refusé par Android avec un message incompréhensible :
        // mieux vaut échouer ici, explicitement.
        if (written != body.size() || body.isEmpty()) {
            QFile::remove(m_apkPath);
            setState(Failed);
            return;
        }

        setState(Ready);
    });
}

void Updater::install()
{
    if (canInstall() && m_state == Ready && !m_apkPath.isEmpty()) {
        if (platformInstallApk(m_apkPath))
            return;
        setState(Failed);
        return;
    }

    // Desktop (ou installation native impossible) : la page de la release, à défaut.
    if (!m_releaseUrl.isEmpty())
        QDesktopServices::openUrl(QUrl(m_releaseUrl));
}

void Updater::dismiss()
{
    if (m_reply)
        m_reply->abort();
    setState(Idle);
}

} // namespace app
