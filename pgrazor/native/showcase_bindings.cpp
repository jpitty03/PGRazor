#include "showcase_bindings.h"
#include "game_bindings.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"
#include <time.h>
#include <unistd.h>
#include <limits>

namespace pgr_showcase {
namespace {
uint64_t boottime() {
  timespec t{};
  if(clock_gettime(CLOCK_BOOTTIME,&t)!=0 || t.tv_sec<0 || t.tv_nsec<0)return 0;
  return uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;
}
bool owner_valid(uint64_t owner,uint64_t generation) {
  if(!owner || owner!=uint64_t(gettid()) || !generation)return false;
  const auto* api=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));
  PgrUnitySchedulerSnapshot s{};
  const bool scheduled=api && !api->snapshot(&s) && s.installed && !s.poisoned &&
    s.owner_tid>0 && uint64_t(s.owner_tid)==owner && s.generation==generation &&
    api->valid(generation)==1;
  if(!scheduled)return false;
  const auto* bootstrap=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
  PgrBootBinding boot{};
  return bootstrap && bootstrap->snapshot(&boot)==0 && boot.runtime.thread_current &&
    reinterpret_cast<void*(*)()>(boot.runtime.thread_current)()!=nullptr;
}
bool live(const ScoreRequest& r) {
  const auto now=boottime();
  return now && r.deadline_ms>now && r.deadline_ms-now<=2500 && owner_valid(r.owner,r.generation);
}
void* resolve(const PgrMetaApi* api,const char* ns,const char* name) {
  PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;
  q.canonical_match=PGR_MD_MATCH_EXACT;
  PgrMetaClassResult r{};
  return api->resolve_class(&q,&r)==0?const_cast<void*>(r.klass):nullptr;
}
}
bool resolve_live_score_bindings(const ScoreRequest& r,ScoreBindings& out) {
  out={};
  if(!live(r))return false;
  const auto* api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
  const auto* bootstrap=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
  PgrBootBinding boot{};
  if(!api || !bootstrap || bootstrap->snapshot(&boot)!=0 || !boot.module_base ||
     !boot.runtime.thread_current || !live(r))return false;
  if(!reinterpret_cast<void*(*)()>(boot.runtime.thread_current)())return false;
  ScoreBindings b{};
  b.service_class=resolve(api,"Niantic.Holoholo.Contests","ContestService");
  if(!b.service_class || !live(r))return false;
  b.pokemon_class=resolve(api,"Holoholo.Rpc","PokemonProto");
  if(!b.pokemon_class || !live(r))return false;
  b.contest_class=resolve(api,"Holoholo.Rpc","ContestProto");
  if(!b.contest_class || !live(r))return false;
  b.single_class=resolve(api,"System","Single");
  if(!b.single_class || !live(r))return false;
  PgrMetaFieldQuery f{};f.version=1;f.size=sizeof(f);f.owner=b.pokemon_class;
  f.name="id_";f.offset=PGR_GAME_SHOWCASE_BINDINGS_CPP_POKEMON_ID_OFFSET;
  f.forbidden_mask=0x50;f.kind=PGR_MD_FIELD_VALUE;f.value_size=8;
  f.type_name="System.UInt64";f.type_match=PGR_MD_MATCH_EXACT;
  PgrMetaFieldResult fr{};
  if(api->validate_field(&f,&fr)!=0 || !live(r))return false;
  b.pokemon_id_field=const_cast<void*>(fr.field);
  constexpr uint8_t prefix[]=PGR_GAME_SHOWCASE_BINDINGS_CPP_SCORE_FINGERPRINT;
  constexpr uint64_t rva=PGR_GAME_SHOWCASE_BINDINGS_CPP_SCORE_RVA;
  if(rva>boot.module_size || sizeof(prefix)>boot.module_size-rva ||
     uintptr_t(boot.module_base)>std::numeric_limits<uintptr_t>::max()-rva)return false;
  PgrMetaMethodQuery m{};m.version=1;m.size=sizeof(m);m.owner=b.service_class;
  m.name="bnen";m.return_type="System.Single";m.return_match=PGR_MD_MATCH_EXACT;
  m.param_count=2;m.param_types[0]="Holoholo.Rpc.PokemonProto";m.param_types[1]="Holoholo.Rpc.ContestProto";
  m.param_match[0]=m.param_match[1]=PGR_MD_MATCH_EXACT;
  m.expected_code=reinterpret_cast<void*>(uintptr_t(boot.module_base)+rva);
  m.prologue=prefix;m.prologue_len=sizeof(prefix);
  PgrMetaMethodResult mr{};
  if(api->validate_method(&m,&mr)!=0 || !live(r))return false;
  b.score_method=mr.method;
  b.root=reinterpret_cast<decltype(b.root)>(boot.runtime.gchandle_new);
  b.target=reinterpret_cast<decltype(b.target)>(boot.runtime.gchandle_get_target);
  b.release=reinterpret_cast<decltype(b.release)>(boot.runtime.gchandle_free);
  b.object_class=reinterpret_cast<decltype(b.object_class)>(boot.runtime.object_get_class);
  b.invoke=reinterpret_cast<decltype(b.invoke)>(boot.runtime.runtime_invoke);
  b.unbox=reinterpret_cast<decltype(b.unbox)>(boot.runtime.object_unbox);
  b.field_get=reinterpret_cast<decltype(b.field_get)>(boot.runtime.field_get_value);
  b.boottime_ms=boottime;b.owner_valid=owner_valid;
  if(!b.root || !b.target || !b.release || !b.object_class || !b.invoke || !b.unbox ||
     !b.field_get || !b.score_method || !b.pokemon_id_field || !live(r))return false;
  out=b;return true;
}
}
