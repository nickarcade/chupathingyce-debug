package com.halo.decomp;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.util.Log;
import rikka.shizuku.Shizuku;

public final class ShizukuHelper {
    private static final String TAG = "chupathingyce-shizuku";
    private static final int SHIZUKU_REQUEST_CODE = 1001;

    private static final Shizuku.OnRequestPermissionResultListener PERMISSION_LISTENER =
            new Shizuku.OnRequestPermissionResultListener() {
                @Override
                public void onRequestPermissionResult(int requestCode, int grantResult) {
                    if (requestCode == SHIZUKU_REQUEST_CODE) {
                        if (grantResult == PackageManager.PERMISSION_GRANTED) {
                            Log.i(TAG, "Shizuku permission granted; uid=2000 tooling available.");
                        } else {
                            Log.w(TAG, "Shizuku permission denied; raw USB input is unaffected.");
                        }
                    }
                }
            };

    private static boolean listenerRegistered = false;

    public static void init(Activity activity) {
        try {
            if (!Shizuku.pingBinder()) {
                Log.d(TAG, "Shizuku is not running; the gamepad does not need it.");
                return;
            }

            if (!listenerRegistered) {
                Shizuku.addRequestPermissionResultListener(PERMISSION_LISTENER);
                listenerRegistered = true;
            }

            if (Shizuku.isPreV11()) {
                Log.d(TAG, "Shizuku pre-v11 unsupported.");
                return;
            }

            int permission = Shizuku.checkSelfPermission();
            if (permission == PackageManager.PERMISSION_GRANTED) {
                Log.i(TAG, "Shizuku permission already granted.");
            } else if (Shizuku.shouldShowRequestPermissionRationale()) {
                Log.i(TAG, "Showing Shizuku request permission rationale...");
                Shizuku.requestPermission(SHIZUKU_REQUEST_CODE);
            } else {
                Log.i(TAG, "Requesting Shizuku permission...");
                Shizuku.requestPermission(SHIZUKU_REQUEST_CODE);
            }
        } catch (Throwable t) {
            Log.e(TAG, "Shizuku initialization exception: " + t.getMessage());
        }
    }

    public static boolean isAvailable() {
        try {
            return Shizuku.pingBinder() && Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED;
        } catch (Throwable t) {
            return false;
        }
    }
}
