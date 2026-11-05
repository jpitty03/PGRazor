package dev.pgrazor;

/** Pure layout policy, also exercised on the host JVM without Android stubs. */
final class UiViewportPolicy {
    private UiViewportPolicy() {}
    static float clamp(float value,int start,int end,int extent){
        return Math.max(start,Math.min(Math.max(start,end-Math.max(0,extent)),value));
    }
    /** Preserve the fraction of the usable track when size or safe insets change. */
    static float resizePosition(float position,int oldStart,int oldEnd,int oldExtent,
                                int start,int end,int extent){
        int oldTrack=Math.max(0,oldEnd-oldStart-Math.max(0,oldExtent));
        float fraction=oldTrack>0?Math.max(0f,Math.min(1f,(position-oldStart)/oldTrack)):0f;
        return clamp(start+fraction*Math.max(0,end-start-Math.max(0,extent)),start,end,extent);
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
