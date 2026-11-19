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

import java.util.Random;

/**
 * The living background behind every page: a night sky gradient, slowly turning light rays, a large wireframe crystal that rotates in
 * space, a horizon ring and drifting motes of light. Its mood (crystal / gold / chaos red) fades between pages: gold when something is
 * finished, red when it failed. `energy` speeds the motes up while the game is being built.
 */
final class BackdropView extends View {
    static final int MOOD_CRYSTAL = 0, MOOD_GOLD = 1, MOOD_CHAOS = 2;
    private static final int[] MOOD_COLOR = {Theme.CRYSTAL, Theme.GOLD, Theme.DANGER};

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG), glow = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float[] px, py, pz, pphase;
    private final Random rnd = new Random(12);
    private long last = 0;
    private float t = 0;
    private int moodColor = Theme.CRYSTAL, fromColor = Theme.CRYSTAL, toColor = Theme.CRYSTAL;
    private float moodT = 1;
    private float energy = 0, energyTarget = 0;
    private float crystalA = 1, crystalTarget = 1;   // the wireframe crystal fades out behind pages that show their own (the hero)
    private boolean slow = false;   // ~30 fps (during the long build: leaves the CPU to the compiler)
    private long nextFrame = 0;
    private Shader sky;
    private int skyH = -1;
    private final Path path = new Path();

    // the crystal: an elongated octahedron with an inner ring
    private static final float[][] V = {{0, -1.6f, 0}, {0, 1.6f, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {0.62f, -0.55f, 0.62f}, {-0.62f, -0.55f, -0.62f}};
    private static final int[][] E = {{0, 2}, {0, 3}, {0, 4}, {0, 5}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {2, 3}, {3, 4}, {4, 5}, {5, 2}};

    BackdropView(Context c) {
        super(c);
        int n = 90;
        px = new float[n];
        py = new float[n];
        pz = new float[n];
        pphase = new float[n];
        for (int i = 0; i < n; i++) {
            px[i] = rnd.nextFloat();
            py[i] = rnd.nextFloat();
            pz[i] = 0.25f + rnd.nextFloat() * 0.75f;
            pphase[i] = rnd.nextFloat() * 6.283f;
        }
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeCap(Paint.Cap.ROUND);
        glow.setStyle(Paint.Style.STROKE);
        glow.setStrokeCap(Paint.Cap.ROUND);
    }

    void setMood(int mood) {
        int c = MOOD_COLOR[Math.max(0, Math.min(2, mood))];
        if (c == toColor) return;
        fromColor = moodColor;
        toColor = c;
        moodT = 0;
    }

    void setEnergy(float e) {
        energyTarget = e;
    }

    void setCrystal(boolean on) {
        crystalTarget = on ? 1 : 0;
    }

    void setSlow(boolean s) {
        slow = s;
    }

    @Override
    protected void onDraw(Canvas c) {
        if (getWidth() < 4 || getHeight() < 4) return;   // not laid out (yet): gradients need a size
        long now = SystemClock.uptimeMillis();
        float dt = last == 0 ? 0.016f : Math.min(0.1f, (now - last) / 1000f);
        last = now;
        t += dt;
        energy += (energyTarget - energy) * Math.min(1, dt * 1.5f);
        crystalA += (crystalTarget - crystalA) * Math.min(1, dt * 3f);
        if (moodT < 1) {
            moodT = Math.min(1, moodT + dt * 1.2f);
            moodColor = Theme.mix(fromColor, toColor, moodT);
        }
        final int w = getWidth(), h = getHeight();
        final boolean landscape = w > h;

        // sky
        if (skyH != h) {
            sky = new LinearGradient(0, 0, 0, h, new int[] {Theme.BG_TOP, Theme.BG_MID, Theme.BG_BOTTOM}, new float[] {0, 0.45f, 1}, Shader.TileMode.CLAMP);
            skyH = h;
        }
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        paint.setShader(sky);
        c.drawRect(0, 0, w, h, paint);

        // the glow the crystal sits in
        float gx = landscape ? w * 0.27f : w * 0.5f, gy = landscape ? h * 0.48f : h * 0.2f;
        float gr = Math.max(w, h) * 0.55f;
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        paint.setShader(new RadialGradient(gx, gy, gr, new int[] {Theme.alpha(moodColor, 0.22f), Theme.alpha(moodColor, 0.06f), 0}, new float[] {0, 0.45f, 1},
                Shader.TileMode.CLAMP));
        c.drawCircle(gx, gy, gr, paint);
        paint.setShader(null);

        // light rays
        c.save();
        c.translate(gx, gy);
        c.rotate(t * 2.2f);
        paint.setColor(Theme.alpha(Theme.GOLD_LIGHT, 0.035f));
        float rl = Math.max(w, h) * 1.2f;
        for (int i = 0; i < 12; i++) {
            path.reset();
            path.moveTo(0, 0);
            double a0 = i * Math.PI * 2 / 12, a1 = a0 + 0.11 + 0.05 * Math.sin(t * 0.4 + i);
            path.lineTo((float) Math.cos(a0) * rl, (float) Math.sin(a0) * rl);
            path.lineTo((float) Math.cos(a1) * rl, (float) Math.sin(a1) * rl);
            path.close();
            c.drawPath(path, paint);
        }
        c.restore();

        // horizon ring
        float ry = landscape ? h * 0.86f : h * 0.9f;
        line.setStrokeWidth(Theme.dpf(1.2f));
        line.setColor(Theme.alpha(Theme.GOLD, 0.16f));
        c.drawOval(-w * 0.2f, ry - h * 0.06f, w * 1.2f, ry + h * 0.06f, line);
        line.setColor(Theme.alpha(moodColor, 0.10f));
        c.drawOval(-w * 0.35f, ry - h * 0.11f, w * 1.35f, ry + h * 0.11f, line);

        // the crystal
        if (crystalA > 0.02f) {
            // landscape: in the glow on the left (pages put their text right of it); portrait: low down, under the cards' usual reach
            int save = c.saveLayerAlpha(0, 0, w, h, Math.round(255 * crystalA * (landscape ? 0.45f : 0.55f)));
            if (landscape) drawCrystal(c, gx, gy, Math.min(w, h) * 0.2f);
            else drawCrystal(c, w * 0.5f, h * 0.8f, Math.min(w, h) * 0.17f);
            c.restoreToCount(save);
        }

        // motes
        float speed = 0.012f + energy * 0.05f;
        for (int i = 0; i < px.length; i++) {
            py[i] -= dt * speed * pz[i];
            px[i] += dt * 0.004f * (float) Math.sin(t * 0.3f + pphase[i]);
            if (py[i] < -0.02f) { py[i] = 1.02f; px[i] = rnd.nextFloat(); }
            float tw = 0.5f + 0.5f * (float) Math.sin(t * (1.2f + pz[i]) + pphase[i]);
            float r = Theme.dpf(0.8f + 1.6f * pz[i]);
            int col = i % 3 == 0 ? Theme.GOLD_LIGHT : moodColor;
            paint.setColor(Theme.alpha(col, 0.15f + 0.55f * tw * pz[i]));
            c.drawCircle(px[i] * w, py[i] * h, r, paint);
            if (pz[i] > 0.85f) {
                paint.setColor(Theme.alpha(col, 0.08f * tw));
                c.drawCircle(px[i] * w, py[i] * h, r * 4, paint);
            }
        }

        if (slow) {
            long due = Math.max(now + 33, nextFrame + 33);
            nextFrame = due;
            postInvalidateDelayed(33);
        } else postInvalidateOnAnimation();
    }

    private final android.graphics.RectF ringBox = new android.graphics.RectF();

    /** Half of the tilted ring around the crystal (front = the lower, nearer half) and its light when it is on that half. */
    private void orbit(Canvas c, float cx, float cy, float size, boolean front) {
        c.save();
        c.translate(cx, cy);
        c.rotate(-12);
        float rw = size * 1.55f, rh = size * 0.32f;
        line.setStrokeWidth(Theme.dpf(1.1f));
        line.setColor(Theme.alpha(Theme.GOLD, front ? 0.38f : 0.18f));
        ringBox.set(-rw, -rh, rw, rh);
        c.drawArc(ringBox, front ? 0 : 180, 180, false, line);
        float oa = t * 0.9f, sy = (float) Math.sin(oa);
        if ((sy > 0) == front) {
            float d = 0.7f + 0.3f * sy;
            paint.setColor(Theme.alpha(Theme.GOLD_LIGHT, 0.85f * d));
            c.drawCircle((float) Math.cos(oa) * rw, sy * rh, Theme.dpf(2.2f) * d, paint);
            paint.setColor(Theme.alpha(Theme.GOLD_LIGHT, 0.15f * d));
            c.drawCircle((float) Math.cos(oa) * rw, sy * rh, Theme.dpf(7) * d, paint);
        }
        c.restore();
    }

    private void drawCrystal(Canvas c, float cx, float cy, float size) {
        float ay = t * 0.35f, ax = 0.35f + 0.08f * (float) Math.sin(t * 0.5f);
        float[][] p = new float[V.length][3];
        float cyy = (float) Math.cos(ay), syy = (float) Math.sin(ay), cxx = (float) Math.cos(ax), sxx = (float) Math.sin(ax);
        float bob = (float) Math.sin(t * 0.8f) * size * 0.06f;
        for (int i = 0; i < V.length; i++) {
            float x = V[i][0], y = V[i][1], z = V[i][2];
            float x1 = x * cyy + z * syy, z1 = -x * syy + z * cyy;
            float y1 = y * cxx - z1 * sxx, z2 = y * sxx + z1 * cxx;
            float persp = 3.2f / (3.2f + z2);
            p[i][0] = cx + x1 * size * persp;
            p[i][1] = cy + bob + y1 * size * persp;
            p[i][2] = z2;
        }
        orbit(c, cx, cy + bob, size, false);   // the ring's far half behind the crystal
        // faces, faintly filled (back to front is not needed at this alpha)
        paint.setColor(Theme.alpha(moodColor, 0.05f));
        int[][] faces = {{0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2}, {1, 2, 3}, {1, 3, 4}, {1, 4, 5}, {1, 5, 2}};
        for (int[] f : faces) {
            path.reset();
            path.moveTo(p[f[0]][0], p[f[0]][1]);
            path.lineTo(p[f[1]][0], p[f[1]][1]);
            path.lineTo(p[f[2]][0], p[f[2]][1]);
            path.close();
            c.drawPath(path, paint);
        }
        glow.setStrokeWidth(Theme.dpf(5));
        line.setStrokeWidth(Theme.dpf(1.1f));
        for (int[] e : E) {
            float depth = (p[e[0]][2] + p[e[1]][2]) * 0.5f;   // nearer edges brighter
            float a = 0.22f + 0.3f * (1 - (depth + 1) / 2);
            glow.setColor(Theme.alpha(moodColor, a * 0.25f));
            c.drawLine(p[e[0]][0], p[e[0]][1], p[e[1]][0], p[e[1]][1], glow);
            line.setColor(Theme.alpha(Theme.mix(moodColor, 0xFFFFFFFF, 0.4f), a));
            c.drawLine(p[e[0]][0], p[e[0]][1], p[e[1]][0], p[e[1]][1], line);
        }
        orbit(c, cx, cy + bob, size, true);   // the ring's near half in front of the crystal
    }
}
