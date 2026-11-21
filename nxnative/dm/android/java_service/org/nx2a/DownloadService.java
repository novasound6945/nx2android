package org.nx2a;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import java.lang.ref.WeakReference;

/**
 * The game data's download kept alive off the screen: a foreground service (type dataSync) whose notification shows the progress and takes Pause / Resume / Cancel.
 * The download is the native code's (datasync_android.c on datasync.h: the list, resumed parts, hashes); this class only keeps the process in the foreground while it
 * runs, holds a partial wake lock and a Wi-Fi lock while bytes come, and passes the buttons and the network's comings and goings to the native side (nativeCommand,
 * registered by datasync_android.c with RegisterNatives). The native side posts every state through update(); its words come from there (bgdl.h bg_note).
 * Built into every APK by mkapk.py and porter apk-update (a dex of its own: nxnative/dm/android/apk/service.dex, build_service_dex.py). Its numbers are bgdl.h's.
 */
public final class DownloadService extends Service {
    static final int ST_RUN = 1;
    static final int ST_PAUSED = 2;
    static final int ST_WAIT_NET = 3;
    static final int ST_WAIT = 4;
    static final int ST_DONE = 5;
    static final int CMD_PAUSE = 1;
    static final int CMD_RESUME = 2;
    static final int CMD_CANCEL = 3;
    static final int CMD_NET_UP = 4;
    static final int CMD_NET_DOWN = 5;
    static final int CMD_STOPPED = 6;

    private static final String CHANNEL = "nx2a.download";
    private static final int ID = 0x6e7801, ID_DONE = 0x6e7802, REQ_PERM = 0x6e78;
    private static final String ACTION = "org.nx2a.DownloadService.CMD", EXTRA = "cmd";
    private static final int TYPE_DATA_SYNC = 1;                // ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC (API 29)
    private static final int FLAG_IMMUTABLE = 0x04000000;       // PendingIntent.FLAG_IMMUTABLE (API 23)
    private static final int WIFI_MODE_FULL_HIGH_PERF = 3;
    private static final String POST_NOTIFICATIONS = "android.permission.POST_NOTIFICATIONS";

    static native void nativeCommand(int cmd);

    private static final Object LOCK = new Object();
    private static DownloadService self;        // the running service (under LOCK)
    private static Notification shown;          // the notification the service starts with (under LOCK)
    private static int shownState;
    private static boolean starting;
    private static WeakReference<Activity> activity = new WeakReference<Activity>(null);

    private PowerManager.WakeLock wake;
    private WifiManager.WifiLock wifi;
    private ConnectivityManager.NetworkCallback net;

    private static void command(int cmd) {
        try { nativeCommand(cmd); } catch (UnsatisfiedLinkError e) { /* the native side did not register (a host without it) */ }
    }

    /** The activity the native start screen runs in (null once it is destroyed): where the notification permission is asked from. Any thread */
    public static void attach(Activity a) {
        synchronized (LOCK) { activity = new WeakReference<Activity>(a); }
    }

