package org.colotaches.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

// Réveil best-effort : si le process Qt est encore vivant, il rattrape et merge.
// S'il est mort, le receiver ne charge pas la lib et se contente de rendre la main ;
// la notification locale du JobService reste le signal.
public class BackgroundSyncReceiver extends BroadcastReceiver {
    private static final String TAG = "ColoBackgroundSync";

    @Override
    public void onReceive(Context context, Intent intent) {
        try {
            nativeSync();
        } catch (UnsatisfiedLinkError e) {
            Log.i(TAG, "Qt process absent, sync ignorée");
        }
    }

    private static native void nativeSync();
}
