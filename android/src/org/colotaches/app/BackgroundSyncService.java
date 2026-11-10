package org.colotaches.app;

import android.app.Notification;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;

import org.qtproject.qt.android.bindings.QtService;

public class BackgroundSyncService extends QtService {
    private static final int FG_ID = 4549;

    @Override
    public void onCreate() {
        Platform.createChannel(this);
        Notification.Builder b = new Notification.Builder(this, Platform.CHANNEL_ID)
                .setSmallIcon(smallIcon())
                .setContentTitle("Colo Tâches")
                .setContentText("Synchronisation en arrière-plan")
                .setOngoing(true)
                .setShowWhen(false)
                .setCategory(Notification.CATEGORY_SERVICE);
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (open != null) {
            open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            b.setContentIntent(PendingIntent.getActivity(this, 0, open,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        }
        try {
            startForeground(FG_ID, b.build());
        } catch (RuntimeException ignored) {
        }
        super.onCreate();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        super.onStartCommand(intent, flags, startId);
        new Handler(Looper.getMainLooper()).postDelayed(this::stopSelf, 30_000);
        return START_NOT_STICKY;
    }

    private int smallIcon() {
        int id = getResources().getIdentifier("ic_stat_notify", "drawable", getPackageName());
        return id != 0 ? id : android.R.drawable.stat_notify_sync;
    }
}