    /** Whether the app may post its notification (Android 13+: POST_NOTIFICATIONS granted; before it: notifications not turned off) */
    public static boolean notificationsAllowed(Context ctx) {
        try {
            if (Build.VERSION.SDK_INT >= 33 && ctx.checkSelfPermission(POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return false;
            NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
            return nm == null || nm.areNotificationsEnabled();
        } catch (RuntimeException e) { return true; }
    }

    /** Android 13+: asks for POST_NOTIFICATIONS once (the first download; never again after an answer), from the attached activity's UI thread. Any thread */
    public static void askNotifications(Context ctx) {
        if (Build.VERSION.SDK_INT < 33) return;
        final Activity a;
        synchronized (LOCK) { a = activity.get(); }
        if (a == null || ctx.checkSelfPermission(POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return;
        final SharedPreferences p = ctx.getSharedPreferences("nx2a_download", Context.MODE_PRIVATE);
        if (p.getBoolean("asked", false)) return;
        p.edit().putBoolean("asked", true).apply();
        a.runOnUiThread(new Ask(a));
    }

    // (named classes, not anonymous ones: d8 of build-tools 32 cannot read javac 25's anonymous classes)
    private static final class Ask implements Runnable {
        private final Activity a;
        Ask(Activity a) { this.a = a; }
        @Override public void run() {
            if (a.isFinishing() || a.isDestroyed()) return;
            try { a.requestPermissions(new String[] {POST_NOTIFICATIONS}, REQ_PERM); } catch (RuntimeException e) { /* no dialog: the download goes on without the notification */ }
        }
    }

    private static final class Net extends ConnectivityManager.NetworkCallback {
        @Override public void onAvailable(Network n) { command(CMD_NET_UP); }
        @Override public void onLost(Network n) { command(CMD_NET_DOWN); }
    }

    /**
     * The download's state for the notification (any thread). The first call starts the service (the app in front: Android 12+ refuses a start from the background);
     * ST_DONE leaves a notification of its own ("tap to start") and ends the service. → false when the service is not running and could not be started
     */
    public static boolean update(Context ctx, int state, String title, String text, int permille, String pause, String cancel) {
        Notification n = build(ctx, state, title, text, permille, pause, cancel);
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (state == ST_DONE) {
            stop(ctx);
            try { if (nm != null) nm.notify(ID_DONE, n); } catch (RuntimeException e) { /* notifications off */ }
            return true;
        }
        DownloadService s;
        synchronized (LOCK) {
            shown = n; shownState = state; s = self;
            if (s == null && starting) return true;   // onCreate posts it
            if (s == null) starting = true;
        }
        if (s != null) {
            try { if (nm != null) nm.notify(ID, n); } catch (RuntimeException e) { /* notifications off: the service runs on */ }
            s.locks(state == ST_RUN);
            return true;
        }
        try {
            if (nm != null) nm.cancel(ID_DONE);
            Intent i = new Intent(ctx, DownloadService.class);
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i); else ctx.startService(i);
            return true;
        } catch (RuntimeException e) {   // ForegroundServiceStartNotAllowedException (in the background), SecurityException
            synchronized (LOCK) { starting = false; }
            return false;
        }
    }

    /** The service ends and its notification goes (the download ended, or the game starts). Any thread */
    public static void stop(Context ctx) {
        DownloadService s;
        synchronized (LOCK) { s = self; starting = false; shown = null; }
        if (s != null) s.end();
        else {
            try { ctx.stopService(new Intent(ctx, DownloadService.class)); } catch (RuntimeException e) { /* not running */ }
        }
    }

    /** The finished download's notification goes (the app is back on the screen). Any thread */
    public static void clearDone(Context ctx) {
        try { ((NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE)).cancel(ID_DONE); } catch (RuntimeException e) { /* none */ }
    }

    private static Notification build(Context ctx, int state, String title, String text, int permille, String pause, String cancel) {
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null && Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, isKorean() ? "게임 데이터 다운로드" : "Game data download", NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(ctx, CHANNEL) : new Notification.Builder(ctx);
        b.setContentTitle(title).setContentText(text).setOnlyAlertOnce(true).setShowWhen(false)
         .setSmallIcon(state == ST_DONE ? android.R.drawable.stat_sys_download_done : android.R.drawable.stat_sys_download);
        if (Build.VERSION.SDK_INT >= 24) b.setStyle(new Notification.BigTextStyle().bigText(text));
        Intent launch = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
        if (launch != null) {
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            b.setContentIntent(PendingIntent.getActivity(ctx, 0, launch, PendingIntent.FLAG_UPDATE_CURRENT | FLAG_IMMUTABLE));
        }
        if (state == ST_DONE) { b.setAutoCancel(true).setOngoing(false); return b.build(); }
        b.setOngoing(true).setCategory(Notification.CATEGORY_PROGRESS);
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        if (permille >= 0) b.setProgress(1000, Math.min(permille, 1000), false); else if (state == ST_RUN) b.setProgress(0, 0, true);
        if (pause != null && pause.length() > 0) b.addAction(new Notification.Action.Builder(null, pause, action(ctx, state == ST_PAUSED ? CMD_RESUME : CMD_PAUSE)).build());
        if (cancel != null && cancel.length() > 0) b.addAction(new Notification.Action.Builder(null, cancel, action(ctx, CMD_CANCEL)).build());
        return b.build();
    }

    private static boolean isKorean() { return "ko".equals(java.util.Locale.getDefault().getLanguage()); }

    private static PendingIntent action(Context ctx, int cmd) {
        Intent i = new Intent(ctx, DownloadService.class).setAction(ACTION + cmd).putExtra(EXTRA, cmd);
        return PendingIntent.getService(ctx, cmd, i, PendingIntent.FLAG_UPDATE_CURRENT | FLAG_IMMUTABLE);
    }

    @Override public void onCreate() {
        super.onCreate();
        Notification n; int st;
        synchronized (LOCK) { self = this; starting = false; n = shown; st = shownState; }
        if (n == null) n = build(this, ST_RUN, "", "", -1, null, null);
        try {
            if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, TYPE_DATA_SYNC); else startForeground(ID, n);
        } catch (RuntimeException e) {   // (Android 14+: a dataSync service the system does not allow now)
            synchronized (LOCK) { self = null; }
            stopSelf();
            command(CMD_STOPPED);
            return;
        }
        locks(st == ST_RUN);
        watchNetwork();
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && intent.hasExtra(EXTRA)) {
            int cmd = intent.getIntExtra(EXTRA, 0);
            if (cmd == CMD_CANCEL) end();   // the notification goes at once; the native side ends the download (and the app, when it is not on the screen)
            command(cmd);
        }
        return START_NOT_STICKY;   // (the download is the process's: a service made again without it would have nothing to show)
    }

    /** Android 15+: a dataSync service's time for the day is up. It ends; the download goes on while the process lives */
    public void onTimeout(int startId, int fgsType) { end(); command(CMD_STOPPED); }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public void onDestroy() {
        locks(false);
        if (net != null) {
            try { ((ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE)).unregisterNetworkCallback(net); } catch (RuntimeException e) { /* gone */ }
            net = null;
        }
        boolean was;
        synchronized (LOCK) { was = self == this; if (was) self = null; }
        super.onDestroy();
    }

    private void end() {
        synchronized (LOCK) { if (self == this) self = null; }
        locks(false);
        try { if (Build.VERSION.SDK_INT >= 24) stopForeground(Service.STOP_FOREGROUND_REMOVE); else stopForeground(true); } catch (RuntimeException e) { /* not in the foreground */ }
        stopSelf();
    }

    // the wake lock and the Wi-Fi lock: held only while bytes come (not paused, not waiting)
    private synchronized void locks(boolean on) {
        try {
            if (on) {
                if (wake == null) { wake = ((PowerManager) getSystemService(Context.POWER_SERVICE)).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nx2a:download"); wake.setReferenceCounted(false); }
                if (!wake.isHeld()) wake.acquire();
                WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wifi == null && wm != null) { wifi = wm.createWifiLock(WIFI_MODE_FULL_HIGH_PERF, "nx2a:download"); wifi.setReferenceCounted(false); }
                if (wifi != null && !wifi.isHeld()) wifi.acquire();
            } else {
                if (wake != null && wake.isHeld()) wake.release();
                if (wifi != null && wifi.isHeld()) wifi.release();
            }
        } catch (RuntimeException e) { /* a permission the APK lacks: the download runs without the lock */ }
    }

    // the device's default network coming and going, to the native side: a download waiting for a connection goes on the moment there is one (no timed retries)
    private void watchNetwork() {
        try {
            net = new Net();
            ((ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE)).registerDefaultNetworkCallback(net);
        } catch (RuntimeException e) { net = null; /* the native side looks at the network itself (dm_net_state) */ }
    }
}
