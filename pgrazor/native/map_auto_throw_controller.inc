// Shared state machine; the game adapter below is replaced by the runtime fixture.
struct MapTapAdapter {
    std::string (*account)();
    bool (*eligible)(void*);
    uint64_t (*identity)(void*);
    bool (*alive)(void*);
    Root (*encounter)(void*);
    int (*poll_encounter)(void*,uint64_t,Root&);
    int (*active_berry)(void*);
    Root (*berry)(void*,int);
    int (*poll_berry)(void*,uint64_t,Root&,int&);
    bool (*throw_value)(PgrThrowValue&);
    int (*choose_ball)(void*,void*,const PgrCatchChanceOptions&,const PgrThrowValue&);
    Root (*capture)(void*,const PgrThrowValue&);
    int (*poll_capture)(void*,uint64_t,int&);
};
const MapTapAdapter& map_tap_adapter();
enum MapTapPhase {MapTapIdle,MapTapEncounter,MapTapBerry,MapTapCapture};
struct MapTapSession {
    Root target,promise,response,berry_response;
    std::string account;
    uint64_t id=0;
    MapTapPhase phase=MapTapIdle;
    int attempts=0,active_berry=0;
    bool uncertain=false,berry_prepared=false,stop_new=false;
    int64_t next_ms=0,promise_ms=0;
};
MapTapSession map_tap;
uint64_t map_tap_generation=0;
// No managed roots retained by dedup. Unknown outcomes cannot be replayed by another tap.
std::set<std::pair<std::string,uint64_t>> map_finished;
void map_event(const char* phase,const std::string& message,int value=0){
    auto tr=transport();if(!tr||!tr->emit)return;
    const std::string json="{\"phase\":"+json_quote(phase)+",\"message\":"+json_quote(message)+
        ",\"encounterId\":"+json_quote(std::to_string(map_tap.id))+",\"throw\":"+std::to_string(map_tap.attempts)+
        ",\"value\":"+std::to_string(value)+"}";
    PgrRuntimeTransportString s[]={{"map-auto-throw",14},{json.c_str(),uint32_t(json.size())}};
    tr->emit(19,nullptr,0,s,2);
}
void map_reset(){++map_tap_generation;map_tap=MapTapSession{};}
void map_done(const char* phase,const std::string& why,bool remember=false){
    map_event(phase,why);
    if((remember||map_tap.uncertain||map_tap.attempts)&&map_tap.id)map_finished.insert({map_tap.account,map_tap.id});
    map_reset();
}
void map_block(const std::string& why){map_done("stopped",why,true);}
struct MapTapPermission {PgrCatchChanceOptions config;bool foreground=false,coherent=false;};
MapTapPermission map_permission(){
    MapTapPermission p;auto tr=transport();PgrRuntimeTransportSample a{},z{};char raw[96]{};uint32_t n=0;int fg=0;
    if(!tr||!tr->sample||!tr->get_string||!tr->get_bool||tr->sample(&a)||!a.snapshot_fresh||a.fault)return p;
    if(tr->get_string(15,raw,sizeof(raw),&n)||n>=sizeof(raw)||tr->get_bool(3,&fg))return p;
    if(tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq)return p;
    raw[n]=0;p.config=pgr_parse_catch_chance(raw);p.coherent=p.config.valid;p.foreground=fg==1;return p;
}
// Slot 12 always publishes OneShotAutomation's inactive v1 message.
// An empty string is also idle; other command families remain exclusive.
bool map_command_idle(const char* raw,uint32_t n){
    if(!n)return true;
    std::vector<std::string> fields;uint32_t start=0;
    for(uint32_t i=0;i<=n;i++)if(i==n||raw[i]=='|'){fields.emplace_back(raw+start,i-start);start=i+1;}
    if(fields.size()!=8||fields[0]!="1"||fields[2]!="0"||fields[7]!="0")return false;
    for(size_t i=1;i<fields.size();i++){
        if(fields[i].empty()||fields[i].size()>64)return false;
        for(char c:fields[i])if(c<'0'||c>'9')return false;
    }
    return fields[5]=="0"||fields[5]=="1";
}
bool map_mode_on(const MapTapPermission& p){return p.coherent&&p.foreground&&p.config.autothrow&&p.config.onmap;}
bool map_tap_consume(void* tapped,bool gate){
    // A tap during an owned session must NOT open an encounter alongside its pending RPC.
    if(map_tap.target){map_event("busy","Finish the current map catch before tapping another Pokemon");return true;}
    if(!tapped||!gate)return false;
    const auto p=map_permission();if(!map_mode_on(p)||p.config.cooldown_ms)return false;
    const auto& a=map_tap_adapter();Root target(tapped);
    if(!a.eligible(target.get()))return false;
    const std::string account=a.account();const uint64_t id=a.identity(target.get());
    if(account.empty()||!id)return false;
    if(map_finished.count({account,id})){map_event("ignored","This encounter already completed or has an unknown outcome; not retried");return true;}
    if(map_finished.size()>=1024)return false;
    map_tap.target=std::move(target);map_tap.account=account;map_tap.id=id;
    map_tap.phase=MapTapEncounter;map_event("tap","Catching tapped Pokemon on the map");return true;
}
void map_poll(uint64_t tid,int64_t now){
    const auto& a=map_tap_adapter();const auto generation=map_tap_generation;
    Root response;int status=0,rc=0;
    if(map_tap.phase==MapTapEncounter)rc=a.poll_encounter(map_tap.promise.get(),tid,response);
    else if(map_tap.phase==MapTapBerry)rc=a.poll_berry(map_tap.promise.get(),tid,response,status);
    else rc=a.poll_capture(map_tap.promise.get(),tid,status);
    if(generation!=map_tap_generation)return;
    if(rc==PGR_VR_PENDING){if(now-map_tap.promise_ms>=kPromiseTimeoutMs)map_block("Request timed out; outcome unknown, not retried");return;}
    require(rc==PGR_VR_DONE,"Request rejected; outcome not retried");
    map_tap.promise.reset();map_tap.uncertain=false;
    if(map_tap.phase==MapTapEncounter){
        require(bool(response),"Encounter was not accepted");map_tap.response=std::move(response);
        map_tap.active_berry=a.active_berry(map_tap.response.get());map_tap.phase=MapTapCapture;return;
    }
    if(map_tap.phase==MapTapBerry){
        require(status==1||status==2,"Berry refused; no automatic retry");
        map_tap.berry_prepared=true;
        if(status==1){require(bool(response),"Berry response missing");map_tap.berry_response=std::move(response);map_tap.active_berry=a.active_berry(map_tap.berry_response.get());}
        map_event("berry-result",status==1?"Berry applied or already active":"Selected berry out of stock",status);
        map_tap.phase=MapTapCapture;return;
    }
    map_event("catch-result","Confirmed capture response",status);
    if(status==1||status==3){map_done(status==1?"caught":"fled",status==1?"Caught on the map":"Pokemon fled",true);return;}
    require(status==2||status==4,"Unknown catch result; no automatic retry");
    if(status==2){map_tap.active_berry=0;map_tap.berry_response.reset();map_tap.berry_prepared=false;}
    map_tap.next_ms=now+kThrowCadenceMs;
}
void step_map_auto_throw(uint64_t tid,bool blocked){
    if(!map_tap.target)return;
    const auto& a=map_tap_adapter();const auto p=map_permission();const int64_t now=clock_ms();
    if(!p.coherent||!p.foreground){map_block("Foreground or settings lease lost; tap abandoned without retry");return;}
    if(a.account()!=map_tap.account){map_block("Account changed; tap abandoned without retry");return;}
    if(blocked||!map_mode_on(p))map_tap.stop_new=true;
    if(now<map_tap.next_ms)return;map_tap.next_ms=now+250;
    // Poll first: the game may already have removed a successfully caught map object.
    if(map_tap.promise){map_poll(tid,now);return;}
    if(map_tap.stop_new){map_done("stopped","Map catching disabled; no new throw sent");return;}
    if(!a.alive(map_tap.target.get())||a.identity(map_tap.target.get())!=map_tap.id){map_done("gone","Pokemon or map no longer available");return;}
    if(p.config.cooldown_ms>0){map_done("stopped","Waiting for cooldown; tap again when ready");return;}
    if(map_tap.attempts>=100){map_done("stopped","Throw limit reached");return;}
    if(map_tap.phase==MapTapEncounter){
        map_tap.uncertain=true;map_tap.promise_ms=now;
        map_tap.promise=a.encounter(map_tap.target.get());require(bool(map_tap.promise),"Encounter returned no promise; not retried");
        map_event("encounter","Requesting map encounter");return;
    }
    PgrThrowValue value{};require(a.throw_value(value),"Throw settings unavailable; no throw sent");
    void* rate=map_tap.berry_response?map_tap.berry_response.get():map_tap.response.get();
    value.item=a.choose_ball(map_tap.target.get(),rate,p.config,value);
    if(!value.item){map_done("not-caught","No supported balls available");return;}
    if(p.config.berry&&!map_tap.active_berry&&!map_tap.berry_prepared){
        map_tap.uncertain=true;map_tap.promise_ms=now;map_tap.phase=MapTapBerry;
        map_tap.promise=a.berry(map_tap.target.get(),p.config.berry);
        if(!map_tap.promise){map_tap.uncertain=false;map_tap.berry_prepared=true;map_tap.phase=MapTapCapture;}
        else map_event("berry","Applying selected berry",p.config.berry);
        return;
    }
    map_tap.uncertain=true;map_tap.promise_ms=now;map_tap.phase=MapTapCapture;++map_tap.attempts;
    map_tap.promise=a.capture(map_tap.target.get(),value);require(bool(map_tap.promise),"Capture returned no promise; not retried");
    auto tr=transport();PgrRuntimeTransportString action{"Ball throw",10};if(tr)tr->emit(17,nullptr,0,&action,1);
    map_event("throw",std::string("Always Hit; radius=")+std::to_string(value.radius)+"; curve="+(value.spinning?"true":"false"),value.item);
}
