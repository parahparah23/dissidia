package org.psprecomp.setup;

import android.animation.LayoutTransition;
import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;

/**
 * One page of the setup: a header (an optional animated hero, small capitals, the title, a few lines of text, optional extra views) and a
 * column of cards. Landscape puts the header on the left and the cards on the right (like the game's menus: description beside the list);
 * portrait stacks them and scrolls. Pieces float in one after another when the page appears.
 */
final class Page {
    final Context c;
    final String id;
    String eyebrow = "", title = "", body = "";
    View hero;
    int heroHeightDp = 170;
    final ArrayList<View> headerExtra = new ArrayList<>(), cards = new ArrayList<>();
    Runnable back;          // null: no way back (the start page)
    View focus;             // what gets the focus first (controller); default the first card
    Runnable onY;           // the controller's Y button on this page (optional)
    LogConsole console;     // L / R page it (optional)
    View custom;            // a page that lays itself out (the build page) instead of header + cards
    final ArrayList<View> animated = new ArrayList<>();
    LinearLayout cardColumn;

    Page(Context c, String id) {
        this.c = c;
        this.id = id;
    }

    Page head(String eyebrow, String title, String body) {
        this.eyebrow = eyebrow;
        this.title = title;
        this.body = body;
        return this;
    }

    Card card(Card k) {
        cards.add(k);
        return k;
    }

    View build(boolean landscape, int widthPx) {
        animated.clear();
        if (custom != null) {
            animated.add(custom);
            return custom;
        }
        final int gap = Theme.dp(10);
        cardColumn = new LinearLayout(c);
        cardColumn.setOrientation(LinearLayout.VERTICAL);
        LayoutTransition lt = new LayoutTransition();   // cards that appear / go (the texture pack row) slide instead of jumping
        lt.enableTransitionType(LayoutTransition.CHANGING);
        cardColumn.setLayoutTransition(lt);
        for (View k : cards) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = gap;
            cardColumn.addView(k, lp);
            animated.add(k);
        }
        LinearLayout header = new LinearLayout(c);
        header.setOrientation(LinearLayout.VERTICAL);
        if (!landscape) header.setGravity(Gravity.CENTER_HORIZONTAL);
        if (hero != null) {
            header.addView(hero, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Theme.dp(landscape ? Math.min(heroHeightDp, 130) : heroHeightDp)));
            animated.add(hero);
        }
        if (!eyebrow.isEmpty()) {
            TextView e = Theme.eyebrow(c, eyebrow);
            if (!landscape) Theme.centered(e);
            header.addView(e);
            animated.add(e);
        }
        if (!title.isEmpty()) {
            TextView t = Theme.heading(c, title, landscape ? 28 : 27);
            if (!landscape) Theme.centered(t);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = Theme.dp(4);
            header.addView(t, lp);
            animated.add(t);
            View rule = new Rule(c);
            LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(Theme.dp(140), Theme.dp(9));
            rp.topMargin = Theme.dp(8);
            rp.bottomMargin = Theme.dp(10);
            if (!landscape) rp.gravity = Gravity.CENTER_HORIZONTAL;
            header.addView(rule, rp);
            animated.add(rule);
        }
        if (!body.isEmpty()) {
            TextView b = Theme.body(c, body);
            if (!landscape) Theme.centered(b);
            header.addView(b);
            animated.add(b);
        }
        for (View v : headerExtra) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = Theme.dp(12);
            header.addView(v, lp);
            animated.add(v);
        }

        int side = Theme.dp(landscape ? 28 : 20);
        if (landscape) {
            LinearLayout row = new LinearLayout(c);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(side, 0, side, 0);
            ScrollView hs = scroller();
            FrameLayout hw = new FrameLayout(c);
            hw.addView(header, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER_VERTICAL));
            hw.setPadding(0, Theme.dp(12), 0, Theme.dp(12));
            hs.addView(hw, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            row.addView(hs, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.42f));
            View sp = new View(c);
            row.addView(sp, new LinearLayout.LayoutParams(Theme.dp(28), 1));
            ScrollView cs = scroller();
            FrameLayout cw = new FrameLayout(c);
            cw.setPadding(0, Theme.dp(16), Theme.dp(4), Theme.dp(16));
            cw.addView(cardColumn, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER_VERTICAL));
            cs.addView(cw, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            row.addView(cs, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.58f));
            return row;
        }
        ScrollView sv = scroller();
        LinearLayout col = new LinearLayout(c);
        col.setOrientation(LinearLayout.VERTICAL);
        int maxW = Theme.dp(580);
        int extra = widthPx > maxW + side * 2 ? (widthPx - maxW) / 2 : side;
        col.setPadding(extra, Theme.dp(4), extra, Theme.dp(28));
        col.addView(header);
        LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cp.topMargin = Theme.dp(22);
        col.addView(cardColumn, cp);
        sv.addView(col);
        return sv;
    }

    private ScrollView scroller() {
        ScrollView s = new ScrollView(c);
        s.setFillViewport(true);
        s.setVerticalScrollBarEnabled(false);
        s.setOverScrollMode(View.OVER_SCROLL_NEVER);
        s.setClipToPadding(false);
        s.setFocusable(false);   // focus goes to the cards, never to the scroller itself (its grey highlight)
        s.setDefaultFocusHighlightEnabled(false);
        return s;
    }

    /** Pieces float up into place one after another. */
    void animateIn(int dir) {
        long d = 70;
        for (View v : animated) {
            float a = v.getAlpha();   // a disabled card stays dimmed
            v.setAlpha(0);
            v.setTranslationY(Theme.dp(18));
            v.animate().alpha(a).translationY(0).setStartDelay(d).setDuration(420).setInterpolator(new DecelerateInterpolator(2f)).start();
            d += 45;
        }
    }

    View firstFocus() {
        if (focus != null) return focus;
        for (View v : cards) if (v.isFocusable() && v.getVisibility() == View.VISIBLE) return v;
        return null;
    }

    /** The small ornament under a title: a line with a crystal in the middle. */
    static final class Rule extends View {
        private final android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);

        Rule(Context c) {
            super(c);
        }

        @Override
        protected void onDraw(android.graphics.Canvas c) {
            float w = getWidth(), h = getHeight(), cy = h / 2;
            boolean centered = getParent() instanceof LinearLayout && (((LinearLayout) getParent()).getGravity() & Gravity.CENTER_HORIZONTAL) == Gravity.CENTER_HORIZONTAL;
            float cx = centered ? w / 2 : Theme.dpf(5);
            p.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
            p.setShader(new android.graphics.LinearGradient(centered ? 0 : cx, 0, w, 0, centered ? new int[] {0, Theme.GOLD, 0} : new int[] {Theme.GOLD, 0}, null,
                    android.graphics.Shader.TileMode.CLAMP));
            p.setStrokeWidth(Theme.dpf(1));
            c.drawLine(0, cy, w, cy, p);
            p.setShader(null);
            p.setColor(Theme.GOLD_LIGHT);
            android.graphics.Path d = new android.graphics.Path();
            float r = h / 2;
            d.moveTo(cx, cy - r);
            d.lineTo(cx + r * 0.6f, cy);
            d.lineTo(cx, cy + r);
            d.lineTo(cx - r * 0.6f, cy);
            d.close();
            c.drawPath(d, p);
        }
    }
}
