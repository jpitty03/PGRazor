package dev.pgrazor;

import android.content.Context;
import android.graphics.*;
import android.view.View;

/**
 * Small canvas-drawn menu glyph shared by the quick menu and the settings home screen, so a
 * feature carries the same icon in both places. Drawn on a 24-unit grid and independent of emoji
 * fonts; Routes uses the image it shares with GameRoutesOverlay.
 */
final class MenuIcon extends View {
    static final int MAP = 0, WALK = 1, AUTOWALK = 2, FEEDS = 3, FAVORITES = 4, TELEPORT = 5, ACCOUNTS = 6, SETTINGS = 7,
        EVENTS = 8, AUTOMATION = 9, HIDE = 10, ROUTES = 11, CATCH = 12, BOX = 13, BAG = 14, BATTLE = 15, HEART = 16, RESEARCH = 17;

    private final int kind;
    /** Draws in the muted colour instead of ink. */
    boolean disabled;
    /** Draws in the theme accent colour instead of ink. */
    boolean accent;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Bitmap routeIcon;

    MenuIcon(Context c, int kind) {
        super(c); this.kind = kind; setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        Bitmap decoded = null;
        if (kind == ROUTES) {
            try (java.io.InputStream in = PgAssets.open(c,"pgrazor/map-icons/routes.png")) {
                decoded = BitmapFactory.decodeStream(in);
            } catch (Exception ignored) { }
        }
        routeIcon = decoded;
    }

    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float scale = Math.min(getWidth(), getHeight()) / 24f; canvas.save(); canvas.scale(scale, scale);
        paint.setColor(disabled ? SettingsUi.muted() : accent ? SettingsUi.accent() : SettingsUi.ink());
        paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
        switch (kind) {
            case ROUTES: {
                if (routeIcon != null) {
                    float ratio = Math.min(22f / routeIcon.getWidth(), 22f / routeIcon.getHeight());
                    float w = routeIcon.getWidth() * ratio, h = routeIcon.getHeight() * ratio;
                    paint.setColorFilter(new PorterDuffColorFilter(paint.getColor(), PorterDuff.Mode.SRC_IN));
                    canvas.drawBitmap(routeIcon, null, new RectF(12-w/2, 12-h/2, 12+w/2, 12+h/2), paint);
                    paint.setColorFilter(null);
                } else {
                    paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.5f);
                    Path path = new Path(); path.moveTo(7,16); path.lineTo(16,16); path.lineTo(16,8.5f); path.lineTo(8.5f,8.5f);
                    canvas.drawPath(path,paint); canvas.drawCircle(7,16,1.5f,paint); canvas.drawCircle(8.5f,8.5f,1.5f,paint);
                }
                break;
            }
            case EVENTS: {
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                canvas.drawRoundRect(4, 5, 20, 21, 2, 2, paint);
                canvas.drawLine(4, 10, 20, 10, paint);
                canvas.drawLine(8, 3, 8, 7, paint); canvas.drawLine(16, 3, 16, 7, paint);
                canvas.drawLine(8, 14, 10, 14, paint); canvas.drawLine(14, 14, 16, 14, paint);
                canvas.drawLine(8, 18, 10, 18, paint);
                break;
            }
            case MAP: {
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                Path m = new Path(); m.moveTo(4, 6); m.lineTo(10, 4); m.lineTo(16, 6); m.lineTo(20, 4);
                m.lineTo(20, 18); m.lineTo(16, 20); m.lineTo(10, 18); m.lineTo(4, 20); m.close();
                canvas.drawPath(m, paint);
                canvas.drawLine(10, 4, 10, 18, paint); canvas.drawLine(16, 6, 16, 20, paint);
                break;
            }
            case WALK: {
                paint.setStyle(Paint.Style.FILL); canvas.drawCircle(12, 5, 2.2f, paint);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2f);
                canvas.drawLine(12, 8, 12, 14, paint);
                canvas.drawLine(12, 14, 9, 19, paint); canvas.drawLine(12, 14, 15, 19, paint);
                canvas.drawLine(12, 10, 9, 13, paint); canvas.drawLine(12, 10, 15, 12, paint);
                break;
            }
            case AUTOWALK: {
                paint.setStyle(Paint.Style.FILL); canvas.drawCircle(13, 5, 2.2f, paint);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2f);
                canvas.drawLine(13, 8, 13, 14, paint);
                canvas.drawLine(13, 14, 10, 19, paint); canvas.drawLine(13, 14, 16, 19, paint);
                canvas.drawLine(13, 10, 16, 12, paint);
                paint.setStrokeWidth(1.6f);
                canvas.drawLine(3, 9, 7, 9, paint); canvas.drawLine(2, 12, 6, 12, paint); canvas.drawLine(3, 15, 7, 15, paint);
                break;
            }
            case FEEDS: {
                paint.setStyle(Paint.Style.FILL); canvas.drawCircle(6.5f, 17.5f, 1.8f, paint);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2f);
                canvas.drawArc(new RectF(5 - 5, 19 - 5, 5 + 5, 19 + 5), -90, 90, false, paint);
                canvas.drawArc(new RectF(5 - 9, 19 - 9, 5 + 9, 19 + 9), -90, 90, false, paint);
                break;
            }
            case FAVORITES: {
                paint.setStyle(Paint.Style.FILL);
                Path star = new Path(); float cx = 12, cy = 12, ro = 8, ri = 3.4f;
                for (int i = 0; i < 10; i++) {
                    double ang = Math.toRadians(-90 + i * 36); float rr = (i % 2 == 0) ? ro : ri;
                    float px = cx + rr * (float) Math.cos(ang), py = cy + rr * (float) Math.sin(ang);
                    if (i == 0) star.moveTo(px, py); else star.lineTo(px, py);
                }
                star.close(); canvas.drawPath(star, paint);
                break;
            }
            case TELEPORT: {
                paint.setStyle(Paint.Style.FILL);
                Path nav = new Path(); nav.moveTo(12, 3.5f); nav.lineTo(19.5f, 19.5f); nav.lineTo(12, 15.5f); nav.lineTo(4.5f, 19.5f); nav.close();
                canvas.drawPath(nav, paint);
                break;
            }
            case ACCOUNTS: {
                // A person glyph paired with circular switch arrows: identity plus "change account".
                paint.setStyle(Paint.Style.FILL); canvas.drawCircle(9, 7.5f, 3f, paint);
                Path body = new Path(); body.addArc(new RectF(3, 11, 15, 23), 180, 180); body.lineTo(15, 18); body.lineTo(3, 18); body.close();
                canvas.drawPath(body, paint);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                canvas.drawArc(new RectF(14, 6, 22, 14), -70, 200, false, paint);
                Path tip = new Path(); tip.moveTo(14.4f, 7.2f); tip.lineTo(16.2f, 4.6f); tip.lineTo(18.3f, 7f); canvas.drawPath(tip, paint);
                break;
            }
            case SETTINGS: {
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2f);
                for (int i = 0; i < 8; i++) {
                    double ang = Math.toRadians(i * 45); float c = (float) Math.cos(ang), s2 = (float) Math.sin(ang);
                    canvas.drawLine(12 + c * 5.5f, 12 + s2 * 5.5f, 12 + c * 8f, 12 + s2 * 8f, paint);
                }
                canvas.drawCircle(12, 12, 5.2f, paint); canvas.drawCircle(12, 12, 1.8f, paint);
                break;
            }
            case AUTOMATION: {
                // A lightning bolt: one-tap automation that fires once on demand.
                paint.setStyle(Paint.Style.FILL);
                Path bolt = new Path(); bolt.moveTo(13, 3); bolt.lineTo(6, 13.5f); bolt.lineTo(11, 13.5f);
                bolt.lineTo(10, 21); bolt.lineTo(18, 10); bolt.lineTo(13, 10); bolt.close();
                canvas.drawPath(bolt, paint);
                break;
            }
            case HIDE: {
                // An eye with a line through it.
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                Path eye = new Path(); eye.moveTo(3, 12); eye.quadTo(12, 3, 21, 12); eye.quadTo(12, 21, 3, 12); eye.close();
                canvas.drawPath(eye, paint);
                canvas.drawCircle(12, 12, 3, paint);
                paint.setStrokeWidth(2f); canvas.drawLine(5, 19, 19, 5, paint);
                break;
            }
            case CATCH: {
                // A Poké Ball: outer ring, centre button and the seam either side of it.
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                canvas.drawCircle(12, 12, 8.2f, paint); canvas.drawCircle(12, 12, 2.6f, paint);
                canvas.drawLine(3.8f, 12, 9.4f, 12, paint); canvas.drawLine(14.6f, 12, 20.2f, 12, paint);
                break;
            }
            case BOX: {
                // A storage box with a lid and a handle slot.
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                canvas.drawRoundRect(3.5f, 5, 20.5f, 9.5f, 1.2f, 1.2f, paint);
                Path box = new Path(); box.moveTo(5, 9.5f); box.lineTo(5, 20); box.lineTo(19, 20); box.lineTo(19, 9.5f);
                canvas.drawPath(box, paint);
                canvas.drawLine(10, 13.5f, 14, 13.5f, paint);
                break;
            }
            case BAG: {
                // A backpack: body, top handle and a front pocket.
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                canvas.drawRoundRect(5, 8, 19, 21, 3, 3, paint);
                canvas.drawArc(new RectF(9, 3.5f, 15, 12.5f), 180, 180, false, paint);
                canvas.drawRoundRect(8.5f, 13.5f, 15.5f, 17.5f, 1, 1, paint);
                break;
            }
            case BATTLE: {
                // Two crossed swords, hilts at the bottom.
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.9f);
                canvas.drawLine(5, 4, 18.5f, 17.5f, paint); canvas.drawLine(19, 4, 5.5f, 17.5f, paint);
                canvas.drawLine(13.5f, 17, 18, 12.5f, paint); canvas.drawLine(6, 12.5f, 10.5f, 17, paint);
                canvas.drawLine(18.5f, 17.5f, 20.5f, 19.5f, paint); canvas.drawLine(5.5f, 17.5f, 3.5f, 19.5f, paint);
                break;
            }
            case HEART: {
                paint.setStyle(Paint.Style.FILL);
                Path heart = new Path(); heart.moveTo(12, 20.5f);
                heart.cubicTo(3, 14, 2.5f, 8, 7, 5.5f); heart.cubicTo(9.5f, 4.3f, 12, 6, 12, 8.5f);
                heart.cubicTo(12, 6, 14.5f, 4.3f, 17, 5.5f); heart.cubicTo(21.5f, 8, 21, 14, 12, 20.5f); heart.close();
                canvas.drawPath(heart, paint);
                break;
            }
            case RESEARCH: {
                // A clipboard with two task lines.
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f);
                canvas.drawRoundRect(5.5f, 5, 18.5f, 21, 2, 2, paint);
                canvas.drawLine(9, 12, 15, 12, paint); canvas.drawLine(9, 16, 15, 16, paint);
                paint.setStyle(Paint.Style.FILL); canvas.drawRoundRect(9, 3, 15, 7, 1, 1, paint);
                break;
            }
        }
        canvas.restore();
    }
}
