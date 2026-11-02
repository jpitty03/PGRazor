#include "../native/egg_hatch_policy.h"
#include "../native/egg_hatch_types.h"
#define CHECK(x) do{if(!(x))return __LINE__;}while(0)
using namespace pgr_egg;
namespace {bool family(void *base,void *actual){return base==(void*)1&&actual==(void*)2;}}

#ifdef __wasm__
extern "C" void *memset(void *p,int c,unsigned long n){auto b=static_cast<unsigned char*>(p);for(unsigned long i=0;i<n;++i)b[i]=c;return p;}
#endif
extern "C" int run_egg_tests(){
 // Normal hatch type, a derived variant (daily or other), and unrelated UI.
 CHECK(hatch_type((void*)1,(void*)1,family));
 CHECK(hatch_type((void*)1,(void*)2,family));
 CHECK(!hatch_type((void*)1,(void*)3,family));
 CHECK(!hatch_type(nullptr,(void*)2,family));
 CHECK(!hatch_type((void*)1,nullptr,family));
 Policy p;Observation o;uint64_t t=1000;
 CHECK(p.step(t,true,true,o)==0);
 // Regression: GUI-only opening prompt, no animation object or active-state edge.
 o.gui=true;t+=1000;CHECK(p.step(t,true,true,o)&START);
 unsigned taps=0;
 for(unsigned i=0;i<5;++i){t+=250;unsigned a=p.step(t,true,true,o);taps+=(a&TAP)!=0;CHECK(!(a&SKIP));}
 CHECK(taps==1);
 o.info=true;o.info_id=1;t+=250;CHECK(p.step(t,true,true,o)&SKIP);p.skip_result(true);
 for(unsigned i=0;i<11;++i){t+=250;CHECK(!(p.step(t,true,true,o)&(TAP|SKIP)));}
 t+=250;CHECK(p.step(t,true,true,o)&TAP);
 // The visible info identity joins the existing session; it cannot restart a lingering hatch.
 CHECK(p.info_id==1);
 o.screen=true;o.screen_id=7;o.special=false;t+=250;CHECK(!(p.step(t,true,true,o)&(TAP|CLOSE)));
 t+=750;CHECK(!(p.step(t,true,true,o)&CLOSE));t+=250;CHECK(p.step(t,true,true,o)&CLOSE);
 o.screen=false;for(unsigned i=0;i<24;++i){t+=250;CHECK(p.step(t,true,true,o)==0);}
 // A new GUI-only prompt requires disappearance then appearance.
 o.info=false;o.gui=false;t+=1000;p.step(t,true,true,o);o.gui=true;t+=1000;CHECK(p.step(t,true,true,o)&START);
 // Turning Off/on does not turn a persistent GUI into a new opening.
 t+=250;p.step(t,false,true,o);t+=250;CHECK(!(p.step(t,true,true,o)&(START|TAP)));
 // Close cooldown consumes a GUI rising edge rather than delaying it.
 Policy q;Observation x;x.active=true;t+=5000;CHECK(q.step(t,true,true,x)&START);
 x.active=false;x.screen=true;x.screen_id=1;x.special=false;t+=250;q.step(t,true,true,x);t+=1000;CHECK(q.step(t,true,true,x)&CLOSE);
 x.screen=false;x.gui=true;t+=250;CHECK(q.step(t,true,true,x)==0);t+=4000;CHECK(q.step(t,true,true,x)==0);
 // Shiny/perfect/unreadable summaries stay open, but the explicit option can be disabled.
 for(unsigned keep=0;keep<2;++keep){Policy k;Observation a;a.info=true;a.info_id=1;t+=5000;k.step(t,true,keep,a);
  a.screen=true;a.screen_id=3;a.special=true;t+=250;unsigned r=k.step(t,true,keep,a);CHECK(bool(r&KEEP)==bool(keep));
  t+=1000;r=k.step(t,true,keep,a);CHECK(bool(r&CLOSE)==!bool(keep));CHECK(!(r&TAP));}
 // A manually opened summary cannot start a session, and a changed summary cannot be closed.
 Policy m;Observation a;a.screen=true;a.screen_id=1;t+=5000;CHECK(!m.step(t,true,false,a));
 a.screen=false;a.info=true;a.info_id=1;t+=250;m.step(t,true,false,a);a.screen=true;t+=250;m.step(t,true,false,a);
 a.screen_id=2;t+=1500;CHECK(!(m.step(t,true,false,a)&CLOSE));
 // Managed skip failure falls back, never retries skip inside the same hatch, and bounds taps at 20.
 Policy f;Observation g;g.info=true;g.info_id=1;t+=5000;f.step(t,true,false,g);unsigned tries=0,nt=0;uint64_t last=0;
 for(unsigned i=0;i<160;++i){t+=250;unsigned r=f.step(t,true,false,g);if(r&SKIP){++tries;f.skip_result(false);}if(r&TAP){++nt;CHECK(!last||t-last>=900);last=t;}}
 CHECK(tries==1&&nt==20);t+=6000;CHECK(f.step(t,true,false,g)&TIMEOUT);
 // Regression: toggling keep-special cannot finish a screen that was already pending.
 Policy change;Observation before;before.info=true;before.info_id=10;t+=5000;change.step(t,true,true,before);
 before.screen=true;before.screen_id=20;before.special=false;t+=250;change.step(t,true,true,before);
 t+=1500;CHECK(!(change.step(t,true,false,before)&CLOSE));CHECK(!change.session);
 t+=250;CHECK(change.step(t,true,false,before)==0);
 before.screen=false;t+=250;CHECK(change.step(t,true,false,before)==0);
 // A genuinely new hatch can use the new choice, including closing a special Pokemon.
 before.info_id=11;t+=250;CHECK(change.step(t,true,false,before)&START);
 before.screen=true;before.screen_id=21;before.special=true;t+=250;CHECK(!(change.step(t,true,false,before)&KEEP));
 t+=1000;CHECK(change.step(t,true,false,before)&CLOSE);
 // A configuration update while suspended also revokes the old session.
 Policy suspended;Observation old;old.info=true;old.info_id=3;t+=5000;suspended.step(t,true,true,old);
 t+=250;suspended.step(t,false,false,{});t+=1000;CHECK(suspended.step(t,true,false,old)==0);
 // Regression: a delayed/opening summary must not consume the only close attempt.
 Policy delayed;Observation late;late.info=true;late.info_id=100;t+=5000;delayed.step(t,true,false,late);
 late.screen=true;late.screen_id=101;late.special=false;t+=250;delayed.step(t,true,false,late);
 t+=1000;CHECK(delayed.step(t,true,false,late)&CLOSE);CHECK(!delayed.done);delayed.close_result(t,false);
 t+=500;CHECK(!(delayed.step(t,true,false,late)&CLOSE));t+=250;CHECK(delayed.step(t,true,false,late)&CLOSE);
 delayed.close_result(t,true);CHECK(!delayed.done);
 t+=750;CHECK(!(delayed.step(t,true,false,late)&CLOSE));t+=250;CHECK(delayed.step(t,true,false,late)&CLOSE);
 delayed.close_result(t,true);late.screen=false;t+=250;CHECK(!(delayed.step(t,true,false,late)&CLOSE));CHECK(delayed.done);
 t+=1000;CHECK(!(delayed.step(t,true,false,late)&CLOSE));
 // Retry budget is finite, and no tap is sent to a Pokemon summary.
 Policy bounded;Observation held;held.info=true;held.info_id=110;t+=5000;bounded.step(t,true,false,held);
 held.screen=true;held.screen_id=111;held.special=false;t+=250;bounded.step(t,true,false,held);
 unsigned close_count=0;bool gave_up=false;
 for(unsigned i=0;i<40;++i){t+=250;unsigned r=bounded.step(t,true,false,held);CHECK(!(r&TAP));if(r&CLOSE){++close_count;bounded.close_result(t,false);}gave_up|=(r&TIMEOUT)!=0;}
 CHECK(close_count==6&&gave_up&&bounded.done);
 // Off, a changed summary or protected special must never get a retry.
 Policy revoked;Observation pending;pending.info=true;pending.info_id=120;t+=5000;revoked.step(t,true,false,pending);
 pending.screen=true;pending.screen_id=121;pending.special=false;t+=250;revoked.step(t,true,false,pending);
 t+=1000;CHECK(revoked.step(t,true,false,pending)&CLOSE);revoked.close_result(t,false);
 t+=750;CHECK(!(revoked.step(t,false,false,pending)&CLOSE));t+=750;CHECK(!(revoked.step(t,true,false,pending)&CLOSE));
 Policy switched;pending.screen=false;pending.info_id=130;t+=5000;switched.step(t,true,false,pending);
 pending.screen=true;pending.screen_id=131;t+=250;switched.step(t,true,false,pending);t+=1000;CHECK(switched.step(t,true,false,pending)&CLOSE);
 pending.screen_id=132;t+=750;CHECK(!(switched.step(t,true,false,pending)&CLOSE));
 return 0;
}
