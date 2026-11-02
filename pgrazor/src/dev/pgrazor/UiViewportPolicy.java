package dev.pgrazor;

/** Pure layout policy, also exercised on the host JVM without Android stubs. */
final class UiViewportPolicy {
    private UiViewportPolicy() {}
    static float clamp(float value,int start,int end,int extent){
        return Math.max(start,Math.min(Math.max(start,end-Math.max(0,extent)),value));
    }
    static float fit(float requested,int width,int height,int availableWidth,int availableHeight){
        float scale=requested;
        if(width>0)scale=Math.min(scale,(float)Math.max(1,availableWidth)/width);
        if(height>0)scale=Math.min(scale,(float)Math.max(1,availableHeight)/height);
        return Math.max(0.01f,scale);
    }
    static int menuExtent(int desired,int available,float scale){
        return Math.max(1,Math.min(desired,(int)Math.floor(Math.max(1,available)/Math.max(.01f,scale))));
    }
}
