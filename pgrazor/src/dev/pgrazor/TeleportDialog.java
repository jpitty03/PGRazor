package dev.pgrazor;
import android.app.Activity;
import android.app.AlertDialog;
import android.os.Handler;
import android.os.Looper;
import java.util.Locale;

/** Shared confirmation for every user teleport; no action runs before explicit confirmation. */
public final class TeleportDialog {
    private TeleportDialog() {}
    public interface Action { boolean apply(double latitude,double longitude); }
    private static AlertDialog active;
    static String duration(long millis) {
        long seconds=(Math.max(0,millis)+999)/1000;
        return String.format(Locale.US,"%dh %02dm %02ds",seconds/3600,(seconds/60)%60,seconds%60);
    }
    private static String warning(double lat,double lon) {
        return String.format(Locale.US,"Destination: %.6f, %.6f\n\n",lat,lon)
            +"Cooldown when you arrive: "+duration(Cooldown.remainingAt(lat,lon));
    }
    public static AlertDialog confirm(Activity activity,double lat,double lon,Action teleport) {
        if(activity.isFinishing()||activity.isDestroyed())return null;
        if(!CooldownState.valid(lat,lon))throw new IllegalArgumentException("Invalid teleport coordinates");
        if(active!=null&&active.isShowing())return active;
        Handler main=new Handler(Looper.getMainLooper());
        AlertDialog dialog=new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle("Teleport cooldown")
            .setMessage(warning(lat,lon)).setNegativeButton("Cancel",null).setPositiveButton("Teleport",null).create();
        Runnable refresh=new Runnable(){public void run(){if(dialog.isShowing()){dialog.setMessage(warning(lat,lon));main.postDelayed(this,1000);}}};
        active=dialog;
        DialogLifecycle.bind(activity,dialog,()->{main.removeCallbacksAndMessages(null);if(active==dialog)active=null;});
        dialog.setOnShowListener(d->{MapTheme.styleButtons(dialog);main.post(refresh);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{if(teleport.apply(lat,lon))dialog.dismiss();});
        });dialog.show();return dialog;
    }
}
