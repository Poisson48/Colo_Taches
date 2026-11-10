#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <QQmlEngine>

#include "appcontroller.h"
#include "permissions.h"
#include "platform.h"
#include "qrimageprovider.h"
#include "taskimageprovider.h"
#include "theme.h"
#include "updater.h"
#include "backgroundsync.h"

#ifdef Q_OS_ANDROID
#  include <QtCore/private/qandroidextras_p.h>
#endif
#ifdef COLO_HAS_CAMERA
#  include "qrscanner.h"
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_ANDROID
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("-service")) {
        QAndroidService serviceApp(argc, argv);
        serviceApp.setOrganizationName("ColoTaches");
        serviceApp.setApplicationName("ColoTaches");
        app::BackgroundSync sync;
        if (!sync.start()) return 0;
        return serviceApp.exec();
    }
#endif

    QGuiApplication app(argc, argv);
    app.setOrganizationName("ColoTaches");
    app.setApplicationName("ColoTaches");

    // L'UI s'appuie sur les attachements Material (theme, elevation, accent) :
    // ils sont ignorés si un autre style est actif.
    QQuickStyle::setStyle(QStringLiteral("Material"));

    // Canal de notification + permission POST_NOTIFICATIONS (Android 13+).
    app::initNotifications();

    app::AppController controller;
    if (!controller.init()) {
        qCritical("AppController::init() failed");
        return 1;
    }

    // Lien d'invitation ouvert depuis une autre app (WhatsApp, SMS…) : Android
    // délivre l'intent VIEW ici, y compris quand l'app tournait déjà.
    QDesktopServices::setUrlHandler(QStringLiteral("colotaches"),
                                    &controller, "handleJoinUrl");

    // SPEC §8 : au retour au premier plan, rattrapage immédiat — Android a pu tuer
    // la socket pendant la mise en veille, et la souscription doit être rejouée.
    QObject::connect(&app, &QGuiApplication::applicationStateChanged,
                     &controller, [&controller](Qt::ApplicationState state) {
        const bool active = (state == Qt::ApplicationActive);
        controller.onApplicationStateChanged(active);
        if (active)
            controller.syncEngine()->subscribeAllLists();
    });

    // Types du scanner, dans le même module QML que les écrans (URI ColoTaches).
    // Sans Qt Multimedia, ScanPage.qml n'est pas embarquée : son Loader échoue
    // proprement et l'appairage se fait par lien.
    app::Permissions permissions;
    qmlRegisterSingletonInstance("ColoTaches", 1, 0, "Permissions", &permissions);
#ifdef COLO_HAS_CAMERA
    qmlRegisterType<app::QrScanner>("ColoTaches", 1, 0, "QrScanner");
#endif

    app::Theme theme;

    // Distribution hors Play Store : l'app va chercher elle-même la dernière release.
    app::Updater updater;
    updater.check();

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("qr"), new app::QrImageProvider());
    // Photos de tâches, servies depuis la base (le provider ouvre ses propres
    // connexions : les requêtes d'images arrivent d'un thread de chargement).
    engine.addImageProvider(QStringLiteral("taskimg"),
                            new app::TaskImageProvider(app::AppController::databasePath()));
    engine.rootContext()->setContextProperty(QStringLiteral("AppController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("Updater"), &updater);

    const QUrl url(QStringLiteral("qrc:/ColoTaches/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
