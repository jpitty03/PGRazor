package dev.pgrazor;

import android.graphics.Rect;
import android.os.Build;
import android.view.View;
import android.view.WindowInsets;

/** Window-local bounds; works when the host game already fits its content inside the bars. */
final class UiViewport {
    private UiViewport() {}
    static Rect bounds(View view){
        int left=0,top=0,right=0,bottom=0;
        WindowInsets insets=view.getRootWindowInsets();
        if(insets!=null){
            if(Build.VERSION.SDK_INT>=30){
                android.graphics.Insets i=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout()|WindowInsets.Type.ime());
                left=i.left;top=i.top;right=i.right;bottom=i.bottom;
            }else{
                left=insets.getSystemWindowInsetLeft();top=insets.getSystemWindowInsetTop();
                right=insets.getSystemWindowInsetRight();bottom=insets.getSystemWindowInsetBottom();
                if(Build.VERSION.SDK_INT>=28&&insets.getDisplayCutout()!=null){
                    android.view.DisplayCutout c=insets.getDisplayCutout();
                    left=Math.max(left,c.getSafeInsetLeft());top=Math.max(top,c.getSafeInsetTop());
                    right=Math.max(right,c.getSafeInsetRight());bottom=Math.max(bottom,c.getSafeInsetBottom());
                }
            }
        }
        View root=view.getRootView();int[] at=new int[2],origin=new int[2];
        view.getLocationInWindow(at);root.getLocationInWindow(origin);
        int x=at[0]-origin[0],y=at[1]-origin[1];
        left=Math.max(0,left-x);top=Math.max(0,top-y);
        right=Math.max(0,x+view.getWidth()-(root.getWidth()-right));
        bottom=Math.max(0,y+view.getHeight()-(root.getHeight()-bottom));
        left=Math.min(left,view.getWidth());top=Math.min(top,view.getHeight());
        return new Rect(left,top,Math.max(left,view.getWidth()-right),Math.max(top,view.getHeight()-bottom));
    }
}
