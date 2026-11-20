package org.psprecomp.setup;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;

/**
 * The big crystal of the progress page: it fills with light from the bottom as the work goes on (a moving surface with a wave), a ring of
 * light runs around it, and the percentage sits under it. Modes: progress, spinner (an unknown amount of work: the light runs up and down),
 * done (fills, then flashes gold with a burst) and failed (red, with a crack).
 */
final class ProgressEmblem extends View {
    static final int PROGRESS = 0, SPIN = 1, DONE = 2, FAILED = 3;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path gem = new Path(), surf = new Path();
    private float shown = 0, target = 0;
    private int mode = PROGRESS;
    private long last = 0, modeStart = 0;
    private float t = 0;
    private boolean showPercent = true;
    private int color = Theme.CRYSTAL;

    ProgressEmblem(Context c) {
        super(c);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        text.setTypeface(Theme.title);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Theme.GOLD_LIGHT);
        text.setShadowLayer(Theme.dpf(8), 0, 0, 0x88E2C47A);
    }

    void setFraction(float f) {
        target = Math.max(0, Math.min(1, f));
    }

    void setMode(int m) {
        if (m == mode) return;
        mode = m;
        modeStart = SystemClock.uptimeMillis();
        if (m == DONE) target = 1;
        color = m == FAILED ? Theme.DANGER : m == DONE ? Theme.GOLD : Theme.CRYSTAL;
    }

    void setShowPercent(boolean s) {
        showPercent = s;
    }

    private final android.graphics.RectF ringBox = new android.graphics.RectF();

    /** One half of the tilted orbit ring and the lights running on it: front = the half nearer the viewer (lower half of the ellipse). */
    private void drawRing(Canvas c, float cx, float cy, float rw, float rh, boolean front) {
        c.save();
        c.translate(cx, cy);
        c.rotate(-14);
        stroke.setStrokeWidth(Theme.dpf(front ? 1.4f : 1));
        stroke.setColor(Theme.alpha(Theme.GOLD, front ? 0.6f : 0.28f));
        ringBox.set(-rw, -rh, rw, rh);
        c.drawArc(ringBox, front ? 0 : 180, 180, false, stroke);   // angles run clockwise: 0..180 is the lower (near) half
        float speed = mode == SPIN ? 2.4f : 1.1f;
        for (int k = 0; k < 2; k++) {
            float a = t * speed + k * 3.1416f;
            float sy = (float) Math.sin(a);
            if ((sy > 0) != front) continue;   // a light on the far half is hidden by the crystal where it passes behind
            float ox = (float) Math.cos(a) * rw, oy = sy * rh, depth = 0.75f + 0.25f * sy;
            paint.setColor(Theme.alpha(Theme.GOLD_LIGHT, 0.9f * depth));
            c.drawCircle(ox, oy, Theme.dpf(2.4f) * depth, paint);
            paint.setColor(Theme.alpha(Theme.GOLD_LIGHT, 0.18f * depth));
            c.drawCircle(ox, oy, Theme.dpf(8) * depth, paint);
        }
        c.restore();
    }

    @Override
    protected void onDraw(Canvas c) {
        if (getWidth() < 4 || getHeight() < 4) return;   // not laid out (yet): gradients need a size
        long now = SystemClock.uptimeMillis();
        float dt = last == 0 ? 0.016f : Math.min(0.1f, (now - last) / 1000f);
        last = now;
        t += dt;
        shown += (target - shown) * Math.min(1, dt * 3.5f);
        float since = (now - modeStart) / 1000f;

        float w = getWidth(), h = getHeight();
        float textH = showPercent ? Math.min(w, h) * 0.2f : 0;
        float gh = (h - textH) * 0.82f, gw = gh * 0.58f;
        if (gw > w * 0.8f) { gw = w * 0.8f; gh = gw / 0.58f; }
        float cx = w / 2, top = (h - textH - gh) / 2 + Theme.dpf(4), bot = top + gh, mid = top + gh * 0.44f;
        gem.reset();
        gem.moveTo(cx, top);
        gem.lineTo(cx + gw / 2, mid);
        gem.lineTo(cx, bot);
        gem.lineTo(cx - gw / 2, mid);
        gem.close();

        // halo
        float pulse = 0.5f + 0.5f * (float) Math.sin(t * 2);
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        float halo = Math.min(gh * 0.75f, Math.min(w, h) * 0.5f);   // inside the view: a clipped halo would show as a box
        paint.setShader(new RadialGradient(cx, (top + bot) / 2, halo, Theme.alpha(color, 0.18f + 0.08f * pulse), 0, Shader.TileMode.CLAMP));
        c.drawCircle(cx, (top + bot) / 2, halo, paint);
        paint.setShader(null);

        // the ring around the crystal: its far half (and lights on it) behind the crystal, its near half in front (drawn below)
        final float ringW = gw * 0.95f, ringH = gh * 0.14f, ringY = (top + bot) / 2;
        drawRing(c, cx, ringY, ringW, ringH, false);

        // the glass
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        paint.setShader(new LinearGradient(cx - gw / 2, top, cx + gw / 2, bot, 0x30FFFFFF, 0x10203050, Shader.TileMode.CLAMP));
        c.drawPath(gem, paint);
        paint.setShader(null);

        // the light inside
        float level;
        if (mode == SPIN) level = 0.5f + 0.42f * (float) Math.sin(t * 1.6f);
        else level = shown;
        c.save();
        c.clipPath(gem);
        float y = bot - (bot - top) * level;
        surf.reset();
        surf.moveTo(cx - gw, bot + 2);
        float amp = Theme.dpf(level > 0.01f && level < 0.99f ? 4 : 0);
        for (float x = cx - gw; x <= cx + gw; x += Theme.dpf(4)) surf.lineTo(x, y + amp * (float) Math.sin(x / Theme.dpf(14) + t * 3));
        surf.lineTo(cx + gw, bot + 2);
        surf.close();
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        paint.setShader(new LinearGradient(0, y, 0, bot, new int[] {Theme.mix(color, 0xFFFFFFFF, 0.55f), color, Theme.mix(color, 0xFF000000, 0.45f)}, null, Shader.TileMode.CLAMP));
        c.drawPath(surf, paint);
        paint.setShader(null);
        // rising sparks inside the light
        for (int i = 0; i < 9; i++) {
            float ph = (t * 0.35f + i * 0.13f) % 1f;
            float sx = cx + (float) Math.sin(i * 7.3f + t * 0.5f) * gw * 0.28f, sy = bot - (bot - y) * ph;
            paint.setColor(Theme.alpha(0xFFFFFFFF, 0.5f * (1 - ph)));
            c.drawCircle(sx, sy, Theme.dpf(1.4f), paint);
        }
        // a sweep of light over the glass
        float sw = ((t * 0.25f) % 1.6f) - 0.3f;
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        paint.setShader(new LinearGradient(cx - gw + sw * gw * 2, top, cx - gw * 0.4f + sw * gw * 2, bot, new int[] {0, 0x40FFFFFF, 0}, null, Shader.TileMode.CLAMP));
        c.drawRect(cx - gw, top, cx + gw, bot, paint);
        paint.setShader(null);
        c.restore();

        // facets and frame
        stroke.setColor(Theme.alpha(Theme.mix(color, 0xFFFFFFFF, 0.5f), 0.55f));
        stroke.setStrokeWidth(Theme.dpf(1));
        c.drawLine(cx - gw / 2, mid, cx + gw / 2, mid, stroke);
        c.drawLine(cx, top, cx, bot, stroke);
        c.drawLine(cx - gw / 2, mid, cx, top + gh * 0.62f, stroke);
        c.drawLine(cx + gw / 2, mid, cx, top + gh * 0.62f, stroke);
        stroke.setStrokeWidth(Theme.dpf(2));
        stroke.setColor(Theme.alpha(Theme.mix(color, 0xFFFFFFFF, 0.3f), 0.95f));
        c.drawPath(gem, stroke);

        if (mode == FAILED) {   // a crack
            stroke.setColor(0xFFFFD9D2);
            stroke.setStrokeWidth(Theme.dpf(1.5f));
            Path k = new Path();
            k.moveTo(cx - gw * 0.08f, top + gh * 0.18f);
            k.lineTo(cx + gw * 0.07f, top + gh * 0.36f);
            k.lineTo(cx - gw * 0.1f, top + gh * 0.5f);
            k.lineTo(cx + gw * 0.12f, top + gh * 0.66f);
            k.lineTo(cx - gw * 0.02f, top + gh * 0.82f);
            c.drawPath(k, stroke);
        }
        drawRing(c, cx, ringY, ringW, ringH, true);   // the near half passes in front of the crystal

        if (mode == DONE && since < 1.6f) {   // a burst of rays when it is finished
            float e = since / 1.6f;
            paint.setColor(Theme.alpha(Theme.GOLD_LIGHT, 0.6f * (1 - e)));
            for (int i = 0; i < 16; i++) {
                double a = i * Math.PI / 8 + 0.2;
                float r0 = gh * (0.35f + 0.5f * e), r1 = r0 + gh * 0.12f;
                stroke.setColor(Theme.alpha(Theme.GOLD_LIGHT, 0.7f * (1 - e)));
                c.drawLine(cx + (float) Math.cos(a) * r0, (top + bot) / 2 + (float) Math.sin(a) * r0, cx + (float) Math.cos(a) * r1,
                        (top + bot) / 2 + (float) Math.sin(a) * r1, stroke);
            }
            c.drawCircle(cx, (top + bot) / 2, gh * 0.5f * e, paint);
        }

        if (showPercent && mode != SPIN) {
            text.setTextSize(textH * 0.78f);
            String s = mode == FAILED ? "STOPPED" : Math.round(shown * 100) + "%";
            if (mode == FAILED) text.setTextSize(textH * 0.5f);
            text.setColor(mode == FAILED ? Theme.DANGER : Theme.GOLD_LIGHT);
            c.drawText(s, cx, h - textH * 0.18f, text);
        }
        postInvalidateOnAnimation();
    }
}
