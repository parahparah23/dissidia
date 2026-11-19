package org.psprecomp.setup;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;

/** The plan as a row of segments (one per step, as wide as its share of the work): done ones glow, the running one fills and shimmers. */
final class StepTrack extends View {
    private float[] weights = {1};
    private int current = 0;
    private float stepFrac = 0, shown = 0;
    private boolean failed = false;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    StepTrack(Context c) {
        super(c);
    }

    void setPlan(float[] w) {
        weights = w.length == 0 ? new float[] {1} : w;
        invalidate();
    }

    /** current: running step; frac: how much of it is done. */
    void set(int step, float frac, boolean failed) {
        current = step;
        stepFrac = Math.max(0, Math.min(1, frac));
        this.failed = failed;
    }

    @Override
    protected void onMeasure(int ws, int hs) {
        setMeasuredDimension(MeasureSpec.getSize(ws), Theme.dp(8));
    }

    @Override
    protected void onDraw(Canvas c) {
        if (getWidth() < 4 || getHeight() < 4) return;   // not laid out (yet): gradients need a size
        float total = 0;
        for (float x : weights) total += Math.max(0.02f, x);
        float gap = Theme.dpf(4), w = getWidth() - gap * (weights.length - 1), h = getHeight(), x = 0;
        shown += (stepFrac - shown) * 0.15f;
        float t = SystemClock.uptimeMillis() / 1000f;
        for (int i = 0; i < weights.length; i++) {
            float sw = w * Math.max(0.02f, weights[i]) / total;
            paint.setShader(null);
            paint.setColor(0x30FFFFFF);
            c.drawRoundRect(x, 0, x + sw, h, h / 2, h / 2, paint);
            float f = i < current ? 1 : i == current ? shown : 0;
            if (f > 0) {
                int col = failed && i == current ? Theme.DANGER : i < current ? Theme.CRYSTAL_DEEP : Theme.CRYSTAL;
                paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
                paint.setShader(new LinearGradient(x, 0, x + sw, 0, Theme.mix(col, 0xFFFFFFFF, 0.2f), col, Shader.TileMode.CLAMP));
                c.drawRoundRect(x, 0, x + Math.max(h, sw * f), h, h / 2, h / 2, paint);
                if (i == current && !failed) {   // shimmer
                    float sx = x + ((t * 0.6f) % 1.4f - 0.2f) * sw;
                    paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
                    paint.setShader(new LinearGradient(sx - sw * 0.15f, 0, sx + sw * 0.15f, 0, new int[] {0, 0x90FFFFFF, 0}, null, Shader.TileMode.CLAMP));
                    c.save();
                    c.clipRect(x, 0, x + Math.max(h, sw * f), h);
                    c.drawRect(x, 0, x + sw, h, paint);
                    c.restore();
                }
            }
            x += sw + gap;
        }
        paint.setShader(null);
        postInvalidateOnAnimation();
    }
}
