package org.psprecomp.setup;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;

/** HTTPS downloads through Android's own stack (the bundled Python is musl: no resolver config, no CA store on Android). Blocking. */
final class Downloader {
    interface Listener {
        void progress(long done, long total);
    }

    private volatile boolean cancelled;

    void cancel() {
        cancelled = true;
    }

    boolean cancelled() {
        return cancelled;
    }

    /** Downloads url to dest (through dest.part). Returns null when done, else why not ("cancelled", "HTTP 404", ...). */
    String fetch(String url, File dest, Listener l) {
        URLConnection c = null;
        File part = new File(dest.getPath() + ".part");
        try {
            c = new URL(url).openConnection();   // a file: URL works too (the test switch files/manifest_url.txt)
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            if (c instanceof HttpURLConnection) {
                HttpURLConnection h = (HttpURLConnection) c;
                h.setInstanceFollowRedirects(true);
                int code = h.getResponseCode();
                if (code != 200) return "HTTP " + code;
            }
            long total = c.getContentLengthLong();
            try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(part)) {
                byte[] buf = new byte[1 << 16];
                long done = 0, lastReport = 0;
                int n;
                while ((n = in.read(buf)) > 0) {
                    if (cancelled) {
                        out.close();
                        part.delete();
                        return "cancelled";
                    }
                    out.write(buf, 0, n);
                    done += n;
                    if (l != null && done - lastReport > (1 << 18)) {
                        l.progress(done, total);
                        lastReport = done;
                    }
                }
                if (l != null) l.progress(done, total);
            }
            dest.delete();
            if (!part.renameTo(dest)) return "cannot write " + dest;
            return null;
        } catch (Exception e) {
            part.delete();
            return cancelled ? "cancelled" : e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
        } finally {
            if (c instanceof HttpURLConnection) ((HttpURLConnection) c).disconnect();
        }
    }
}
