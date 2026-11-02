#include "../native/manual_transfer_policy.h"
#define CHECK(x) do{if(!(x))return __LINE__;}while(0)
using namespace pgr_manual;
extern "C" int run_manual_tests(){
 unsigned comparisons=0;
 for(unsigned bits=0;bits<16;++bits)for(unsigned on=0;on<2;++on)for(unsigned result=0;result<2;++result)for(unsigned fav=0;fav<2;++fav){
  Pokemon p{bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8)};
  const bool allowed=p.known&&!p.favorite&&!p.egg&&!p.gym;
  CHECK(transferable(result,on,fav,p)==bool(result||(on&&!fav&&allowed)));
  CHECK(disabled(result,on,p)==bool(result&&!(on&&allowed)));++comparisons;
 }
 CHECK(comparisons==128);
 // There must have been an explicit manual start, even during the first seconds after boot.
 Window w;CHECK(!w.capture(1000));CHECK(!w.recent(1000,30000));
 w.begin(1000);CHECK(w.capture(1100));CHECK(!w.due(1499));CHECK(w.due(1500));
 CHECK(!w.expired(6100));CHECK(w.expired(6101));w.drop();CHECK(!w.due(1600));
 // Ordinary confirmation is left intact; a later special warning can still be recognized.
 CHECK(confirm(false,2,1,2,true)==-1);CHECK(w.capture(2000));CHECK(w.due(2400));
 CHECK(confirm(true,2,1,2,true)==1);CHECK(confirm(true,2,2,2,true)==0);
 w.consume();CHECK(!w.capture(2500));CHECK(!w.due(2500));
 // Off, stale permission or a new manual start revokes any pending dialog.
 w.begin(5000);CHECK(w.capture(5100));w.revoke();CHECK(!w.due(5600));CHECK(!w.capture(5600));
 w.begin(6000);CHECK(w.capture(6100));w.begin(6200);CHECK(!w.due(6600));
 CHECK(w.capture(6500));CHECK(!w.due(6499));CHECK(w.expired(6499));
 CHECK(!w.recent(26200,20000));CHECK(!w.capture(26200));CHECK(w.recent(26200,30000));
 CHECK(!w.recent(36200,30000));
 // Ambiguous buttons/counts never authorize a click, regardless of order.
 for(unsigned count=0;count<5;++count)for(unsigned mask=0;mask<16;++mask)for(unsigned ui=0;ui<5;++ui){
  int expected=count==2&&ui==2&&(mask==1||mask==2)?(mask==1?1:0):-1;
  CHECK(confirm(true,count,mask,ui,true)==expected);
  CHECK(confirm(true,count,mask,ui,false)==-1);
  CHECK(confirm(false,count,mask,ui,true)==-1);
 }
 return 0;
}
