package org.psprecomp.game;

import android.app.Activity;
import android.app.Fragment;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The system document picker (Storage Access Framework) for the save manager (runtime/src/psp_file_dialog.cpp calls the static
 * methods through JNI). A headless Fragment added to SDL's activity starts the picker and receives its answer, so SDLActivity
 * itself needs no change. Picked documents are content:// URIs: imports are copied into a folder the game can read, exports are
 * written by the game into a folder of its own and copied out to the picked place.
 */
public class FilePicker extends Fragment {
    private static final int REQ = 0x5A7E;
    private static volatile String sResult = "";     // "" while open; "!cancel", "!error <text>" or the URI
    private static final long MAX_BYTES = 512L << 20;

    /** what: import_folder, import_file, export_folder, export_zip. Returns "ok" or an error text. */
    public static String start(final String what, final String title, final String name) {
        final Activity a = (Activity) SDLActivity.getContext();
        if (a == null) return "no activity";
        sResult = "";
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                FilePicker f = new FilePicker();
                Bundle b = new Bundle();
                b.putString("what", what);
                b.putString("title", title);
                b.putString("name", name);
                f.setArguments(b);
                a.getFragmentManager().beginTransaction().add(f, "psprecomp_picker").commitAllowingStateLoss();
            }
        });
        return "ok";
    }

    public static String poll() { return sResult; }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) return;   // recreated after a configuration change: the request is already out
        Bundle b = getArguments();
        String what = b.getString("what", "");
        Intent i;
        if (what.equals("import_folder") || what.equals("export_folder")) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else if (what.equals("import_file")) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
        } else {
            i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            i.putExtra(Intent.EXTRA_TITLE, b.getString("name", "saves.zip"));
        }
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            startActivityForResult(i, REQ);
        } catch (Exception e) {
            sResult = "!error no document picker on this device";
            done();
        }
    }

    @Override public void onActivityResult(int req, int res, Intent data) {
        if (req != REQ) return;
        if (res != Activity.RESULT_OK || data == null || data.getData() == null) sResult = "!cancel";
        else sResult = data.getData().toString();
        done();
    }

    private void done() {
        try { getFragmentManager().beginTransaction().remove(this).commitAllowingStateLoss(); } catch (Exception e) { }
    }

    private static ContentResolver cr() { return SDLActivity.getContext().getContentResolver(); }

    private static long copy(InputStream in, OutputStream out, long budget) throws Exception {
        byte[] buf = new byte[65536];
        long n = 0;
        try {
            for (int r; (r = in.read(buf)) > 0; ) {
                n += r;
                if (n > budget) throw new Exception("too big for saves");
                out.write(buf, 0, r);
            }
        } finally {
            in.close();
            out.close();
        }
        return n;
    }

    private static boolean plainName(String n) {
        return n != null && !n.isEmpty() && !n.equals(".") && !n.equals("..") && n.indexOf('/') < 0 && n.indexOf('\\') < 0;
    }

    /** Copies a picked document into the file dst. "" or an error text. */
    public static String copyFileIn(String uri, String dst) {
        try {
            File f = new File(dst);
            f.getParentFile().mkdirs();
            copy(cr().openInputStream(Uri.parse(uri)), new FileOutputStream(f), MAX_BYTES);
            return "";
        } catch (Exception e) {
            return "cannot read the picked file: " + e.getMessage();
        }
    }

    /** Copies a picked folder (tree URI) into the folder dst, up to five levels deep. "" or an error text. */
    public static String copyTreeIn(String uri, String dst) {
        try {
            Uri tree = Uri.parse(uri);
            String root = DocumentsContract.getTreeDocumentId(tree);
            long[] budget = {MAX_BYTES};
            int[] files = {0};
            copyDirIn(tree, root, new File(dst), 0, budget, files);
            return "";
        } catch (Exception e) {
            return "cannot read the picked folder: " + e.getMessage();
        }
    }

    private static void copyDirIn(Uri tree, String docId, File dst, int depth, long[] budget, int[] files) throws Exception {
        dst.mkdirs();
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        Cursor c = cr().query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null);
        if (c == null) return;
        try {
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (!plainName(name) || name.startsWith(".")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (depth < 5) copyDirIn(tree, id, new File(dst, name), depth + 1, budget, files);
                } else {
                    if (++files[0] > 20000) throw new Exception("too many files");
                    budget[0] -= copy(cr().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id)), new FileOutputStream(new File(dst, name)), budget[0]);
                }
            }
        } finally {
            c.close();
        }
    }

    /** Writes the file src to the picked document (ACTION_CREATE_DOCUMENT). "" or an error text. */
    public static String copyFileOut(String src, String uri) {
        try {
            copy(new FileInputStream(new File(src)), cr().openOutputStream(Uri.parse(uri), "wt"), Long.MAX_VALUE);
            return "";
        } catch (Exception e) {
            return "cannot write the picked file: " + e.getMessage();
        }
    }

    /** Copies the folders inside src (one level of files each) into the picked folder; existing files are replaced. */
    public static String copyTreeOut(String src, String uri) {
        try {
            Uri tree = Uri.parse(uri);
            Uri root = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
            File[] dirs = new File(src).listFiles();
            if (dirs == null) return "nothing to export";
            for (File d : dirs) {
                if (!d.isDirectory()) continue;
                Uri dir = findChild(tree, root, d.getName());
                if (dir == null) dir = DocumentsContract.createDocument(cr(), root, DocumentsContract.Document.MIME_TYPE_DIR, d.getName());
                if (dir == null) return "cannot create " + d.getName();
                File[] fs = d.listFiles();
                if (fs == null) continue;
                for (File f : fs) {
                    if (!f.isFile()) continue;
                    Uri doc = findChild(tree, dir, f.getName());
                    if (doc == null) doc = DocumentsContract.createDocument(cr(), dir, "application/octet-stream", f.getName());
                    if (doc == null) return "cannot create " + f.getName();
                    copy(new FileInputStream(f), cr().openOutputStream(doc, "wt"), Long.MAX_VALUE);
                }
            }
            return "";
        } catch (Exception e) {
            return "cannot write to the picked folder: " + e.getMessage();
        }
    }

    private static Uri findChild(Uri tree, Uri parent, String name) {
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(parent));
        Cursor c = cr().query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null);
        if (c == null) return null;
        try {
            while (c.moveToNext()) if (name.equals(c.getString(1))) return DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0));
        } finally {
            c.close();
        }
        return null;
    }
}
