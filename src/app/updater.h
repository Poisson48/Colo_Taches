#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;

namespace app {

// Mise à jour depuis les Releases GitHub : l'app se distribue hors Play Store, donc
// personne ne la met à jour à notre place. On interroge la dernière release, on
// télécharge l'APK, et on laisse Android demander confirmation à l'utilisateur.
//
// L'APK publié est signé avec la clé de publication du projet : Android n'accepte de
// l'installer par-dessus que parce que la signature est identique (cf. release.yml).
class Updater : public QObject
{
    Q_OBJECT

    Q_PROPERTY(State   state          READ state          NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion  READ latestVersion  NOTIFY stateChanged)
    // Ce que la nouvelle version apporte, tel que publié dans la release. Vide si la
    // release n'en dit rien — on n'invente pas un changelog.
    Q_PROPERTY(QString releaseNotes   READ releaseNotes   NOTIFY stateChanged)
    Q_PROPERTY(qreal   progress       READ progress       NOTIFY progressChanged)
    // L'enum n'est pas lisible depuis QML via une propriété de contexte : on expose
    // l'état sous forme de booléens, que la vue lit sans avoir à connaître de nombres.
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool downloading     READ downloading     NOTIFY stateChanged)
    Q_PROPERTY(bool readyToInstall  READ readyToInstall  NOTIFY stateChanged)
    // false hors Android : il n'y a pas d'APK à installer, le bouton renvoie alors
    // vers la page de la release.
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
    qreal   progress() const { return m_progress; }
    bool    canInstall() const;

    bool updateAvailable() const { return m_state == Available; }
    bool downloading() const     { return m_state == Downloading; }
    bool readyToInstall() const  { return m_state == Ready; }

    // "0.10.0" > "0.9.1" : comparaison composant par composant, pas lexicographique.
    // Un préfixe "v" est toléré. Retourne true si `candidate` est plus récent que
    // `current`. Exposé pour être testable.
    static bool isNewer(const QString &candidate, const QString &current);

    // Extrait les nouveautés du corps d'une release : tout ce qui précède la première
    // ligne « --- » (au-delà, ce sont les consignes d'installation, hors sujet dans
    // l'app). Exposé pour être testable.
    static QString notesFromBody(const QString &body);

public slots:
    // Interroge la dernière release. Silencieux en cas d'échec (hors ligne) : une
    // mise à jour ratée n'est pas une erreur à jeter au visage de l'utilisateur.
    void check();
    void download();
    // Lance l'installation (Android), ou ouvre la page de la release (ailleurs).
    void install();
    // Masquer la proposition jusqu'au prochain lancement.
    void dismiss();

signals:
    void stateChanged();
    void progressChanged();

private:
    void setState(State s);

    QNetworkAccessManager   m_net;
    QPointer<QNetworkReply> m_reply;

    State   m_state = Idle;
    QString m_latestVersion;
    QString m_releaseNotes;
    QString m_apkUrl;
    QString m_releaseUrl;
    QString m_apkPath;
    qreal   m_progress = 0.0;
};

} // namespace app
