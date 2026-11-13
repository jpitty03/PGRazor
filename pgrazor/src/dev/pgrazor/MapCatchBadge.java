package dev.pgrazor;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.view.View;

/** Small, touch-through map-catching badge, styled like the VGP overlays. */
final class MapCatchBadge extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private String label = "";
    MapCatchBadge(Context context) {
        super(context);
        setClickable(false); setFocusable(false);
    }
    void refresh(boolean visible) {
        label = visible ? Control.mapCatchLabel() : "";
        setVisibility(label.isEmpty() ? GONE : VISIBLE);
        setContentDescription("Map Auto Throw: " + label);
        if (!label.isEmpty()) invalidate();
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.save(); canvas.scale(getWidth()/54f, getHeight()/54f);
        paint.setStyle(Paint.Style.FILL); paint.setColor(SettingsUi.surface());
        canvas.drawRoundRect(1,1,53,53,14,14,paint);
        int tint = "CAUGHT".equals(label) ? Color.rgb(33,188,125)
            : "FLED".equals(label) || "STOPPED".equals(label) ? Color.rgb(235,126,85) : SettingsUi.accent();
        paint.setColor(tint); canvas.drawCircle(27,21,13,paint);
        paint.setColor(Color.WHITE); paint.setStrokeWidth(2.5f);
        canvas.drawLine(14,21,40,21,paint); canvas.drawCircle(27,21,4.5f,paint);
        paint.setColor(SettingsUi.surface()); canvas.drawCircle(27,21,2.2f,paint);
        if ("CATCH".equals(label) || "RETRY".equals(label)) {
            paint.setStyle(Paint.Style.STROKE); paint.setColor(tint); paint.setStrokeWidth(2);
            float rotation = (android.os.SystemClock.uptimeMillis()%1200)*.3f;
            canvas.drawArc(10,4,44,38,rotation,110,false,paint);
        }
        paint.setStyle(Paint.Style.FILL); paint.setColor(tint); paint.setTextSize(9);
        paint.setTypeface(Typeface.DEFAULT_BOLD); paint.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(label,27,48,paint);
        canvas.restore();
    }
}
