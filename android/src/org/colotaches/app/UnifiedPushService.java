package org.colotaches.app;

import android.util.Log;

import java.nio.charset.StandardCharsets;

import org.unifiedpush.android.connector.FailedReason;
import org.unifiedpush.android.connector.PushService;
import org.unifiedpush.android.connector.data.PushEndpoint;
import org.unifiedpush.android.connector.data.PushMessage;

// Réception UnifiedPush, sans Google. Aucun contenu de liste ne transite.
public class UnifiedPushService extends PushService {
    private static final String TAG = "ColoUnifiedPush";

    @Override
    public void onMessage(PushMessage message, String instance) {
        if (message == null) return;
        final String text = new String(message.getContent(), StandardCharsets.UTF_8);
        if (!message.getDecrypted() && !"sync".equals(text)) return;
        Platform.wakeBackgroundSync(this);
        Platform.showNotification(this, "Liste mise à jour",
                "Ouvrez l'app pour voir le détail.");
    }

    @Override
    public void onNewEndpoint(PushEndpoint endpoint, String instance) {
        if (endpoint == null) return;
        Platform.registerUnifiedPushEndpoint(this, endpoint.getUrl());
    }

    @Override
    public void onRegistrationFailed(FailedReason reason, String instance) {
        Log.w(TAG, "registration failed: " + reason);
    }

    @Override
    public void onUnregistered(String instance) {}

    @Override
    public void onTempUnavailable(String instance) {}
}
