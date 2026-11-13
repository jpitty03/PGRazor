#pragma once
#include <atomic>
#include <cstdio>
#include <cstring>

// Published only after reading the game's named WildBall enum literal. This is
// the inventory ball, not WildBallPremier (supplied by special encounters).
inline std::atomic<int> pgr_safari_inventory_item{0};
inline bool pgr_is_inventory_safari_ball(int item) {
    return item > 0 && item == pgr_safari_inventory_item.load();
}

struct PgrCatchChanceOptions {
    bool valid=false, show=false, autoball=false, safari=false, autothrow=false, onmap=false;
    long long cooldown_ms=0;
    int target=75, berry=0;
};
inline PgrCatchChanceOptions pgr_parse_catch_chance(const char *s) {
    PgrCatchChanceOptions c;
    if (!s) return c;
    int show=0, automatic=0, target=0, berry=0, safari=0, throwing=0, onmap=0, end=0;
    long long cooldown=0;
    if (std::strncmp(s,"C4|",3)==0) {
        // C4 adds the subordinate Auto Throw "catch on the map tap" flag. It is meaningful
        // only while throwing==1; the controller still requires autothrow before acting.
        if (std::sscanf(s+3,"%d|%d|%d|%d|%d|%d|%lld|%d%n",&show,&automatic,&target,&berry,&safari,&throwing,&cooldown,&onmap,&end)!=8 || s[3+end]) return c;
        if(throwing<0||throwing>1||cooldown<0||cooldown>7200000||onmap<0||onmap>1)return c;
    } else if (std::strncmp(s,"C3|",3)==0) {
        if (std::sscanf(s+3,"%d|%d|%d|%d|%d|%d|%lld%n",&show,&automatic,&target,&berry,&safari,&throwing,&cooldown,&end)!=7 || s[3+end]) return c;
        if(throwing<0||throwing>1||cooldown<0||cooldown>7200000)return c;
    } else if (std::strncmp(s,"C2|",3)==0) {
        if (std::sscanf(s+3,"%d|%d|%d|%d|%d%n",&show,&automatic,&target,&berry,&safari,&end)!=5 || s[3+end]) return c;
    } else if (std::strncmp(s,"C1|",3)==0) {
        int fields=std::sscanf(s+3,"%d|%d|%d|%d%n",&show,&automatic,&target,&berry,&end);
        if (fields==3) {
            berry=0;
            if(std::sscanf(s+3,"%d|%d|%d%n",&show,&automatic,&target,&end)!=3) return c;
        } else if(fields!=4) return c;
        if(s[3+end]) return c;
    } else return c;
    if(show<0||show>1||automatic<0||automatic>1||safari<0||safari>1||target<1||target>99) return c;
    c.autothrow=throwing==1;c.cooldown_ms=cooldown;c.onmap=onmap==1;
    c.valid=true;c.show=show==1;c.autoball=automatic==1;c.safari=safari==1;c.target=target;
    c.berry=berry>=701&&berry<=799?berry:0;
    return c;
}

// A separate preference augments Auto Ball; it never enables Auto Ball itself.
inline bool pgr_manual_safari_pick(bool automatic, bool preference, bool mighty,
                                  bool allowed, int stock, int resolved_item) {
    return automatic && preference && mighty && allowed && stock>0 && resolved_item>0;
}
