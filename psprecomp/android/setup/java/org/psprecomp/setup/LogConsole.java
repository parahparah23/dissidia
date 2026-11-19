package org.psprecomp.setup;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.GestureDetector;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.OverScroller;

import java.util.ArrayList;

/**
 * The live log: the output of the build tools as it arrives, in a dark glass panel. Lines wrap to the width; errors are red, warnings amber,
 * the build's own progress lines gold. It follows the newest line until the player scrolls up (drag / fling, the d-pad or L / R when it has
 * focus); a "LIVE" tag at the bottom brings it back. Keeps the last 4000 lines (the full log is the file).
 */
final class LogConsole extends View {
    private static final int MAX = 4000;
    private final ArrayList<String> lines = new ArrayList<>();
    private final ArrayList<int[]> rows = new ArrayList<>();   // {line index, start, end}
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG), paint = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final OverScroller scroller;
    private final GestureDetector gestures;
    private float scrollPos = 0;   // pixels from the top
    private boolean follow = true;
    private int cols = 80, wrappedFor = -1;
    private float lineH, charW;
    private final int pad = Theme.dp(12);
    private final Path frame = new Path();
    private String emptyText = "Waiting for output...";
    private float focusT = 0;
    private boolean live = true;   // false: a finished file (the tag says END instead of LIVE)

    void setLive(boolean l) {
        live = l;
        invalidate();
    }

    LogConsole(Context c) {
        super(c);
        text.setTypeface(Theme.mono);
        setTextSize(11.5f);
        stroke.setStyle(Paint.Style.STROKE);
        scroller = new OverScroller(c);
        setFocusable(true);
        gestures = new GestureDetector(c, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDown(MotionEvent e) {
                scroller.forceFinished(true);
                return true;
            }

            @Override
            public boolean onScroll(MotionEvent a, MotionEvent b, float dx, float dy) {
                getParent().requestDisallowInterceptTouchEvent(true);
                scrollBy(dy);
                return true;
            }

            @Override
            public boolean onFling(MotionEvent a, MotionEvent b, float vx, float vy) {
                scroller.fling(0, (int) scrollPos, 0, (int) -vy, 0, 0, 0, (int) maxScroll());
                follow = false;
                postInvalidateOnAnimation();
                return true;
            }

            @Override
            public boolean onSingleTapUp(MotionEvent e) {
                if (!follow && e.getY() > getHeight() - Theme.dp(44)) {   // the LIVE tag
                    follow = true;
                    invalidate();
                    return true;
                }
                return false;
            }
        });
    }

    void setTextSize(float sp) {
        text.setTextSize(sp * getResources().getDisplayMetrics().scaledDensity);
        Paint.FontMetrics fm = text.getFontMetrics();
        lineH = (fm.descent - fm.ascent) * 1.12f;
        charW = text.measureText("M");
        wrappedFor = -1;
        invalidate();
    }

    void setEmptyText(String s) {
        emptyText = s;
        invalidate();
    }

    void clear() {
        lines.clear();
        rows.clear();
        scrollPos = 0;
        follow = true;
        invalidate();
    }

    void append(String[] add) {
        if (add == null || add.length == 0) return;
        for (String s : add) lines.add(s.replace('\t', ' ').replace("\r", ""));
        if (lines.size() > MAX) {
            lines.subList(0, lines.size() - MAX).clear();
            wrappedFor = -1;
        } else if (wrappedFor == cols) {
            for (int i = lines.size() - add.length; i < lines.size(); i++) wrap(i);
        }
        invalidate();
    }

    void setAll(java.util.List<String> all) {
        lines.clear();
        int from = Math.max(0, all.size() - MAX);
        for (int i = from; i < all.size(); i++) lines.add(all.get(i).replace('\t', ' ').replace("\r", ""));
        wrappedFor = -1;
        invalidate();
    }

    private void wrap(int li) {
        String s = lines.get(li);
        if (s.isEmpty()) { rows.add(new int[] {li, 0, 0}); return; }
        int st = 0;
        while (st < s.length()) {   // wrap at a space or a path separator when there is one in the last third of the row
            int end = Math.min(s.length(), st + cols);
            if (end < s.length()) {
                for (int k = end - 1; k > st + cols * 2 / 3; k--) {
                    char ch = s.charAt(k);
                    if (ch == ' ' || ch == '/' || ch == ',') { end = k + 1; break; }
                }
            }
            rows.add(new int[] {li, st, end});
            st = end;
        }
    }

    private void rewrap() {
        rows.clear();
        for (int i = 0; i < lines.size(); i++) wrap(i);
        wrappedFor = cols;
    }

    private float contentH() {
        return rows.size() * lineH + pad * 2;
    }

    private float maxScroll() {
        return Math.max(0, contentH() - getHeight());
    }

    private void scrollBy(float dy) {
        scrollPos = Math.max(0, Math.min(maxScroll(), scrollPos + dy));
        follow = scrollPos >= maxScroll() - lineH;
        invalidate();
    }

    void page(int dir) {
        scrollBy(dir * (getHeight() - lineH * 3));
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        boolean r = gestures.onTouchEvent(e);
        if (e.getActionMasked() == MotionEvent.ACTION_UP || e.getActionMasked() == MotionEvent.ACTION_CANCEL)
            getParent().requestDisallowInterceptTouchEvent(false);
        return r || super.onTouchEvent(e);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent e) {
        switch (keyCode) {
        case KeyEvent.KEYCODE_DPAD_UP:
            if (scrollPos <= 0) return super.onKeyDown(keyCode, e);   // at the top: let focus move on
            scrollBy(-lineH * 3);
            return true;
        case KeyEvent.KEYCODE_DPAD_DOWN:
            if (follow) return super.onKeyDown(keyCode, e);
            scrollBy(lineH * 3);
            return true;
        case KeyEvent.KEYCODE_PAGE_UP:
        case KeyEvent.KEYCODE_BUTTON_L1:
            page(-1);
            return true;
        case KeyEvent.KEYCODE_PAGE_DOWN:
        case KeyEvent.KEYCODE_BUTTON_R1:
            page(1);
            return true;
        case KeyEvent.KEYCODE_MOVE_END:
            follow = true;
            invalidate();
            return true;
        }
        return super.onKeyDown(keyCode, e);
    }

    @Override
    protected void onFocusChanged(boolean gain, int dir, android.graphics.Rect prev) {
        super.onFocusChanged(gain, dir, prev);
        focusT = gain ? 1 : 0;
        invalidate();
    }

    private int colorOf(String s) {
        String l = s.toLowerCase();
        if (l.startsWith("failed") || l.contains("error") || l.contains("traceback") || l.contains("exception") || l.contains("fatal")) return Theme.DANGER;
        if (l.contains("warning") || l.contains("warn:")) return Theme.WARN;
        if (s.startsWith("[psp-build]") || s.startsWith("=====") || s.startsWith("[update]")) return Theme.GOLD;
        if (s.startsWith("$ ") || s.startsWith("+ ")) return Theme.SKY;
        return 0xFFC9CFDC;
    }

    @Override
    protected void onDraw(Canvas c) {
        if (getWidth() < 4 || getHeight() < 4) return;   // not laid out (yet): gradients need a size
        float w = getWidth(), h = getHeight();
        Card.chamferPath(frame, 1, 1, w - 1, h - 1, Theme.dp(10));
        paint.setColor(0xD0050810);
        c.drawPath(frame, paint);
        stroke.setStrokeWidth(Theme.dpf(1));
        stroke.setColor(Theme.alpha(Theme.GOLD, 0.25f + 0.55f * focusT));
        c.drawPath(frame, stroke);

        int nc = Math.max(20, (int) ((w - pad * 2) / charW));
        if (nc != cols || wrappedFor != nc) {
            cols = nc;
            rewrap();
        }
        if (scroller.computeScrollOffset()) {
            scrollPos = scroller.getCurrY();
            postInvalidateOnAnimation();
        }
        if (follow) scrollPos = maxScroll();
        c.save();
        c.clipPath(frame);
        if (rows.isEmpty()) {
            text.setColor(Theme.TEXT_FAINT);
            c.drawText(emptyText, pad, pad - text.ascent(), text);
        }
        int first = Math.max(0, (int) ((scrollPos - pad) / lineH));
        float y = pad + first * lineH - scrollPos - text.ascent();
        for (int i = first; i < rows.size() && y - lineH < h; i++, y += lineH) {
            int[] r = rows.get(i);
            String s = lines.get(r[0]);
            text.setColor(colorOf(s));
            c.drawText(s, r[1], r[2], pad, y, text);
        }
        // fade at the top edge
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        paint.setShader(new android.graphics.LinearGradient(0, 0, 0, pad * 2, 0xD0050810, 0, android.graphics.Shader.TileMode.CLAMP));
        c.drawRect(0, 0, w, pad * 2, paint);
        paint.setShader(null);
        // scroll thumb
        if (contentH() > h) {
            float th = Math.max(Theme.dp(24), h * h / contentH()), ty = (h - th) * (scrollPos / Math.max(1, maxScroll()));
            paint.setColor(0x50E2C47A);
            c.drawRoundRect(w - Theme.dp(5), ty + 2, w - Theme.dp(2), ty + th - 2, Theme.dp(2), Theme.dp(2), paint);
        }
        // LIVE tag / paused tag
        String tag = follow ? (live ? "\u25CF LIVE" : "END") : live ? "\u2193 BACK TO LIVE" : "\u2193 TO THE END";
        text.setTypeface(Theme.bodyBold);
        float tw = text.measureText(tag) + Theme.dp(16), th = Theme.dp(22);
        float tx = w - tw - Theme.dp(10), ty = h - th - Theme.dp(10);
        paint.setColor(follow ? 0x60102A26 : 0xE0283454);
        c.drawRoundRect(tx, ty, tx + tw, ty + th, th / 2, th / 2, paint);
        float blink = follow ? 0.6f + 0.4f * (float) Math.sin(System.currentTimeMillis() / 300.0) : 1;
        text.setColor(Theme.alpha(follow ? Theme.CRYSTAL : Theme.GOLD_LIGHT, follow ? blink : 1));
        c.drawText(tag, tx + Theme.dp(8), ty + th / 2 - (text.ascent() + text.descent()) / 2, text);
        text.setTypeface(Theme.mono);
        c.restore();
        if (follow && live) postInvalidateDelayed(120);   // the LIVE dot breathes
    }
}
