#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>

class QNetworkReply;

namespace app {

// Mise à jour depuis les Releases GitHub : l'app se distribue hors Play Store, donc
// personne ne la met à jour à notre place. On interroge les releases, on télécharge
// l'APK, et on laisse Android demander confirmation à l'utilisateur.
//
// L'APK publié est signé avec la clé de publication du projet : Android n'accepte de
// l'installer par-dessus que parce que la signature est identique (cf. release.yml).
class Updater : public QObject
{
    Q_OBJECT

    Q_PROPERTY(State   state          READ state          NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion  READ latestVersion  NOTIFY stateChanged)
    // Nouveautés de la (ou des) version(s) plus récente(s) que celle installée —
    // affichées AVANT de télécharger. Vide si rien à mettre à jour.
    Q_PROPERTY(QString releaseNotes   READ releaseNotes   NOTIFY stateChanged)
    // Historique des releases (du plus récent au plus ancien) :
    // [{ version, notes, publishedAt }, …]. Disponible même à jour.
    Q_PROPERTY(QVariantList changelog READ changelog NOTIFY changelogChanged)
    // Nouveautés depuis la dernière fois qu'on a lu les notes (APRÈS une mise à
    // jour, ou à l'ouverture). Vide une fois acknowledgeNotes() appelé.
    Q_PROPERTY(QString whatsNewNotes  READ whatsNewNotes  NOTIFY changelogChanged)
    Q_PROPERTY(bool    hasWhatsNew    READ hasWhatsNew    NOTIFY changelogChanged)
    Q_PROPERTY(qreal   progress       READ progress       NOTIFY progressChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool downloading     READ downloading     NOTIFY stateChanged)
    Q_PROPERTY(bool readyToInstall  READ readyToInstall  NOTIFY stateChanged)
    Q_PROPERTY(bool    canInstall     READ canInstall     CONSTANT)

public:
    enum State {
        Idle,         // rien à signaler (à jour, ou vérification jamais faite)
        Checking,
        Available,    // une version plus récente existe
        Downloading,
        Ready,        // APK téléchargé, prêt à être installé
        Failed,
    };
    Q_ENUM(State)

    explicit Updater(QObject *parent = nullptr);

    State   state() const { return m_state; }
    QString currentVersion() const;
    QString latestVersion() const { return m_latestVersion; }
    QString releaseNotes() const { return m_releaseNotes; }
    QVariantList changelog() const { return m_changelog; }
    QString whatsNewNotes() const { return m_whatsNewNotes; }
    bool    hasWhatsNew() const { return !m_whatsNewNotes.isEmpty(); }
    qreal   progress() const { return m_progress; }
    bool    canInstall() const;

    bool updateAvailable() const { return m_state == Available; }
    bool downloading() const     { return m_state == Downloading; }
    bool readyToInstall() const  { return m_state == Ready; }

    static bool isNewer(const QString &candidate, const QString &current);
    static QString notesFromBody(const QString &body);

public slots:
    void check();
    void download();
    void install();
    void dismiss();
    // Marque les notes de la version installée comme lues (après affichage post-maj).
    void acknowledgeNotes();

signals:
    void stateChanged();
    void progressChanged();
    void changelogChanged();

private:
    void setState(State s);
    void rebuildDerivedNotes();
    static QString formatEntries(const QVariantList &entries);

    QNetworkAccessManager   m_net;
    QPointer<QNetworkReply> m_reply;

    State   m_state = Idle;
    QString m_latestVersion;
    QString m_releaseNotes;    // versions > current (avant téléchargement)
    QString m_whatsNewNotes;   // versions <= current et > acknowledged (après maj)
    QVariantList m_changelog;  // historique complet
    QString m_apkUrl;
    QString m_releaseUrl;
    QString m_apkPath;
    qreal   m_progress = 0.0;
};

} // namespace app
