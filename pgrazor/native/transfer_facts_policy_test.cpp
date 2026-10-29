// Couple the ACTUAL feature35 reader to ACTUAL feature4 policy. Reuse only the fake managed-runtime
// scaffold; its baseline main is renamed and is never called. No ReleasePokemon/RPC is invoked.
#define main transfer_facts_baseline_main_unused
#include "transfer_facts_test.cpp"
#undef main
#include "transfer_policy.h"

namespace {
PgrTransferInput policyInput(const PgrTransferFactsOut &out,uint32_t protect){
  PgrTransferInput v{};v.version=1;v.size=sizeof(v);
  v.attack=out.attack;v.defense=out.defense;v.stamina=out.stamina;v.species=out.species;
  v.protect=protect;v.known=out.known;v.flags=out.flags;
  return v;
}
PgrTransferFactsOut readFacts(FakePokemon &pk,uint32_t protect){
  const auto req=make_request(&pk,protect);PgrTransferFactsOut out{};
  assert(g_api->inspect(&req,&out)==PGR_TF_VALID);
  assert(out.known==255&&out.caught_id==pk.id&&g_handles.empty());return out;
}
void resetPolicyFixture(){assert(g_handles.empty());reset_flags();g_size_val=12;}
}

int main(int argc,char **argv){
  assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
  const auto get=reinterpret_cast<GetApiFn>(dlsym(lib,"pgrazor_native_api"));assert(get);
  const auto *core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);
  g_api=static_cast<const PgrTransferFactsApi*>(core->query_feature(35,1));
  const auto *policy=static_cast<const PgrTransferApi*>(core->query_feature(4,1));
  assert(g_api&&g_api->size==24&&policy&&policy->version==1&&policy->size==16);
  auto cfg=make_config();assert(g_api->install(&cfg)==PGR_TF_OK);

  resetPolicyFixture();
  {auto display=make_display();auto pk=make_pokemon(&display);
   const auto out=readFacts(pk,63);auto input=policyInput(out,63);
   assert(out.flags==0&&policy->decide(&input)==0);}

  // Reader/policy flag schemas must agree directly; never translate through a copied decision rule.
  for(uint32_t bit=0;bit<6;++bit){
    resetPolicyFixture();auto display=make_display();auto pk=make_pokemon(&display);
    if(bit==0)g_rare_val=1;
    else if(bit==1)g_shiny_val=1;
    else if(bit==2)g_count_val=1;
    else if(bit==3)display.background=&g_gm;
    else g_size_val=bit==4?10:14;
    const uint32_t protect=1u<<bit;
    auto out=readFacts(pk,protect);assert(out.flags&protect);
    auto input=policyInput(out,protect);assert(policy->decide(&input)!=0);
    out=readFacts(pk,0);input=policyInput(out,0);assert(policy->decide(&input)==0);
    if(bit>=4)assert(!(out.flags&48));  // legacy: size is not queried when neither size keep is enabled
  }

  for(bool egg:{false,true}){
    resetPolicyFixture();auto display=make_display();auto pk=make_pokemon(&display);
    if(egg)pk.egg=1;else pk.favorite=1;
    const auto out=readFacts(pk,0);assert(out.flags&(egg?128u:64u));
    auto input=policyInput(out,0);assert(policy->decide(&input)!=0);  // unconditional keeps
  }

  for(int sum:{40,41}){
    resetPolicyFixture();auto display=make_display();auto pk=make_pokemon(&display);
    pk.attack=15;pk.defense=15;pk.stamina=sum-30;
    auto input=policyInput(readFacts(pk,0),0);
    assert((policy->decide(&input)==0)==(sum==40));
  }

  resetPolicyFixture();
  {auto display=make_display();auto pk=make_pokemon(&display);auto input=policyInput(readFacts(pk,0),0);
   const int32_t keep=25,other=26;
   input.keep_count=1;input.keep_ids=&keep;assert(policy->decide(&input)!=0);
   input.keep_ids=&other;assert(policy->decide(&input)==0);
   input.keep_count=0;input.keep_ids=nullptr;input.keep_all=1;assert(policy->decide(&input)!=0);}

  // An unsuccessful read cannot manufacture eligibility by supplying zeroed IVs/flags with known=0.
  resetPolicyFixture();
  {auto display=make_display();auto pk=make_pokemon(&display);auto req=make_request(&pk,0);
   req.lease_ms=0;PgrTransferFactsOut out{};
   assert(g_api->inspect(&req,&out)==PGR_TF_OK&&out.known==0);
   auto input=policyInput(out,0);assert(policy->decide(&input)!=0);
   req.lease_ms=2000;pk.favorite=2;
   assert(g_api->inspect(&req,&out)<0&&out.known==0);
   input=policyInput(out,0);assert(policy->decide(&input)!=0);assert(g_handles.empty());}
  puts("PASS actual facts35->policy4: category masks, unconditional keeps, IV40/41, custom keeps, unknown facts fail closed");
  dlclose(lib);
}
