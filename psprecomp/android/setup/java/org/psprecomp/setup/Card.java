package org.psprecomp.setup;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.Shader;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.OvershootInterpolator;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * One choice on a page: a panel with chamfered corners and a thin gold frame (the game's menu windows), an icon in a crystal badge, a
 * title, a line of explanation and, at the end, a chevron, a value, an On/Off crystal switch or a two-way choice. Focus (d-pad / controller)
 * lights the frame and sends a shine across; a press sinks it a little. Left / right flip a switch or a choice.
 */
final class Card extends LinearLayout {
    static final int ACTION = 0, TOGGLE = 1, CHOICE = 2, INFO = 3;
    static final int STYLE_NORMAL = 0, STYLE_PRIMARY = 1, STYLE_DANGER = 2, STYLE_QUIET = 3;

    interface OnChange {
        void changed(Card c);
    }

    final int kind;
    int style = STYLE_NORMAL;
    private final TextView titleView, summaryView, valueView;
    private final Icons.IconView iconView;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path shape = new Path();
    private float focusT = 0, shineT = 2, switchT = 0, pulse = 0;
    private ValueAnimator focusAnim, shineAnim, switchAnim;
    private boolean on;
    private String[] choices;
    private int choice;
    private OnChange onChange;
    private final int chamfer = Theme.dp(12);
    private boolean enabledLook = true;

    Card(Context c, int kind, int icon, String title, String summary) {
        super(c);
        this.kind = kind;
        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setWillNotDraw(false);
        setClipToPadding(false);
        int ph = Theme.dp(16), pv = Theme.dp(kind == INFO ? 12 : 14);
        setPadding(ph, pv, ph, pv);
        setMinimumHeight(Theme.dp(kind == INFO ? 52 : 64));
        if (kind != INFO) {
            setFocusable(true);
            setClickable(true);
        }

        iconView = new Icons.IconView(c, icon, Theme.GOLD);
        iconView.strokeDp = 1.7f;
        LayoutParams ip = new LayoutParams(Theme.dp(26), Theme.dp(26));
        ip.rightMargin = Theme.dp(16);
        ip.leftMargin = Theme.dp(4);
        if (icon != Icons.NONE) addView(iconView, ip);

        LinearLayout col = new LinearLayout(c);
        col.setOrientation(VERTICAL);
        titleView = Theme.text(c, title, 16.5f, Theme.TEXT, Theme.bodyBold);
        summaryView = Theme.text(c, summary == null ? "" : summary, 13, Theme.TEXT_DIM, Theme.body);
        summaryView.setLineSpacing(Theme.dpf(1.5f), 1f);
        summaryView.setVisibility(summary == null || summary.isEmpty() ? GONE : VISIBLE);
        col.addView(titleView);
        LayoutParams sp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
        sp.topMargin = Theme.dp(2);
        col.addView(summaryView, sp);
        addView(col, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));

        valueView = Theme.text(c, "", 13.5f, Theme.GOLD_LIGHT, Theme.bodyBold);
        if (kind == CHOICE) {   // the choice sits under the title as a chip (room for the explanation on narrow screens)
            android.graphics.drawable.GradientDrawable chip = new android.graphics.drawable.GradientDrawable();
            chip.setColor(0x40E2C47A);
            chip.setStroke(Theme.dp(1), 0x90E2C47A);
            chip.setCornerRadius(Theme.dp(14));
            valueView.setBackground(chip);
            valueView.setPadding(Theme.dp(12), Theme.dp(4), Theme.dp(12), Theme.dp(5));
            LayoutParams vp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
            vp.topMargin = Theme.dp(6);
            vp.bottomMargin = Theme.dp(2);
            col.addView(valueView, 1, vp);
        } else {
            valueView.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
            valueView.setMaxWidth(Theme.dp(220));
            LayoutParams vp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
            vp.leftMargin = Theme.dp(12);
            addView(valueView, vp);
        }
        // room for what is drawn at the end (chevron or switch)
        View endSpace = new View(c);
        int endW = kind == TOGGLE ? 54 : kind == ACTION ? 22 : kind == CHOICE ? 6 : 0;
        if (endW > 0) addView(endSpace, new LayoutParams(Theme.dp(endW), 1));

        setOnFocusChangeListener((v, has) -> {
            animateFocus(has);
            if (has) shine();
        });
    }

    Card style(int s) {
        style = s;
        if (s == STYLE_DANGER) iconView.set(iconView.icon, Theme.DANGER);
        if (s == STYLE_PRIMARY) titleView.setTextColor(Theme.GOLD_LIGHT);
        if (s == STYLE_QUIET) {
            titleView.setTextSize(15);
            setMinimumHeight(Theme.dp(54));
        }
        invalidate();
        return this;
    }

    Card value(String v) {
        valueView.setText(v == null ? "" : v);
        return this;
    }

    Card summary(String s) {
        summaryView.setText(s == null ? "" : s);
        summaryView.setVisibility(s == null || s.isEmpty() ? GONE : VISIBLE);
        return this;
    }

    Card title(String s) {
        titleView.setText(s);
        return this;
    }

    Card icon(int icon, int color) {
        iconView.set(icon, color);
        return this;
    }

    Card onClick(OnClickListener l) {
        setOnClickListener(l);
        return this;
    }

    Card onChange(OnChange l) {
        onChange = l;
        return this;
    }

    Card toggle(boolean v) {
        on = v;
        switchT = v ? 1 : 0;
        valueView.setText("");
        setContentDescription(titleView.getText() + (v ? ", on" : ", off"));
        setOnClickListener(x -> flip(+1));
        return this;
    }

    Card choices(String[] c, int sel) {
        choices = c;
        choice = sel;
        valueView.setText("‹  " + c[sel] + "  ›");
        setOnClickListener(x -> flip(+1));
        return this;
    }

    boolean isOn() {
        return on;
    }

    int choice() {
        return choice;
    }

    /** Greys the card out (a feature this release does not have yet): it stays readable but cannot be changed or focused. */
    Card disabled(String why) {
        enabledLook = false;
        setFocusable(false);
        setClickable(false);
        setAlpha(0.5f);
        if (why != null) valueView.setText(why);
        valueView.setTextColor(Theme.TEXT_FAINT);
        return this;
    }

    private void flip(int dir) {
        if (!enabledLook) return;
        if (kind == TOGGLE) {
            on = !on;
            setContentDescription(titleView.getText() + (on ? ", on" : ", off"));
            if (switchAnim != null) switchAnim.cancel();
            switchAnim = ValueAnimator.ofFloat(switchT, on ? 1 : 0);
            switchAnim.setDuration(260);
            switchAnim.setInterpolator(new OvershootInterpolator(1.6f));
            switchAnim.addUpdateListener(a -> { switchT = (float) a.getAnimatedValue(); invalidate(); });
            switchAnim.start();
        } else if (kind == CHOICE && choices != null) {
            choice = (choice + dir + choices.length) % choices.length;
            valueView.setText("‹  " + choices[choice] + "  ›");
            valueView.setAlpha(0);
            valueView.setTranslationX(Theme.dp(10) * dir);
            valueView.animate().alpha(1).translationX(0).setDuration(220).setInterpolator(new DecelerateInterpolator()).start();
        }
        shine();
        if (onChange != null) onChange.changed(this);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent e) {
        if ((kind == TOGGLE || kind == CHOICE) && (keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_DPAD_RIGHT)) {
            if (kind == TOGGLE) {
                boolean want = keyCode == KeyEvent.KEYCODE_DPAD_RIGHT;
                if (want != on) flip(1);
            } else flip(keyCode == KeyEvent.KEYCODE_DPAD_RIGHT ? 1 : -1);
            return true;
        }
        return super.onKeyDown(keyCode, e);
    }

    @Override
    public void setPressed(boolean p) {
        super.setPressed(p);
        if (!enabledLook || kind == INFO) return;
        animate().scaleX(p ? 0.975f : 1f).scaleY(p ? 0.975f : 1f).setDuration(p ? 90 : 220).setInterpolator(new DecelerateInterpolator()).start();
        invalidate();
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_UP && isPressed()) shine();
        return super.onTouchEvent(e);
    }

    private void animateFocus(boolean has) {
        if (focusAnim != null) focusAnim.cancel();
        focusAnim = ValueAnimator.ofFloat(focusT, has ? 1 : 0);
        focusAnim.setDuration(200);
        focusAnim.addUpdateListener(a -> { focusT = (float) a.getAnimatedValue(); invalidate(); });
        focusAnim.start();
        animate().scaleX(has ? 1.015f : 1f).scaleY(has ? 1.015f : 1f).setDuration(200).start();
    }

    private void shine() {
        if (shineAnim != null) shineAnim.cancel();
        shineAnim = ValueAnimator.ofFloat(-0.3f, 1.3f);
        shineAnim.setDuration(650);
        shineAnim.setInterpolator(new DecelerateInterpolator());
        shineAnim.addUpdateListener(a -> { shineT = (float) a.getAnimatedValue(); invalidate(); });
        shineAnim.start();
    }

    /** A soft breathing glow on the frame (the suggested next step). */
    Card pulse() {
        ValueAnimator a = ValueAnimator.ofFloat(0, 1);
        a.setDuration(1800);
        a.setRepeatCount(ValueAnimator.INFINITE);
        a.setRepeatMode(ValueAnimator.REVERSE);
        a.addUpdateListener(x -> { pulse = (float) x.getAnimatedValue(); invalidate(); });
        a.start();
        addOnAttachStateChangeListener(new OnAttachStateChangeListener() {
            public void onViewAttachedToWindow(View v) {}
            public void onViewDetachedFromWindow(View v) { a.cancel(); }
        });
        return this;
    }

    static void chamferPath(Path p, float l, float t, float r, float b, float ch) {
        p.reset();
        p.moveTo(l + ch, t);
        p.lineTo(r, t);
        p.lineTo(r, b - ch);
        p.lineTo(r - ch, b);
        p.lineTo(l, b);
        p.lineTo(l, t + ch);
        p.close();
    }

    @Override
    protected void onDraw(Canvas c) {
        if (getWidth() < 4 || getHeight() < 4) return;   // not laid out (yet): gradients need a size
        final float w = getWidth(), h = getHeight(), inset = Theme.dpf(1);
        chamferPath(shape, inset, inset, w - inset, h - inset, chamfer);
        int accent = style == STYLE_DANGER ? Theme.DANGER : style == STYLE_PRIMARY ? Theme.GOLD : Theme.GOLD;
        boolean pressed = isPressed();

        // fill: dark glass, brighter on focus / press; the primary card carries a warm gold tint
        int top = style == STYLE_PRIMARY ? 0xE8473C26 : style == STYLE_DANGER ? 0xC02A1420 : kind == INFO ? 0x80101830 : Theme.PANEL_HI;
        int bottom = style == STYLE_PRIMARY ? 0xE81E1A2E : style == STYLE_DANGER ? 0xC0160A12 : kind == INFO ? 0x700A1024 : Theme.PANEL;
        float lift = Math.max(focusT, pressed ? 0.7f : 0);
        paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        paint.setShader(new LinearGradient(0, 0, 0, h, Theme.mix(top, 0xF0344070, lift * 0.35f), Theme.mix(bottom, 0xF0202A50, lift * 0.35f), Shader.TileMode.CLAMP));
        c.drawPath(shape, paint);
        paint.setShader(null);

        // glow around the frame when focused (drawn as wide faint strokes)
        float glowA = Math.max(focusT, pulse * 0.45f);
        if (glowA > 0.01f) {
            stroke.setStyle(Paint.Style.STROKE);
            for (int i = 3; i >= 1; i--) {
                stroke.setStrokeWidth(Theme.dpf(2.5f * i));
                stroke.setColor(Theme.alpha(accent, 0.07f * glowA));
                c.drawPath(shape, stroke);
            }
        }
        // the frame: a gold line, brighter at the top-left like light on metal
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(Theme.dpf(style == STYLE_PRIMARY ? 1.4f : 1));
        int frameA = Theme.alpha(accent, 0.30f + 0.6f * Math.max(glowA, style == STYLE_PRIMARY ? 0.45f : 0));
        stroke.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
        stroke.setShader(new LinearGradient(0, 0, w, h, new int[] {Theme.mix(frameA, 0xFFFFFFFF, 0.25f), frameA, Theme.alpha(accent, 0.18f + 0.4f * glowA)},
                null, Shader.TileMode.CLAMP));
        c.drawPath(shape, stroke);
        stroke.setShader(null);
        // corner ticks at the chamfers
        stroke.setColor(Theme.alpha(accent, 0.55f + 0.45f * glowA));
        stroke.setStrokeWidth(Theme.dpf(2));
        c.drawLine(inset + chamfer * 0.15f, inset + chamfer * 0.85f, inset + chamfer * 0.85f, inset + chamfer * 0.15f, stroke);
        c.drawLine(w - inset - chamfer * 0.85f, h - inset - chamfer * 0.15f, w - inset - chamfer * 0.15f, h - inset - chamfer * 0.85f, stroke);

        // the shine that runs across once on focus / tap
        if (shineT > -0.3f && shineT < 1.3f) {
            c.save();
            c.clipPath(shape);
            float x = shineT * (w + h) - h;
            paint.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
            paint.setShader(new LinearGradient(x, 0, x + h * 0.9f, h, new int[] {0, 0x30FFF4D6, 0}, null, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, paint);
            paint.setShader(null);
            c.restore();
        }
        // the icon's crystal badge
        if (iconView.getParent() != null && iconView.icon != Icons.NONE) {
            float cx = iconView.getLeft() + iconView.getWidth() / 2f, cy = iconView.getTop() + iconView.getHeight() / 2f, r = Theme.dpf(21);
            Path d = new Path();
            d.moveTo(cx, cy - r);
            d.lineTo(cx + r, cy);
            d.lineTo(cx, cy + r);
            d.lineTo(cx - r, cy);
            d.close();
            paint.setColor(Theme.alpha(style == STYLE_DANGER ? Theme.DANGER : Theme.GOLD, 0.08f + 0.1f * glowA));
            c.drawPath(d, paint);
            stroke.setColor(Theme.alpha(style == STYLE_DANGER ? Theme.DANGER : Theme.GOLD, 0.25f + 0.35f * glowA));
            stroke.setStrokeWidth(Theme.dpf(1));
            c.drawPath(d, stroke);
        }
        // end decoration
        if (kind == ACTION && enabledLook) {
            float cx = w - Theme.dpf(22), cy = h / 2;
            Icons.draw(c, Icons.CHEVRON, cx + Theme.dpf(3) * focusT, cy, Theme.dpf(16), Theme.alpha(accent, 0.5f + 0.5f * focusT), Theme.dpf(1.8f));
        } else if (kind == TOGGLE) {
            drawSwitch(c, w - Theme.dpf(16) - Theme.dpf(44), h / 2);
        }
    }

    private void drawSwitch(Canvas c, float x, float cy) {
        float tw = Theme.dpf(44), th = Theme.dpf(20);
        int track = Theme.mix(0x40FFFFFF, Theme.alpha(Theme.CRYSTAL_DEEP, 0.85f), switchT);
        paint.setColor(track);
        c.drawRoundRect(x, cy - th / 2, x + tw, cy + th / 2, th / 2, th / 2, paint);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(Theme.dpf(1));
        stroke.setColor(Theme.mix(0x50FFFFFF, Theme.CRYSTAL, switchT));
        c.drawRoundRect(x, cy - th / 2, x + tw, cy + th / 2, th / 2, th / 2, stroke);
        // the thumb is a small crystal
        float tx = x + th / 2 + (tw - th) * Math.max(0, Math.min(1.08f, switchT)), r = th * 0.62f;
        if (switchT > 0.05f) {
            paint.setColor(Theme.alpha(Theme.CRYSTAL, 0.25f * switchT));
            c.drawCircle(tx, cy, r * 1.6f, paint);
        }
        Path d = new Path();
        d.moveTo(tx, cy - r);
        d.lineTo(tx + r * 0.62f, cy);
        d.lineTo(tx, cy + r);
        d.lineTo(tx - r * 0.62f, cy);
        d.close();
        paint.setColor(Theme.mix(0xFFB8BED0, 0xFFE9FFF7, switchT));
        c.drawPath(d, paint);
        stroke.setColor(Theme.mix(0xFF7A8098, Theme.CRYSTAL_DEEP, switchT));
        c.drawPath(d, stroke);
        stroke.setColor(0x60FFFFFF);
        c.drawLine(tx - r * 0.62f, cy, tx + r * 0.62f, cy, stroke);
    }

    @Override
    protected void onFocusChanged(boolean gain, int dir, Rect prev) {
        super.onFocusChanged(gain, dir, prev);
        if (gain) {   // keep the focused card in view inside a scroller
            requestRectangleOnScreen(new Rect(0, -Theme.dp(24), getWidth(), getHeight() + Theme.dp(24)), false);
        }
    }
}
