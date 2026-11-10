package org.colotaches.app;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.WindowManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;

// Services natifs appelés depuis C++ via JNI (src/app/platform.cpp) : notification
// locale après un merge distant (SPEC §8) et feuille de partage du lien d'appairage.
// Aucune référence à la classe R générée : les ressources sont résolues par nom,
// pour ne dépendre d'aucun namespace Gradle.
public class Platform {

    private static final String CHANNEL_ID = "colotaches.sync";
    private static final int    NOTIFICATION_ID = 4545;
    private static final int    PERMISSION_REQUEST = 4545;

    public static void createChannel(Context ctx) {
        if (ctx == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.O)
            return;
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null || nm.getNotificationChannel(CHANNEL_ID) != null)
            return;
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "Synchronisation", NotificationManager.IMPORTANCE_DEFAULT);
        channel.setDescription("Changements reçus sur vos listes");
        nm.createNotificationChannel(channel);
    }

    // Android 13+ : POST_NOTIFICATIONS est une permission runtime. Sans activité
    // (cas d'un service), on ne peut pas la demander : on sort silencieusement.
    public static void requestPermission(Context ctx) {
        if (!(ctx instanceof Activity) || Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU)
            return;
        Activity activity = (Activity) ctx;
        if (activity.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            activity.requestPermissions(
                    new String[]{ android.Manifest.permission.POST_NOTIFICATIONS },
                    PERMISSION_REQUEST);
        }
    }

    public static void showNotification(Context ctx, String title, String body) {
        showNotification(ctx, title, body, 0L);
    }

    // whenMs > 0 : horodatage de la notification = heure de la modification
    // (pas l'heure de réception). setShowWhen(true) pour l'afficher dans le tiroir.
    public static void showNotification(Context ctx, String title, String body,
                                        long whenMs) {
        if (ctx == null)
            return;
        createChannel(ctx);
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null)
            return;

        Notification.Builder builder = new Notification.Builder(ctx, CHANNEL_ID)
                .setSmallIcon(smallIcon(ctx))
                .setContentTitle(title)
                .setContentText(body)
                .setStyle(new Notification.BigTextStyle().bigText(body))
                .setAutoCancel(true);

        if (whenMs > 0) {
            builder.setWhen(whenMs);
            builder.setShowWhen(true);
        }

        // Tap sur la notif → ouvre l'app.
        Intent open = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
        if (open != null) {
            open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            builder.setContentIntent(PendingIntent.getActivity(
                    ctx, 0, open,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        }

        nm.notify(NOTIFICATION_ID, builder.build());
    }

    // Veille différée : JobScheduler, aucun service foreground.
    public static void configureNotifications(Context ctx, String mode, String provider,
                                              int intervalMinutes, String baseUrl,
                                              String[] topics, String deviceId) {
        DeferredPushJob.configure(ctx, mode, provider, intervalMinutes,
                                  baseUrl, topics, deviceId);
    }

    // Feuille de partage native : le lien d'appairage part dans WhatsApp, SMS, mail…
    public static boolean shareText(Context ctx, String text) {
        if (ctx == null)
            return false;
        try {
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_TEXT, text);
            Intent chooser = Intent.createChooser(send, "Partager la liste");
            // Hors d'une Activity, le chooser exige sa propre tâche.
            chooser.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(chooser);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // --- Agenda ---
    // ACTION_INSERT sur le calendrier : l'app d'agenda s'ouvre pré-remplie (titre,
    // description, journée entière) et c'est l'utilisateur qui confirme. Aucune
    // permission calendrier requise — on n'écrit rien nous-mêmes.
    public static boolean addCalendarEvent(Context ctx, String title,
                                           String description, long startMs) {
        if (ctx == null)
            return false;
        try {
            Intent intent = new Intent(Intent.ACTION_INSERT)
                .setData(android.provider.CalendarContract.Events.CONTENT_URI)
                .putExtra(android.provider.CalendarContract.Events.TITLE, title)
                .putExtra(android.provider.CalendarContract.Events.DESCRIPTION, description)
                .putExtra(android.provider.CalendarContract.EXTRA_EVENT_BEGIN_TIME, startMs)
                .putExtra(android.provider.CalendarContract.EXTRA_EVENT_END_TIME,
                          startMs + 24L * 3600 * 1000)
                .putExtra(android.provider.CalendarContract.EXTRA_EVENT_ALL_DAY, true);
            intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(intent);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // --- Mise à jour depuis l'app ---
    //
    // PackageInstaller plutôt qu'un Intent ACTION_VIEW sur un content:// : celui-ci
    // imposerait un FileProvider (donc une dépendance androidx et une autorité
    // déclarée). Ici on écrit l'APK dans une session d'installation, et Android
    // affiche lui-même sa demande de confirmation — rien ne s'installe en douce.
    public static boolean installApk(Context ctx, String apkPath) {
        if (ctx == null || apkPath == null)
            return false;

        File apk = new File(apkPath);
        if (!apk.isFile() || apk.length() == 0)
            return false;

        PackageInstaller.Session session = null;
        try {
            PackageInstaller installer = ctx.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                    PackageInstaller.SessionParams.MODE_FULL_INSTALL);

            int sessionId = installer.createSession(params);
            session = installer.openSession(sessionId);

            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("colotaches", 0, apk.length())) {
                byte[] buffer = new byte[65536];
                int read;
                while ((read = in.read(buffer)) > 0)
                    out.write(buffer, 0, read);
                session.fsync(out);
            }

            // Android répond de façon asynchrone : pour une app hors Play Store, la
            // première réponse est STATUS_PENDING_USER_ACTION, qui porte l'écran de
            // confirmation à afficher. Sans ce receveur, l'installation resterait
            // silencieusement en attente.
            Intent status = new Intent(ACTION_INSTALL_STATUS).setPackage(ctx.getPackageName());
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                flags |= PendingIntent.FLAG_MUTABLE;   // Android remplit l'Intent de réponse

            PendingIntent pending = PendingIntent.getBroadcast(ctx, sessionId, status, flags);
            session.commit(pending.getIntentSender());
            return true;

        } catch (Exception e) {
            if (session != null)
                session.abandon();
            return false;
        } finally {
            if (session != null)
                session.close();
        }
    }

    public static final String ACTION_INSTALL_STATUS = "org.colotaches.app.INSTALL_STATUS";

    // Déclaré dans AndroidManifest.xml. Reçoit l'avancement de la session et ouvre
    // l'écran de confirmation système quand Android le demande.
    public static class InstallReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS,
                                            PackageInstaller.STATUS_FAILURE);
            if (status != PackageInstaller.STATUS_PENDING_USER_ACTION)
                return;   // succès, échec ou annulation : Android a déjà informé l'utilisateur

            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm == null)
                return;
            // Le receveur n'est pas une Activity : l'écran de confirmation a besoin
            // de sa propre tâche.
            confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(confirm);
        }
    }

    // Vibration courte : en mode Courses, on coche sans quitter le rayon des yeux.
    public static void vibrate(Context ctx, int ms) {
        if (ctx == null)
            return;
        try {
            Vibrator vibrator;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                VibratorManager manager = ctx.getSystemService(VibratorManager.class);
                vibrator = (manager != null) ? manager.getDefaultVibrator() : null;
            } else {
                vibrator = (Vibrator) ctx.getSystemService(Context.VIBRATOR_SERVICE);
            }
            if (vibrator == null || !vibrator.hasVibrator())
                return;
            vibrator.vibrate(VibrationEffect.createOneShot(
                    ms, VibrationEffect.DEFAULT_AMPLITUDE));
        } catch (Exception e) {
            // Pas de vibreur, permission refusée : ce n'est qu'un confort.
        }
    }

    // Mode Courses : l'écran doit rester allumé, on tient le téléphone sans le toucher
    // pendant des minutes. Les drapeaux de fenêtre ne se posent que sur le thread UI —
    // les toucher depuis le thread Qt lève une exception.
    public static void keepScreenOn(Context ctx, final boolean on) {
        if (!(ctx instanceof Activity))
            return;
        final Activity activity = (Activity) ctx;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (on)
                    activity.getWindow().addFlags(
                            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                else
                    activity.getWindow().clearFlags(
                            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
        });
    }

    private static int smallIcon(Context ctx) {
        int id = ctx.getResources().getIdentifier(
                "ic_stat_notify", "drawable", ctx.getPackageName());
        return id != 0 ? id : android.R.drawable.stat_notify_sync;
    }
}
