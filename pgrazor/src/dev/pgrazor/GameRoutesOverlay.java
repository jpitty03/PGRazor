package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.*;
import android.view.View;
import java.io.InputStream;

/** Small active-run indicator. Dragging/scaling is owned by Pad, as for other overlays. */
final class GameRoutesOverlay extends View {
    private final Activity activity;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final Bitmap icon;
    private AlertDialog details;

    GameRoutesOverlay(Activity activity) {
        super(activity);this.activity=activity;
        Bitmap decoded=null;
        try(InputStream in=PgAssets.open(activity, "pgrazor/map-icons/routes.png")) {
            decoded=BitmapFactory.decodeStream(in);
        } catch(Exception ignored) { }
        icon=decoded;setClickable(true);setFocusable(true);
        setOnClickListener(v->{
            if(details!=null&&details.isShowing())return;
            details=GameRoutesDetailsDialog.show(activity);
        });
    }

    void refresh(boolean allowed) {
        boolean active=Control.gameRoutesRunning();
        setVisibility(allowed&&active?VISIBLE:GONE);
        if(active)setContentDescription("In-game Routes: "+Control.gameRoutesStatus()+". Tap for current path; drag to move.");
        invalidate();
    }

    void close(){if(details!=null)details.dismiss();details=null;}

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);canvas.save();canvas.scale(getWidth()/48f,getHeight()/48f);
        paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.surface());canvas.drawCircle(24,24,23,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.5f);paint.setColor(MapTheme.accent());canvas.drawCircle(24,24,22,paint);
        paint.setStyle(Paint.Style.FILL);
        if(icon!=null){float ratio=Math.min(32f/icon.getWidth(),32f/icon.getHeight()),w=icon.getWidth()*ratio,h=icon.getHeight()*ratio;
            canvas.drawBitmap(icon,null,new RectF(24-w/2,23-h/2,24+w/2,23+h/2),paint);
        }else{
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(3);paint.setStrokeCap(Paint.Cap.ROUND);
            Path path=new Path();path.moveTo(14,32);path.lineTo(32,32);path.lineTo(32,17);path.lineTo(17,17);canvas.drawPath(path,paint);
            canvas.drawCircle(14,32,3,paint);canvas.drawCircle(17,17,3,paint);paint.setStyle(Paint.Style.FILL);
        }
        paint.setColor(MapTheme.surface());canvas.drawCircle(38,38,6,paint);
        paint.setColor(MapTheme.accent());canvas.drawCircle(38,38,4,paint);canvas.restore();
    }
}
