#include "showcase_score.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <cstdio>
namespace {
using namespace pgr_showcase;
int classes[4], method, field;
struct Object {void* cls;uint64_t id;float value;};
Object objects[2][4];unsigned bank;uint64_t now;
bool owner_ok, throw_error, mutate_id, expire, wrong_box, recurse;
bool root_failed, expire_cleanup, no_result;
int roots, releases, invokes;
ScoreBindings bindings;ScoreRequest request;
void move_objects(){
  unsigned next=1-bank;
  std::memcpy(objects[next],objects[bank],sizeof(objects[bank]));
  for(auto& obj:objects[bank])obj.cls=nullptr; // stale pointers become invalid
  bank=next;
}
void* target(uintptr_t h){return h>=1&&h<=4?&objects[bank][h-1]:nullptr;}
void* klass(void* o){return static_cast<Object*>(o)->cls;}
uintptr_t root(void* o,int pinned){
  assert(!pinned);assert(o==target(4));if(root_failed)return 0;++roots;move_objects();return 4;
}
void release(uintptr_t h){assert(h==4);++releases;move_objects();if(expire_cleanup)now=request.deadline_ms;}
void field_get(void* o,void* f,void* out){assert(f==&field);assert(o==target(2));std::memcpy(out,&static_cast<Object*>(o)->id,8);}
void* unbox(void* o){assert(o==target(4));return &static_cast<Object*>(o)->value;}
uint64_t clock_now(){return now;}
bool owner(uint64_t tid,uint64_t generation){return owner_ok&&tid==71&&generation==7;}
void* invoke(const void* m,void* self,void** args,void** exc){
  assert(m==&method);assert(self==target(1));assert(args[0]==target(2));assert(args[1]==target(3));++invokes;
  if(recurse){double value=99;assert(read_score(bindings,request,value)==ScoreStatus::refused);assert(value==0);}
  move_objects();
  if(mutate_id)objects[bank][1].id++;
  if(expire)now=request.deadline_ms;
  if(wrong_box)objects[bank][3].cls=&classes[0];
  if(throw_error)*exc=target(3);
  return no_result?nullptr:target(4);
}
void reset(){
  bank=0;now=100;roots=releases=invokes=0;
  owner_ok=true;throw_error=mutate_id=expire=wrong_box=recurse=false;
  root_failed=expire_cleanup=no_result=false;
  for(int i=0;i<4;++i)objects[0][i]={&classes[i],123,876.5f};
  bindings={root,target,release,klass,invoke,unbox,field_get,clock_now,owner,
    &classes[0],&classes[1],&classes[2],&classes[3],&method,&field};
  request={71,2600,123,1,2,3,7};
}
void expect(ScoreStatus expected){double value=999;assert(read_score(bindings,request,value)==expected);assert(value==(expected==ScoreStatus::valid?876.5:0));assert(roots==releases);}
}
void test_showcase_score(){
  reset();expect(ScoreStatus::valid);assert(roots==1&&invokes==1);
  reset();owner_ok=false;expect(ScoreStatus::refused);assert(!invokes&&!roots);
  reset();request.deadline_ms=2601;expect(ScoreStatus::refused);assert(!invokes);
  reset();request.deadline_ms=100;expect(ScoreStatus::refused);
  reset();now=0;expect(ScoreStatus::refused);
  reset();request.pokemon_id=124;expect(ScoreStatus::bad_identity);assert(!invokes);
  reset();bindings.score_method=nullptr;expect(ScoreStatus::bad_binding);assert(!invokes);
  reset();throw_error=true;expect(ScoreStatus::managed_error);assert(!roots);
  reset();expire=true;expect(ScoreStatus::refused);assert(!roots);
  reset();mutate_id=true;expect(ScoreStatus::bad_identity);assert(roots==1);
  reset();wrong_box=true;expect(ScoreStatus::bad_score);assert(roots==1);
  reset();objects[0][3].value=std::numeric_limits<float>::quiet_NaN();expect(ScoreStatus::bad_score);
  reset();objects[0][3].value=std::numeric_limits<float>::infinity();expect(ScoreStatus::bad_score);
  reset();objects[0][3].value=0;expect(ScoreStatus::bad_score);
  reset();recurse=true;expect(ScoreStatus::valid);assert(invokes==1);
  reset();root_failed=true;expect(ScoreStatus::bad_score);assert(!roots);
  reset();expire_cleanup=true;expect(ScoreStatus::refused);assert(roots==1);
  reset();no_result=true;expect(ScoreStatus::bad_score);assert(!roots);
  reset();request.pokemon=0;expect(ScoreStatus::bad_identity);assert(!invokes);
  reset();request.generation=8;expect(ScoreStatus::refused);assert(!invokes);
  std::puts("PASS native Showcase score reader: 20 relocating-GC, identity, lease, reentry, exception and malformed-result scenarios");
}
