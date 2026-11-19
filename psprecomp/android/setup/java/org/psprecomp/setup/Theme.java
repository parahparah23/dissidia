package org.psprecomp.setup;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.TextView;

/**
 * The look of the setup app: the night-sky / crystal / gold palette of Dissidia's menus, its fonts (Cinzel for titles like the game's logo
 * type, Noto Sans for text; both OFL, shipped in assets/fonts) and a few text helpers. Everything is built in code (no XML layouts).
 */
final class Theme {
    // palette
    static final int BG_TOP = 0xFF16203F, BG_MID = 0xFF0B1028, BG_BOTTOM = 0xFF04060F;
    static final int GOLD = 0xFFE2C47A, GOLD_LIGHT = 0xFFF6E7BC, GOLD_DIM = 0xFF8C7543;
    static final int CRYSTAL = 0xFF86EBCB, CRYSTAL_DEEP = 0xFF2E9C86, CRYSTAL_GLOW = 0x5586EBCB;
    static final int SKY = 0xFF8FC3FF;
    static final int TEXT = 0xFFF1ECDF, TEXT_DIM = 0xFFA9B0C4, TEXT_FAINT = 0xFF6D7591;
    static final int PANEL = 0xC0121A36, PANEL_HI = 0xE01C2750, PANEL_LINE = 0x40E2C47A;
    static final int DANGER = 0xFFE0675A, DANGER_DIM = 0xFF7A2C27, WARN = 0xFFF0B45A, OK = 0xFF86EBCB;

    static Typeface title, body, bodyBold, mono;
    static float density = 1;

    static void init(Context c) {
        density = c.getResources().getDisplayMetrics().density;
        if (title != null) return;
        title = load(c, "fonts/Cinzel.ttf", Typeface.SERIF);
        body = load(c, "fonts/NotoSans-Regular.ttf", Typeface.SANS_SERIF);
        bodyBold = load(c, "fonts/NotoSans-Bold.ttf", Typeface.DEFAULT_BOLD);
        mono = Typeface.MONOSPACE;
    }

    private static Typeface load(Context c, String path, Typeface fallback) {
        try {
            return Typeface.createFromAsset(c.getAssets(), path);
        } catch (Exception e) {
            return fallback;
        }
    }

    static int dp(float v) {
        return Math.round(v * density);
    }

    static float dpf(float v) {
        return v * density;
    }

    static int alpha(int color, float a) {
        return (Math.round(Color.alpha(color) * Math.max(0, Math.min(1, a))) << 24) | (color & 0xFFFFFF);
    }

    static int mix(int a, int b, float t) {
        t = Math.max(0, Math.min(1, t));
        return Color.argb(Math.round(Color.alpha(a) + (Color.alpha(b) - Color.alpha(a)) * t), Math.round(Color.red(a) + (Color.red(b) - Color.red(a)) * t),
                Math.round(Color.green(a) + (Color.green(b) - Color.green(a)) * t), Math.round(Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t));
    }

    /** Small spaced capitals above a title ("STEP 1", "OPTIONS"). */
    static TextView eyebrow(Context c, String s) {
        TextView t = text(c, s.toUpperCase(), 12, GOLD, bodyBold);
        t.setLetterSpacing(0.28f);
        return t;
    }

    static TextView heading(Context c, String s, float sp) {
        TextView t = text(c, s, sp, GOLD_LIGHT, title);
        t.setLetterSpacing(0.06f);
        t.setShadowLayer(dpf(10), 0, 0, 0x66E2C47A);
        return t;
    }

    static TextView text(Context c, String s, float sp, int color, Typeface tf) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(tf);
        t.setIncludeFontPadding(true);
        return t;
    }

    static TextView body(Context c, String s) {
        TextView t = text(c, s, 15, TEXT_DIM, body);
        t.setLineSpacing(dpf(2), 1f);
        return t;
    }

    static TextView single(TextView t) {
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        return t;
    }

    static TextView centered(TextView t) {
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        return t;
    }

    private Theme() {}
}
