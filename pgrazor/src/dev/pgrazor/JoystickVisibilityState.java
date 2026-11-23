package dev.pgrazor;

/** Hide immediately; require continuous map/menu evidence before showing again. */
final class JoystickVisibilityState {
    static final long SETTLE_MS=650, FRESH_MS=1500;
    private long mapAt,menuAt,mapSince,menuSince;

    private static boolean fresh(long at,long now){return at>0&&at<=now&&now-at<FRESH_MS;}
    synchronized void map(boolean visible,long at,long now){
        if(!visible||!fresh(at,now)){mapAt=mapSince=0;return;}
        if(!fresh(mapAt,now)||at<mapAt)mapSince=at;
        mapAt=at;
    }
    synchronized void menu(boolean clear,long at,long now){
        if(!clear||!fresh(at,now)){menuAt=menuSince=0;return;}
        if(!fresh(menuAt,now)||at<menuAt)menuSince=at;
        menuAt=at;
    }
    synchronized boolean visible(long now){
        if(!fresh(mapAt,now)||!fresh(menuAt,now))return false;
        // Both streams must have progressed past the same uninterrupted interval.
        // Waiting on one cached sample alone must not reveal the joystick.
        return Math.min(mapAt,menuAt)-Math.max(mapSince,menuSince)>=SETTLE_MS;
    }
    synchronized void reset(){mapAt=menuAt=mapSince=menuSince=0;}
}
