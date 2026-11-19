package org.psprecomp.setup;

import android.content.Context;
import android.os.Build;

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Crash reports of the setup app itself: an uncaught exception writes files/crash/crash-<time>.txt (device, versions, the stack trace and
 * the last lines of a running build) before Android closes the app. The next start offers to share or save it once; Logs and reports lists
 * the last five.
 */
final class CrashReports {
    private static boolean installed;

    static void install(Context c) {
        if (installed) return;
        installed = true;
        final Context app = c.getApplicationContext();
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try {
                write(app, t, e);
            } catch (Throwable ignored) {
                // nothing more to do
            }
            if (prev != null) prev.uncaughtException(t, e);
        });
    }

    private static File dir(Context c) {
        File d = new File(c.getFilesDir(), "crash");
        d.mkdirs();
        return d;
    }

    private static void write(Context c, Thread t, Throwable e) throws Exception {
        StringWriter sw = new StringWriter();
        PrintWriter w = new PrintWriter(sw);
        String ver = "";
        try {
            ver = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Exception x) {
            // unknown
        }
        w.println("Dissidia Recomp setup crash report");
        w.println("time:    " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date()));
        w.println("app:     " + c.getPackageName() + " " + ver);
        w.println("device:  " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + "), Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")");
        w.println("abis:    " + Arrays.toString(Build.SUPPORTED_ABIS));
        w.println("thread:  " + t.getName());
        w.println();
        e.printStackTrace(w);
        BuildRunner r = BuildRunner.current;
        if (r != null) {
            w.println();
            w.println("---- the running build (" + r.kind + "), last lines ----");
            synchronized (r.history) {
                List<String> h = r.history;
                for (String s : h.subList(Math.max(0, h.size() - 200), h.size())) w.println(s);
            }
        }
        w.flush();
        String name = "crash-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".txt";
        try (FileOutputStream o = new FileOutputStream(new File(dir(c), name))) {
            o.write(sw.toString().getBytes("UTF-8"));
        }
    }

    /** The reports, newest first (older than the fifth are deleted). */
    static List<File> list(Context c) {
        File[] fs = dir(c).listFiles((d, n) -> n.startsWith("crash-") && n.endsWith(".txt"));
        ArrayList<File> l = new ArrayList<>();
        if (fs == null) return l;
        Arrays.sort(fs, (a, b) -> b.getName().compareTo(a.getName()));
        for (int i = 0; i < fs.length; i++) {
            if (i < 5) l.add(fs[i]);
            else {
                fs[i].delete();
                new File(fs[i].getPath() + ".seen").delete();
            }
        }
        return l;
    }

    /** The newest report not offered yet, or null. */
    static File unseen(Context c) {
        List<File> l = list(c);
        return !l.isEmpty() && !new File(l.get(0).getPath() + ".seen").exists() ? l.get(0) : null;
    }

    static void markSeen(File f) {
        try {
            new File(f.getPath() + ".seen").createNewFile();
        } catch (Exception e) {
            // offered again next time
        }
    }

    private CrashReports() {}
}
