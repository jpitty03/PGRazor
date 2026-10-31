package dev.pgrazor;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.RectF;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import java.io.InputStream;
import android.graphics.Typeface;
import android.view.View;
import android.widget.Toast;

/** Draggable map handle; Pad owns its OverlayDrag, visibility, scale, and lifecycle. */
public final class GymFeedOverlay extends View {
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final Bitmap gymIcon,berryIcon;

    public GymFeedOverlay(Context context){
        super(context);
        gymIcon=loadIcon(context,"gym-automation.png");berryIcon=loadIcon(context,"berry-701.png");
        setClickable(true);setFocusable(true);
        setOnClickListener(v->{
            boolean wasOn=GymAutomationSettings.running();
            boolean nowOn=GymAutomationSettings.toggleRunning();
            if(!wasOn&&!nowOn)
                Toast.makeText(getContext(),GymAutomationSettings.status(),Toast.LENGTH_SHORT).show();
            refresh(true);
        });
    }

    /** Pad calls this on each heartbeat, hiding it beneath settings and quick menu. */
    public void refresh(boolean visible){
        boolean on=GymAutomationSettings.running();
        setVisibility(visible&&GymAutomationSettings.overlayEnabled()?VISIBLE:GONE);
        setContentDescription("Gym Slotter and Berry Feeder "+(on?"on; tap to stop":"off; tap to start")
            +". Drag to reposition. "+GymAutomationSettings.status());
        invalidate();
    }

    public void close(){setOnClickListener(null);setVisibility(GONE);}

    private static Bitmap loadIcon(Context context,String name){
        try(InputStream in=context.getAssets().open("pgrazor/map-icons/"+name)){
            BitmapFactory.Options options=new BitmapFactory.Options();
            options.inSampleSize="gym-automation.png".equals(name)?8:1;
            return BitmapFactory.decodeStream(in,null,options);
        }catch(Exception ignored){return null;}
    }
    private void drawIcon(Canvas canvas,Bitmap icon,float left,float top,float right,float bottom){
        if(icon==null)return;
        float scale=Math.min((right-left)/icon.getWidth(),(bottom-top)/icon.getHeight());
        float w=icon.getWidth()*scale,h=icon.getHeight()*scale;
        float x=(left+right-w)/2,y=(top+bottom-h)/2;
        canvas.drawBitmap(icon,null,new RectF(x,y,x+w,y+h),paint);
    }

    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);
        boolean on=GymAutomationSettings.running();
        canvas.save();canvas.scale(getWidth()/54f,getHeight()/54f);
        paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.surface());
        canvas.drawRoundRect(1,1,53,53,14,14,paint);
        // User-supplied transparent gym symbol with the bundled Razz Berry badge.
        paint.setColorFilter(new PorterDuffColorFilter(MapTheme.ink(),PorterDuff.Mode.SRC_IN));
        drawIcon(canvas,gymIcon,6,4,39,39);
        paint.setColorFilter(null);
        paint.setColor(MapTheme.surface());canvas.drawCircle(38,30,13,paint);
        drawIcon(canvas,berryIcon,26,17,50,41);
        paint.setColor(on?Color.rgb(56,181,112):MapTheme.ink());
        paint.setTextSize(11);paint.setTypeface(Typeface.DEFAULT_BOLD);paint.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(on?"ON":Cooldown.remaining()>0?"CD":"OFF",27,49,paint);
        canvas.restore();
    }
}
