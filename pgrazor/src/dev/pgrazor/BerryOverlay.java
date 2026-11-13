package dev.pgrazor;

import android.app.Activity;
import android.graphics.*;
import android.view.View;
import java.io.InputStream;

/** Compact icon/count display and persistent, ordered catch-berry picker. */
final class BerryOverlay extends View {
    private static final int[] IDS=BerryPriorityPicker.IDS;
    private static final String[] NAMES=BerryPriorityPicker.NAMES;
    private final Activity activity;
    private final Bitmap[] icons=new Bitmap[IDS.length];
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final BerryPriorityPicker picker;

    BerryOverlay(Activity activity,Runnable changed){
        super(activity);this.activity=activity;this.picker=new BerryPriorityPicker(activity,changed);setClickable(true);setFocusable(true);
        for(int i=1;i<IDS.length;i++)try(InputStream in=PgAssets.open(activity, "pgrazor/map-icons/berry-"+IDS[i]+".png")){icons[i]=BitmapFactory.decodeStream(in);}catch(Exception ignored){}
        setOnClickListener(v->picker.show());
    }
    private int dp(float n){return MapTheme.dp(activity,n);}
    void applyUiScale(float scale){setPivotX(getWidth());setPivotY(getHeight()/2f);setScaleX(scale);setScaleY(scale);}
    protected void onSizeChanged(int w,int h,int oldw,int oldh){super.onSizeChanged(w,h,oldw,oldh);setPivotX(w);setPivotY(h/2f);}
    private int index(int id){for(int i=0;i<IDS.length;i++)if(IDS[i]==id)return i;return 0;}
    void refresh(){
        boolean visible=Control.encounterBerryVisible();setVisibility(visible?VISIBLE:GONE);
        if(!visible)picker.close();
        int id=Control.autoBerrySelected();
        setContentDescription("Catch berry: "+NAMES[index(id)]+(id==0?"":", "+Control.berryCount(id)+" available")+". Tap to choose berry priorities.");
        setEnabled(!Control.encounterBerryBusy());invalidate();picker.refresh();
    }
    void close(){picker.close();}
    protected void onDraw(Canvas c){
        super.onDraw(c);float w=getWidth(),h=getHeight();
        paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.surface());c.drawRoundRect(0,0,w,h,dp(14),dp(14),paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(1));paint.setColor(MapTheme.accent());c.drawRoundRect(dp(0.5f),dp(0.5f),w-dp(0.5f),h-dp(0.5f),dp(14),dp(14),paint);
        paint.setStyle(Paint.Style.FILL);
        int id=Control.autoBerrySelected(),i=index(id);Bitmap icon=icons[i];
        if(icon!=null){float size=dp(35),scale=Math.min(size/icon.getWidth(),size/icon.getHeight()),iw=icon.getWidth()*scale,ih=icon.getHeight()*scale;
            paint.setAlpha(Control.encounterBerryBusy()?140:255);c.drawBitmap(icon,null,new RectF(dp(23)-iw/2,h/2-ih/2,dp(23)+iw/2,h/2+ih/2),paint);paint.setAlpha(255);
        }else{
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(2));paint.setColor(MapTheme.ink());
            c.drawCircle(dp(23),h/2,dp(12),paint);c.drawLine(dp(15),h/2+dp(8),dp(31),h/2-dp(8),paint);paint.setStyle(Paint.Style.FILL);
        }
        paint.setColor(MapTheme.ink());paint.setTypeface(Typeface.DEFAULT_BOLD);paint.setTextAlign(Paint.Align.CENTER);
        String count=id==0?"":("("+Math.max(0,Control.berryCount(id))+")");
        paint.setTextSize(dp(count.length()>5?11:13));
        c.drawText(count,dp(65),h/2-(paint.ascent()+paint.descent())/2,paint);
    }
}
