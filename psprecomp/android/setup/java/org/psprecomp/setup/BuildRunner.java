package org.psprecomp.setup;

import android.os.Handler;
import android.os.Looper;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;

/**
 * Runs a plan (Engine.plan) on its own thread, one step after the other: commands through the engine (their output becomes progress there),
 * download steps with Downloader. Every output line goes to setup.log in the work folder and, as it arrives, to the listener (the live log
 * on the page). One runner at a time (current); the page reattaches to it after a rotation or when the app comes back.
 */
final class BuildRunner {
    interface Listener {
        void lines(String[] lines);
        void progress(JSONObject p);
        void finished(boolean ok, String error, String note);
    }

    static volatile BuildRunner current;
    /** Exists while a plan runs (set by the activity): found at the next start with no runner = the app was killed or crashed mid-install. */
    static volatile File marker;

    final String kind;
    final JSONArray plan;
    final File log;
    final ArrayList<String> history = new ArrayList<>();   // what the live log shows (capped)
    private final ArrayDeque<String> recent = new ArrayDeque<>();   // the last lines (the failure page shows them)
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile Listener listener;
    private volatile boolean cancel;
    private Downloader dl;
    volatile boolean running = true, ok = false;
    volatile String error = "", note = "";
    volatile JSONObject lastProgress;
    private Writer out;

    BuildRunner(String kind, JSONArray plan, File log) {
        this.kind = kind;
        this.plan = plan;
        this.log = log;
    }

    void setListener(Listener l) {
        listener = l;
    }

    String recentText() {
        synchronized (recent) {
            StringBuilder b = new StringBuilder();
            for (String s : recent) b.append(s).append('\n');
            return b.toString().trim();
        }
    }

    void start() {
        current = this;
        new Thread(this::run, "setup-build").start();
    }

    void cancel() {
        cancel = true;
        Engine.kill();
        Downloader d = dl;
        if (d != null) d.cancel();
    }

    private void log(String s) {
        synchronized (history) {
            history.add(s);
            if (history.size() > 6000) history.subList(0, 1000).clear();
        }
        if (!s.trim().isEmpty() && !s.startsWith("FAILED:")) {
            synchronized (recent) {
                recent.addLast(s);
                while (recent.size() > 14) recent.removeFirst();
            }
        }
        try {
            if (out != null) {
                out.write(s);
                out.write('\n');
                out.flush();   // a line is on disk before the next runs: a crash or a kill still leaves the log
            }
        } catch (Exception e) {
            // the log file is a convenience
        }
    }

    private void emit(String[] lines) {
        final Listener l = listener;
        if (l != null && lines != null) ui.post(() -> { if (listener == l) l.lines(lines); });
    }

    private long lastProg = 0;

    private void emitProgress(boolean force) {
        long now = System.currentTimeMillis();
        if (!force && now - lastProg < 200) return;
        lastProg = now;
        try {
            JSONObject p = new JSONObject(Engine.progress());
            lastProgress = p;
            final Listener l = listener;
            if (l != null) ui.post(() -> { if (listener == l) l.progress(p); });
        } catch (Exception e) {
            // never mind one frame
        }
    }

    private void say(String s) {
        log(s);
        emit(new String[] {s});
    }

    private void run() {
        try {
            log.getParentFile().mkdirs();
            out = new OutputStreamWriter(new FileOutputStream(log, true), "UTF-8");
        } catch (Exception e) {
            out = null;
        }
        try {
            File m = marker;
            if (m != null) {
                m.getParentFile().mkdirs();
                try (FileOutputStream mo = new FileOutputStream(m)) {
                    mo.write((kind + "\n").getBytes("UTF-8"));
                }
            }
        } catch (Exception e) {
            // no marker: the next start cannot tell
        }
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        say("===== psprecomp_setup run (" + kind + ", " + stamp + ") =====");
        String fail = null;
        try {
            for (int i = 0; i < plan.length() && fail == null; i++) {
                JSONObject step = plan.getJSONObject(i);
                say("===== step " + (i + 1) + "/" + plan.length() + ": " + step.optString("label") + " =====");
                String err = Engine.startStep(i);
                emitProgress(true);
                if (!err.isEmpty()) { fail = "Could not start: " + err; break; }
                JSONArray dls = step.optJSONArray("downloads");
                int code = 0;
                if (dls != null && dls.length() > 0) {
                    String r = downloads(dls);
                    if (r != null) {
                        fail = cancel ? "The installation was cancelled." : "The download failed (" + r + "). Check your connection and try again; finished files are kept.";
                        break;
                    }
                } else {
                    while (true) {
                        String[] lines = Engine.readLines();
                        if (lines != null) {
                            for (String s : lines) log(s);
                            emit(lines);
                        }
                        emitProgress(false);
                        if (lines == null && !Engine.busy()) break;
                        if (lines == null) Thread.sleep(60);
                    }
                    code = Engine.exitCode();
                }
                if (cancel) { fail = "The installation was cancelled."; break; }
                JSONObject p = new JSONObject(Engine.progress());
                String perr = p.optString("error");
                if (code != 0 || !perr.isEmpty()) {
                    if (step.optBoolean("optional")) {
                        String why = perr.isEmpty() ? p.optString("last") : perr;
                        note = step.optString("label") + " did not work" + (why.isEmpty() ? "." : ": " + why);
                        say("optional step failed: " + note);
                        Engine.clearError();
                    } else {
                        fail = perr.isEmpty() ? "A step failed (exit code " + code + "). The log shows what happened." : perr + ".";
                    }
                }
            }
        } catch (Exception e) {
            fail = "The setup stopped: " + e;
        }
        if (fail != null) say("FAILED: " + fail);
        else say("===== finished =====");
        emitProgress(true);
        try {
            if (out != null) out.close();
        } catch (Exception e) {
            // closed anyway
        }
        File m = marker;
        if (m != null) m.delete();
        ok = fail == null;
        error = fail == null ? "" : fail;
        running = false;
        final Listener l = listener;
        if (l != null) ui.post(() -> { if (listener == l) l.finished(ok, error, note); });
    }

    /** A download step: every file that is not there yet (a finished one from an earlier try is kept). null = done. */
    private String downloads(JSONArray dls) throws Exception {
        int n = dls.length();
        for (int k = 0; k < n; k++) {
            JSONObject d = dls.getJSONObject(k);
            File f = new File(d.getString("file"));
            if (f.isFile()) { say("already downloaded: " + f.getName()); continue; }
            f.getParentFile().mkdirs();
            final int idx = k;
            say("downloading " + d.getString("url"));
            dl = new Downloader();
            if (cancel) return "cancelled";
            final long[] lastLog = {0};
            String r = dl.fetch(d.getString("url"), f, (done, total) -> {
                double frac = total > 0 ? (double) done / total : 0;
                String detail = "File " + (idx + 1) + " of " + n + ": " + size(done) + (total > 0 ? " of " + size(total) : "");
                Engine.noteDownload((idx + frac) / n, detail);
                if (done - lastLog[0] >= (50L << 20)) {   // a log line every 50 MB
                    lastLog[0] = done;
                    say("  " + detail);
                }
                emitProgress(false);
            });
            dl = null;
            if (r != null) return r;
            say("  done: " + f.getName() + " (" + size(f.length()) + ")");
        }
        return null;
    }

    static String size(long n) {
        if (n >= (1L << 30)) return String.format(Locale.US, "%.1f GB", n / (double) (1L << 30));
        if (n >= (1L << 20)) return String.format(Locale.US, "%.0f MB", n / (double) (1L << 20));
        return (n >> 10) + " KB";
    }
}
