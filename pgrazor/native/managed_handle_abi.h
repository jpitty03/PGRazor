#pragma once
#include "game_bindings.h"
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
 const uint32_t head[]=PGR_GAME_MANAGED_HANDLE_ABI_H_HEAD_FINGERPRINT;
 const uint32_t makeTail[]=PGR_GAME_MANAGED_HANDLE_ABI_H_MAKETAIL_FINGERPRINT,pro[]=PGR_GAME_MANAGED_HANDLE_ABI_H_PRO_FINGERPRINT,getTail[]=PGR_GAME_MANAGED_HANDLE_ABI_H_GETTAIL_FINGERPRINT,freeTail[]=PGR_GAME_MANAGED_HANDLE_ABI_H_FREETAIL_FINGERPRINT;
 const auto c=reinterpret_cast<uintptr_t>(create),g=reinterpret_cast<uintptr_t>(get),f=reinterpret_cast<uintptr_t>(free);
 if(!c||!g||!f||c>UINTPTR_MAX-PGR_GAME_SHARED_GC_CREATE_BRANCH_OFFSET||!words(c,head,5))return -1;
 const auto cb=branch(c+PGR_GAME_SHARED_GC_CREATE_BRANCH_OFFSET),gb=branch(g),fb=branch(f);
 if(!cb||!gb||!fb||cb>UINTPTR_MAX-PGR_GAME_SHARED_GC_CREATE_BODY_OFFSET||gb>UINTPTR_MAX-PGR_GAME_SHARED_GC_GET_TAIL_OFFSET||fb>UINTPTR_MAX-PGR_GAME_SHARED_GC_FREE_TAIL_OFFSET)return -1;
 return words(cb+PGR_GAME_SHARED_GC_CREATE_BODY_OFFSET,makeTail,1)&&words(gb,pro,1)&&words(gb+PGR_GAME_SHARED_GC_GET_TAIL_OFFSET,getTail,2)&&words(fb,pro,1)&&words(fb+PGR_GAME_SHARED_GC_FREE_TAIL_OFFSET,freeTail,2)?0:-1;
}
