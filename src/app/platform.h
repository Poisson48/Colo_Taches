#pragma once

#include <QString>

namespace app {

// Pont vers les services natifs (Android : org.colotaches.app.Platform via JNI).
// Chaque fonction est un no-op renvoyant false hors Android, l'appelant fournit
// alors un repli desktop.

// Prépare le canal de notification et demande POST_NOTIFICATIONS (Android 13+).
// À appeler une fois au démarrage.
void initNotifications();

// Notification système native. whenMs > 0 → horodatage affiché (heure de modif).
// false → l'appelant retombe sur QSystemTrayIcon.
bool platformNotify(const QString& title, const QString& body, qint64 whenMs = 0);

// Feuille de partage native (ACTION_SEND). false → l'appelant copie le texte.
bool platformShare(const QString& text);

// Installe un APK déjà téléchargé (PackageInstaller). Android affiche sa propre
// demande de confirmation ; l'app n'installe rien dans le dos de l'utilisateur.
// false hors Android, ou si la session d'installation n'a pas pu s'ouvrir.
bool platformInstallApk(const QString& apkPath);

// Vibration courte : confirmer un cochage sans avoir à regarder l'écran.
void platformVibrate(int ms);

// Empêche l'écran de s'éteindre (mode Chantier : le téléphone posé à côté,
// les mains prises, sans le toucher pendant des minutes).
void platformKeepScreenOn(bool on);

// Propose l'ajout d'un événement « journée entière » à l'agenda du téléphone
// (ACTION_INSERT : l'app d'agenda s'ouvre pré-remplie, l'utilisateur confirme).
// false hors Android → l'appelant retombe sur un fichier .ics.
bool platformAddCalendarEvent(const QString& title, const QString& description,
                              qint64 startMs);

void platformConfigureNotifications(const QString &mode, const QString &provider,
                                    int intervalMinutes, const QString &baseUrl,
                                    const QStringList &topics,
                                    const QString &deviceId);
bool platformFcmAvailable();
void platformSetNotificationPreferences(const QString &alert, const QString &signal);

} // namespace app
