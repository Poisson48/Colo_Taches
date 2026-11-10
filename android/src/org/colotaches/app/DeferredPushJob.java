package org.colotaches.app;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

// Veille différée : JobScheduler (15/30/60 min), aucune notification permanente,
// aucun service foreground. Une passe courte, puis le job se termine.
public class DeferredPushJob extends JobService {

    private static final String TAG = "ColoDeferredPush";
    private static final String PREFS = "colocourse_push";
    private static final int JOB_ID = 4547;
    private static final long NOTIFY_COOLDOWN_MS = 60_000L;

    private volatile Thread worker;

    public static void configure(Context ctx, String mode, String provider,
                                 int intervalMinutes, String baseUrl,
                                 String[] topics, String deviceId) {
        if (ctx == null) return;

        SharedPreferences prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        prefs.edit()
                .putString("mode", mode != null ? mode : "off")
                .putString("provider", provider != null ? provider : "auto")
                .putInt("interval", intervalMinutes)
                .putString("baseUrl", baseUrl != null ? baseUrl.trim() : "")
                .putString("deviceId", deviceId != null ? deviceId.trim() : "")
                .putStringSet("topics", new HashSet<>(Arrays.asList(
                        topics != null ? topics : new String[0])))
                .apply();

        JobScheduler scheduler = (JobScheduler) ctx.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (scheduler == null) return;
        scheduler.cancel(JOB_ID);

        if (!"deferred".equals(mode) || baseUrl == null || baseUrl.isEmpty()
                || topics == null || topics.length == 0) {
            return;
        }

        final int minutes = (intervalMinutes == 30 || intervalMinutes == 60)
                ? intervalMinutes : 15;
        JobInfo job = new JobInfo.Builder(
                JOB_ID, new ComponentName(ctx, DeferredPushJob.class))
                .setPeriodic(minutes * 60_000L)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setRequiresBatteryNotLow(true)
                .setPersisted(true)
                .build();
        scheduler.schedule(job);
        Log.i(TAG, "deferred job scheduled every " + minutes + " min, topics=" + topics.length);
    }

    @Override
    public boolean onStartJob(JobParameters params) {
        worker = new Thread(() -> {
            try {
                pollOnce();
            } catch (Exception e) {
                Log.w(TAG, "poll failed", e);
            } finally {
                jobFinished(params, false);
            }
        }, "ColoDeferredPush");
        worker.start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        if (worker != null) worker.interrupt();
        return true; // reschedule si le système a interrompu
    }

    private void pollOnce() {
        // Réveil best-effort du process Qt pour merger, avant/après la notification.
        sendBroadcast(new Intent(this, BackgroundSyncReceiver.class)
                .setAction("org.colocourse.app.BACKGROUND_SYNC"));
        SharedPreferences prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        final String baseUrl = prefs.getString("baseUrl", "");
        final Set<String> topics = prefs.getStringSet("topics", null);
        if (baseUrl.isEmpty() || topics == null || topics.isEmpty()) return;

        final String root = baseUrl.endsWith("/") ? baseUrl : baseUrl + "/";
        final String ownDevice = prefs.getString("deviceId", "");
        final String ownTag = ownDevice.isEmpty() ? "" : ("device:" + ownDevice);

        for (String topic : topics) {
            if (Thread.currentThread().isInterrupted()) return;
            try {
                final String sinceKey = "since_" + topic;
                final String since = prefs.getString(sinceKey, "");
                final boolean priming = since.isEmpty();

                String pollUrl = root + topic + "/json?poll=1";
                if (!since.isEmpty())
                    pollUrl += "&since=" + URLEncoder.encode(since, "UTF-8");

                HttpURLConnection conn = (HttpURLConnection) new URL(pollUrl).openConnection();
                conn.setRequestMethod("GET");
                conn.setConnectTimeout(10000);
                conn.setReadTimeout(30000);
                conn.setRequestProperty("Connection", "close");
                if (conn.getResponseCode() != 200) {
                    conn.disconnect();
                    continue;
                }

                StringBuilder sb = new StringBuilder();
                try (BufferedReader br = new BufferedReader(
                        new InputStreamReader(conn.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line);
                }
                conn.disconnect();

                JSONArray arr = new JSONArray(sb.toString());
                for (int i = 0; i < arr.length(); ++i) {
                    JSONObject msg = arr.getJSONObject(i);
                    final String id = msg.optString("id", "");
                    if (!id.isEmpty())
                        prefs.edit().putString(sinceKey, id).apply();

                    if (priming) continue;
                    if (!"message".equals(msg.optString("event", "message"))) continue;
                    if (!ownTag.isEmpty() && hasTag(msg, ownTag)) continue;

                    final long now = System.currentTimeMillis();
                    final long last = prefs.getLong("lastNotify_" + topic, 0L);
                    if (now - last < NOTIFY_COOLDOWN_MS) continue;
                    prefs.edit().putLong("lastNotify_" + topic, now).apply();

                    // Aucun contenu de liste : juste le signal, l'app lira le chiffré.
                    Platform.showNotification(DeferredPushJob.this,
                            "Liste mise à jour",
                            "Ouvrez l'app pour voir le détail.");
                }
            } catch (Exception e) {
                Log.w(TAG, "topic poll failed: " + topic, e);
            }
        }
    }

    private static boolean hasTag(JSONObject msg, String tag) {
        JSONArray tags = msg.optJSONArray("tags");
        if (tags == null) return false;
        for (int i = 0; i < tags.length(); ++i)
            if (tag.equals(tags.optString(i, ""))) return true;
        return false;
    }
}
