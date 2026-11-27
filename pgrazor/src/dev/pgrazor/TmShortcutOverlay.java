package dev.pgrazor;

import android.app.Activity;
import android.graphics.*;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.Toast;
import java.io.InputStream;

/** Fast/Charged TM icons placed before each move's power number on the game's Pokemon screen. */
final class TmShortcutOverlay {
    private static final String[] NAMES={"Fast TM","Charged TM","Charged TM"};
    private final Activity activity;
    private final ImageView[] icons=new ImageView[3];
    private final int[] counts=new int[3];
    private boolean unavailable;

    TmShortcutOverlay(Activity activity,FrameLayout parent){
        this.activity=activity;
        Bitmap fast=load("item-1201.png"),charged=load("item-1202.png");
        for(int i=0;i<3;i++){
            final int row=i;ImageView v=new ImageView(activity);v.setImageBitmap(i==0?fast:charged);v.setScaleType(ImageView.ScaleType.FIT_CENTER);
            v.setContentDescription("Use "+NAMES[i]);v.setVisibility(View.GONE);
            v.setOnClickListener(x->{if(counts[row]<=0)Toast.makeText(activity,"No "+NAMES[row]+" available",Toast.LENGTH_SHORT).show();else NativeCore.tmUse(row);});
            parent.addView(v,new FrameLayout.LayoutParams(dp(40),dp(22)));icons[i]=v;
        }
    }
    private Bitmap load(String name){try(InputStream in=PgAssets.open(activity,"pgrazor/map-icons/"+name)){return BitmapFactory.decodeStream(in);}catch(Exception e){return null;}}
    private int dp(float n){return MapTheme.dp(activity,n);}

    void refresh(boolean allowed){
        float[] s=null;
        if(allowed&&!unavailable){try{s=NativeCore.tmState();}catch(UnsatisfiedLinkError e){unavailable=true;}}
        for(int i=0;i<3;i++){
            ImageView v=icons[i];boolean show=s!=null&&s.length==12&&s[i*4]>0;
            v.setVisibility(show?View.VISIBLE:View.GONE);if(!show)continue;
            View parent=(View)v.getParent();counts[i]=(int)s[i*4+3];
            v.setX(s[i*4+1]*parent.getWidth()-dp(40)-dp(30));v.setY(s[i*4+2]*parent.getHeight()-dp(11));
            v.setAlpha(counts[i]>0?1f:0.45f);
            if(counts[i]>0)v.clearColorFilter();else{ColorMatrix m=new ColorMatrix();m.setSaturation(0);v.setColorFilter(new ColorMatrixColorFilter(m));}
        }
    }
    void close(){for(ImageView v:icons)((FrameLayout)v.getParent()).removeView(v);}
}
