#pragma once
#include "runtime_metadata.h"
#include <cstring>
// Supported Unity6000.3.22f1 ARM64 pointer-width GC handle ABI. Read-only proof;
// no GC operation occurs before all code ranges and instruction words match.
inline int pgr_pointer_handles(const PgrMetaApi *md,const void *create,const void *get,const void *free){
 auto words=[&](uintptr_t address,const uint32_t *expected,uint32_t count){
  PgrMetaCodeQuery q{};q.version=1;q.size=sizeof(q);q.address=reinterpret_cast<void *>(address);
  q.length=count*4;q.prologue=expected;q.prologue_len=count*4;return md->validate_code(&q)==0;
 };
 auto branch=[&](uintptr_t address)->uintptr_t{
  PgrMetaCodeQuery q{};q.version=1;q.size=sizeof(q);q.address=reinterpret_cast<void *>(address);q.length=4;
  if(md->validate_code(&q))return 0;
  uint32_t op;std::memcpy(&op,q.address,4);if((op>>26)!=5)return 0;
  int64_t delta=int64_t(op&0x3ffffff);if(delta&0x2000000)delta-=0x4000000;delta*=4;
  if((delta<0&&address<uint64_t(-delta))||(delta>0&&address>UINTPTR_MAX-uint64_t(delta)))return 0;
  return delta<0?address-uintptr_t(-delta):address+uintptr_t(delta);
 };
 const uint32_t head[]={0xaa0003e8,0x52800049,0x7200003f,0x1a890520,0xaa0803e1};
 const uint32_t makeTail[]={0x9a953120},pro[]={0xf81b0ffe},getTail[]={0x9273c817,0x394082e8},freeTail[]={0x9273c816,0x394082c8};
 const auto c=reinterpret_cast<uintptr_t>(create),g=reinterpret_cast<uintptr_t>(get),f=reinterpret_cast<uintptr_t>(free);
 if(!c||!g||!f||c>UINTPTR_MAX-20||!words(c,head,5))return -1;
 const auto cb=branch(c+20),gb=branch(g),fb=branch(f);
 if(!cb||!gb||!fb||cb>UINTPTR_MAX-0x384||gb>UINTPTR_MAX-20||fb>UINTPTR_MAX-24)return -1;
 return words(cb+0x384,makeTail,1)&&words(gb,pro,1)&&words(gb+20,getTail,2)&&words(fb,pro,1)&&words(fb+24,freeTail,2)?0:-1;
}
