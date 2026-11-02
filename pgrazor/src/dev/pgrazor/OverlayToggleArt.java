package dev.pgrazor;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.graphics.Typeface;

/** Shared 0.1.6 tile language; vector geometry and shaders are reused between frames. */
final class OverlayToggleArt {
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Typeface face=MapTheme.medium();
    private final Path roof=new Path(),berry=new Path(),leaf=new Path();
    private LinearGradient gradient;
    private int lastLime,lastAccent;

    OverlayToggleArt(){
        roof.moveTo(9,18);roof.lineTo(22,9);roof.lineTo(35,18);roof.close();
        berry.moveTo(36,24);berry.cubicTo(26,20,27,32,32,36);berry.cubicTo(35,40,40,39,44,33);
        berry.cubicTo(49,24,43,20,36,24);berry.close();
        leaf.moveTo(36,23);leaf.cubicTo(34,16,41,14,43,16);leaf.cubicTo(44,21,39,24,36,23);leaf.close();
    }
    private void palette(){
        int lime=MapTheme.accentLime(),accent=MapTheme.accent();
        if(gradient==null||lime!=lastLime||accent!=lastAccent){
            gradient=new LinearGradient(11,9,42,37,lime,accent,Shader.TileMode.CLAMP);lastLime=lime;lastAccent=accent;
        }
        paint.setShader(null);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeJoin(Paint.Join.ROUND);
    }
    private void tile(Canvas c,boolean on){
        palette();paint.setStyle(Paint.Style.FILL);paint.setColor((MapTheme.card()&0x00ffffff)|0xeb000000);
        c.drawRoundRect(1,1,53,53,16,16,paint);
        if(on){paint.setColor(MapTheme.accentSoft());c.drawRoundRect(1,1,53,53,16,16,paint);}
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(on?2:1);paint.setColor(on?MapTheme.accent():MapTheme.outline());
        c.drawRoundRect(on?2:1.5f,on?2:1.5f,on?52:52.5f,on?52:52.5f,15,15,paint);
    }
    private void chip(Canvas c,boolean on,boolean cooldown){
        paint.setShader(null);paint.setTypeface(face);paint.setTextSize(9.5f);paint.setTextAlign(Paint.Align.CENTER);
        paint.setStyle(on?Paint.Style.FILL:Paint.Style.STROKE);paint.setStrokeWidth(1);
        paint.setColor(on?MapTheme.accent():MapTheme.outline());c.drawRoundRect(15,39,39,50,5.5f,5.5f,paint);
        paint.setStyle(Paint.Style.FILL);paint.setColor(on?MapTheme.card():MapTheme.muted());
        c.drawText(on?"ON":cooldown?"CD":"OFF",27,47.4f,paint);
    }
    void vgp(Canvas c,boolean on){
        tile(c,on);c.saveLayerAlpha(0,0,54,38,on?255:90);c.save();c.rotate(-12,24,20);
        paint.setStyle(Paint.Style.FILL);paint.setShader(gradient);c.drawRoundRect(15,10,33,31,7,7,paint);paint.setShader(null);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1);paint.setColor((MapTheme.closeFill()&0x00ffffff)|0x59000000);
        c.drawRoundRect(15,10,33,31,7,7,paint);
        paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);c.drawCircle(24,18,3,paint);
        paint.setColor(0xb3ffffff);c.drawRoundRect(20,25,28,27,1,1,paint);c.restore();
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2);paint.setColor(MapTheme.accent());
        c.drawArc(22.71f,12,38.71f,28,-48.6f,97.2f,false,paint);
        c.drawArc(17.62f,7,43.62f,33,-43.8f,87.6f,false,paint);c.restore();chip(c,on,false);
    }
    void gym(Canvas c,boolean on,boolean cooldown){
        tile(c,on);c.saveLayerAlpha(0,0,54,39,on?255:90);
        paint.setStyle(Paint.Style.FILL);paint.setShader(gradient);c.drawPath(roof,paint);
        c.drawRoundRect(10,30,34,34,1.5f,1.5f,paint);c.drawRoundRect(8,34,36,37,1.5f,1.5f,paint);paint.setShader(null);
        paint.setColor(MapTheme.accent());c.drawRoundRect(12,18,32,21,1,1,paint);
        paint.setShader(gradient);c.drawRoundRect(14,21,17,30,.7f,.7f,paint);c.drawRoundRect(21,21,24,30,.7f,.7f,paint);c.drawRoundRect(28,21,31,30,.7f,.7f,paint);paint.setShader(null);
        paint.setColor(MapTheme.card());c.drawCircle(37,28,12,paint);
        paint.setShader(gradient);c.drawPath(berry,paint);paint.setShader(null);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1);paint.setColor((MapTheme.closeFill()&0x00ffffff)|0x59000000);c.drawPath(berry,paint);
        paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.accentLime());c.drawPath(leaf,paint);
        paint.setColor(Color.WHITE);c.drawCircle(33,27,.8f,paint);c.drawCircle(39,28,.8f,paint);c.drawCircle(35,32,.8f,paint);c.drawCircle(41,32,.8f,paint);
        c.restore();chip(c,on,cooldown);
    }
}
