package org.psprecomp.setup;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

/** Line icons drawn with paths on a 24 x 24 grid (no image resources), in the thin-stroke style of the game's menu glyphs. */
final class Icons {
    static final int NONE = 0, PLAY = 1, DISC = 2, SLIDERS = 3, REFRESH = 4, DOWNLOAD = 5, LOG = 6, SHARE = 7, SAVE = 8, BACK = 9, CHECK = 10,
            WARN = 11, CRYSTAL = 12, FOLDER = 13, INSTALL = 14, QUIT = 15, BUILD = 16, FILE = 17, KEY = 18, RETRY = 19, CHEVRON = 20,
            INFO = 21, CLOSE = 22, EYE = 23, VOICE = 24, SWORD = 25, TEXTURE = 26, UP = 27, STORAGE = 28, BUG = 29;

    static void draw(Canvas c, int icon, float cx, float cy, float size, int color, float stroke) {
        if (icon == NONE) return;
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(color);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(stroke);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        c.save();
        float s = size / 24f;
        c.translate(cx - size / 2, cy - size / 2);
        c.scale(s, s);
        p.setStrokeWidth(stroke / s);
        Path a = new Path();
        switch (icon) {
        case PLAY:
            a.moveTo(8, 5); a.lineTo(19, 12); a.lineTo(8, 19); a.close();
            break;
        case DISC:
            c.drawCircle(12, 12, 9, p); c.drawCircle(12, 12, 2.5f, p);
            a.moveTo(12, 5.5f); a.cubicTo(15, 5.5f, 17.5f, 7.5f, 18.3f, 10);
            break;
        case SLIDERS:
            a.moveTo(4, 7); a.lineTo(20, 7); a.moveTo(4, 12); a.lineTo(20, 12); a.moveTo(4, 17); a.lineTo(20, 17);
            c.drawPath(a, p); a.reset();
            fill(c, p, 9, 7, 2.2f); fill(c, p, 15, 12, 2.2f); fill(c, p, 7, 17, 2.2f);
            break;
        case REFRESH:
        case RETRY:
            c.drawArc(new RectF(4.5f, 4.5f, 19.5f, 19.5f), -60, 290, false, p);
            a.moveTo(15.8f, 2.8f); a.lineTo(16.2f, 6.6f); a.lineTo(12.4f, 7.2f);
            break;
        case DOWNLOAD:
            a.moveTo(12, 4); a.lineTo(12, 15); a.moveTo(7.5f, 10.5f); a.lineTo(12, 15); a.lineTo(16.5f, 10.5f); a.moveTo(5, 19.5f); a.lineTo(19, 19.5f);
            break;
        case INSTALL:
            a.moveTo(12, 3.5f); a.lineTo(12, 13); a.moveTo(8.5f, 9.5f); a.lineTo(12, 13); a.lineTo(15.5f, 9.5f);
            a.moveTo(4, 13); a.lineTo(4, 19); a.lineTo(20, 19); a.lineTo(20, 13);
            break;
        case LOG:
        case FILE:
            a.moveTo(6, 3.5f); a.lineTo(14.5f, 3.5f); a.lineTo(18.5f, 7.5f); a.lineTo(18.5f, 20.5f); a.lineTo(6, 20.5f); a.close();
            a.moveTo(14.5f, 3.5f); a.lineTo(14.5f, 7.5f); a.lineTo(18.5f, 7.5f);
            if (icon == LOG) { a.moveTo(9, 11.5f); a.lineTo(15.5f, 11.5f); a.moveTo(9, 14.5f); a.lineTo(15.5f, 14.5f); a.moveTo(9, 17.5f); a.lineTo(13, 17.5f); }
            break;
        case SHARE:
            c.drawCircle(17.5f, 5.5f, 2.5f, p); c.drawCircle(6.5f, 12, 2.5f, p); c.drawCircle(17.5f, 18.5f, 2.5f, p);
            a.moveTo(8.7f, 10.8f); a.lineTo(15.3f, 6.8f); a.moveTo(8.7f, 13.2f); a.lineTo(15.3f, 17.2f);
            break;
        case SAVE:
            a.moveTo(5, 4); a.lineTo(16.5f, 4); a.lineTo(20, 7.5f); a.lineTo(20, 20); a.lineTo(5, 20); a.close();
            a.moveTo(8, 4); a.lineTo(8, 9); a.lineTo(15, 9); a.lineTo(15, 4); a.moveTo(8, 20); a.lineTo(8, 14); a.lineTo(17, 14); a.lineTo(17, 20);
            break;
        case BACK:
            a.moveTo(15, 5); a.lineTo(8, 12); a.lineTo(15, 19);
            break;
        case CHEVRON:
            a.moveTo(9, 5); a.lineTo(16, 12); a.lineTo(9, 19);
            break;
        case UP:
            a.moveTo(5, 15); a.lineTo(12, 8); a.lineTo(19, 15);
            break;
        case CHECK:
            a.moveTo(5, 12.5f); a.lineTo(10, 17.5f); a.lineTo(19.5f, 7);
            break;
        case WARN:
            a.moveTo(12, 3.5f); a.lineTo(21.5f, 20); a.lineTo(2.5f, 20); a.close(); a.moveTo(12, 9.5f); a.lineTo(12, 14);
            c.drawPath(a, p); a.reset();
            fill(c, p, 12, 17, 1.1f);
            break;
        case CRYSTAL:
            a.moveTo(12, 2); a.lineTo(18.5f, 11); a.lineTo(12, 22); a.lineTo(5.5f, 11); a.close();
            a.moveTo(5.5f, 11); a.lineTo(18.5f, 11); a.moveTo(12, 2); a.lineTo(12, 22);
            break;
        case FOLDER:
            a.moveTo(3.5f, 6); a.lineTo(9.5f, 6); a.lineTo(11.5f, 8); a.lineTo(20.5f, 8); a.lineTo(20.5f, 19); a.lineTo(3.5f, 19); a.close();
            break;
        case STORAGE:
            a.addRoundRect(new RectF(4, 5, 20, 19), 2, 2, Path.Direction.CW); a.moveTo(4, 12); a.lineTo(20, 12);
            c.drawPath(a, p); a.reset();
            fill(c, p, 16.5f, 8.5f, 1); fill(c, p, 16.5f, 15.5f, 1);
            break;
        case QUIT:
            c.drawArc(new RectF(4.5f, 5, 19.5f, 20), -55, 290, false, p);
            a.moveTo(12, 3); a.lineTo(12, 11);
            break;
        case BUILD: {   // a hammer over an anvil-like base
            a.moveTo(6, 19); a.lineTo(14, 11); a.moveTo(11.5f, 8.5f); a.lineTo(16.5f, 3.5f); a.lineTo(20.5f, 7.5f); a.lineTo(15.5f, 12.5f); a.close();
            a.moveTo(4, 21); a.lineTo(9, 21);
            break;
        }
        case KEY:
            c.drawCircle(8, 12, 4, p);
            a.moveTo(12, 12); a.lineTo(21, 12); a.moveTo(18, 12); a.lineTo(18, 15.5f); a.moveTo(21, 12); a.lineTo(21, 15);
            break;
        case INFO:
            c.drawCircle(12, 12, 9, p); a.moveTo(12, 11); a.lineTo(12, 16.5f);
            c.drawPath(a, p); a.reset();
            fill(c, p, 12, 7.8f, 1.1f);
            break;
        case CLOSE:
            a.moveTo(6, 6); a.lineTo(18, 18); a.moveTo(18, 6); a.lineTo(6, 18);
            break;
        case EYE:
            a.moveTo(2.5f, 12); a.cubicTo(6, 6, 18, 6, 21.5f, 12); a.cubicTo(18, 18, 6, 18, 2.5f, 12);
            c.drawCircle(12, 12, 3, p);
            break;
        case VOICE:
            a.addRoundRect(new RectF(9, 3, 15, 14), 3, 3, Path.Direction.CW);
            a.moveTo(5.5f, 11); a.cubicTo(5.5f, 19, 18.5f, 19, 18.5f, 11); a.moveTo(12, 17.5f); a.lineTo(12, 21);
            break;
        case SWORD:
            a.moveTo(19.5f, 4.5f); a.lineTo(9, 15); a.moveTo(19.5f, 4.5f); a.lineTo(19.5f, 8); a.moveTo(19.5f, 4.5f); a.lineTo(16, 4.5f);
            a.moveTo(6.5f, 12.5f); a.lineTo(11.5f, 17.5f); a.moveTo(8, 16); a.lineTo(4.5f, 19.5f);
            break;
        case TEXTURE:
            a.addRoundRect(new RectF(3.5f, 4.5f, 20.5f, 19.5f), 2, 2, Path.Direction.CW);
            a.moveTo(3.5f, 16); a.lineTo(9, 11); a.lineTo(13.5f, 15.5f); a.lineTo(16, 13); a.lineTo(20.5f, 17);
            c.drawPath(a, p); a.reset();
            c.drawCircle(15.5f, 8.5f, 1.6f, p);
            break;
        case BUG:
            a.addOval(new RectF(7, 7, 17, 20), Path.Direction.CW);
            a.moveTo(7, 12); a.lineTo(3.5f, 10.5f); a.moveTo(7, 16); a.lineTo(3.5f, 17.5f); a.moveTo(17, 12); a.lineTo(20.5f, 10.5f);
            a.moveTo(17, 16); a.lineTo(20.5f, 17.5f); a.moveTo(9.5f, 7.5f); a.lineTo(8, 4); a.moveTo(14.5f, 7.5f); a.lineTo(16, 4); a.moveTo(12, 9); a.lineTo(12, 20);
            break;
        }
        c.drawPath(a, p);
        c.restore();
    }

    private static void fill(Canvas c, Paint p, float x, float y, float r) {
        Paint.Style st = p.getStyle();
        p.setStyle(Paint.Style.FILL);
        c.drawCircle(x, y, r, p);
        p.setStyle(st);
    }

    /** A view that shows one icon (centered), e.g. in a header. */
    static final class IconView extends View {
        int icon, color;
        float strokeDp = 1.8f;

        IconView(Context c, int icon, int color) {
            super(c);
            this.icon = icon;
            this.color = color;
        }

        void set(int icon, int color) {
            this.icon = icon;
            this.color = color;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float s = Math.min(getWidth(), getHeight());
            Icons.draw(c, icon, getWidth() / 2f, getHeight() / 2f, s, color, Theme.dpf(strokeDp));
        }
    }

    private Icons() {}
}
