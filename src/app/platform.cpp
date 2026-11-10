#include "platform.h"

#ifdef Q_OS_ANDROID
#  include <QCoreApplication>
#  include <QJniObject>
#  include <QJniEnvironment>
#  include <QMetaObject>
#  include "appcontroller.h"
#endif

namespace app {

#ifdef Q_OS_ANDROID

namespace {

constexpr const char* kPlatformClass = "org/colotaches/app/Platform";

// context() renvoie l'Activity quand l'app tourne au premier plan. Son type de
// retour a changé entre versions de Qt (QJniObject → jobject) : l'init par
// accolades accepte les deux.
QJniObject androidContext()
{
    return QJniObject{ QNativeInterface::QAndroidApplication::context() };
}

} // namespace

void initNotifications()
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid())
        return;

    QJniObject::callStaticMethod<void>(
        kPlatformClass, "createChannel",
        "(Landroid/content/Context;)V", ctx.object());

    QJniObject::callStaticMethod<void>(
        kPlatformClass, "requestPermission",
        "(Landroid/content/Context;)V", ctx.object());
}

bool platformNotify(const QString& title, const QString& body, qint64 whenMs)
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid())
        return false;

    const QJniObject jTitle = QJniObject::fromString(title);
    const QJniObject jBody  = QJniObject::fromString(body);

    QJniObject::callStaticMethod<void>(
        kPlatformClass, "showNotification",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;J)V",
        ctx.object(), jTitle.object<jstring>(), jBody.object<jstring>(),
        static_cast<jlong>(whenMs));

    return true;
}

bool platformShare(const QString& text)
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid())
        return false;

    const QJniObject jText = QJniObject::fromString(text);
    return QJniObject::callStaticMethod<jboolean>(
        kPlatformClass, "shareText",
        "(Landroid/content/Context;Ljava/lang/String;)Z",
        ctx.object(), jText.object<jstring>());
}

bool platformInstallApk(const QString& apkPath)
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid())
        return false;

    const QJniObject jPath = QJniObject::fromString(apkPath);
    return QJniObject::callStaticMethod<jboolean>(
        kPlatformClass, "installApk",
        "(Landroid/content/Context;Ljava/lang/String;)Z",
        ctx.object(), jPath.object<jstring>());
}

void platformVibrate(int ms)
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid())
        return;

    QJniObject::callStaticMethod<void>(
        kPlatformClass, "vibrate",
        "(Landroid/content/Context;I)V", ctx.object(), static_cast<jint>(ms));
}

void platformKeepScreenOn(bool on)
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid())
        return;

    QJniObject::callStaticMethod<void>(
        kPlatformClass, "keepScreenOn",
        "(Landroid/content/Context;Z)V", ctx.object(), static_cast<jboolean>(on));
}

bool platformAddCalendarEvent(const QString& title, const QString& description,
                              qint64 startMs)
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid())
        return false;

    const QJniObject jTitle = QJniObject::fromString(title);
    const QJniObject jDesc  = QJniObject::fromString(description);
    return QJniObject::callStaticMethod<jboolean>(
        kPlatformClass, "addCalendarEvent",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;J)Z",
        ctx.object(), jTitle.object<jstring>(), jDesc.object<jstring>(),
        static_cast<jlong>(startMs));
}

extern "C" JNIEXPORT void JNICALL
Java_org_colotaches_app_BackgroundSyncReceiver_nativeSync(JNIEnv*, jclass)
{
    if (auto* controller = app::AppController::instance())
        QMetaObject::invokeMethod(controller, "backgroundSyncNow", Qt::QueuedConnection);
}

void platformConfigureNotifications(const QString &mode, const QString &provider,
                                     int intervalMinutes, const QString &baseUrl,
                                     const QStringList &topics,
                                     const QString &deviceId)
{
    const QJniObject ctx = androidContext();
    if (!ctx.isValid()) return;
    QJniEnvironment env;
    const jsize n = static_cast<jsize>(topics.size());
    jobjectArray arr = env->NewObjectArray(n, env->FindClass("java/lang/String"), nullptr);
    for (jsize i = 0; i < n; ++i) {
        const QJniObject s = QJniObject::fromString(topics.at(static_cast<int>(i)));
        env->SetObjectArrayElement(arr, i, s.object<jstring>());
    }
    const QJniObject jMode = QJniObject::fromString(mode);
    const QJniObject jProvider = QJniObject::fromString(provider);
    const QJniObject jUrl = QJniObject::fromString(baseUrl);
    const QJniObject jDev = QJniObject::fromString(deviceId);
    QJniObject::callStaticMethod<void>(
        kPlatformClass, "configureNotifications",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;[Ljava/lang/String;Ljava/lang/String;)V",
        ctx.object(), jMode.object<jstring>(), jProvider.object<jstring>(),
        static_cast<jint>(intervalMinutes), jUrl.object<jstring>(), arr,
        jDev.object<jstring>());
    env->DeleteLocalRef(arr);
}

#else // !Q_OS_ANDROID

void initNotifications() {}

bool platformNotify(const QString&, const QString&, qint64) { return false; }

bool platformShare(const QString&) { return false; }

bool platformInstallApk(const QString&) { return false; }

void platformVibrate(int) {}

void platformKeepScreenOn(bool) {}

bool platformAddCalendarEvent(const QString&, const QString&, qint64) { return false; }

void platformConfigureNotifications(const QString &, const QString &, int,
                                     const QString &, const QStringList &,
                                     const QString &) {}

#endif

} // namespace app
