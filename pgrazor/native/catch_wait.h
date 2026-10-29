#pragma once
#include "runtime_bootstrap.h"
// Feature58 replaces production quick-summary.js presentation wait reduction.
// nullptr config uses compiled bootstrap54 and native dlsym. Explicit config is
// the native fixture seam; no JavaScript pointer marshalling is needed in production.
struct PgrCatchWaitConfig {
 uint32_t version,size;
 const PgrBootBinding *binding;
 void *class_get_name;
 pgr_meta_seg_fn segment;
 void *segment_context;
 void *fixture_move_next; // native fixture only, requires explicit segment resolver
};
struct PgrCatchWaitSample {
 uint32_t version,size,ready,active;
 int32_t fault,owner_tid;
 uint32_t queued;
 int32_t success_value;
 uint64_t observed,shortened;
};
struct PgrCatchWaitEvent {
 uint32_t kind,branch; // 1 observed,2 shortened
 int32_t tid,status;
 float original_seconds,new_seconds;
 uint64_t boottime_ms;
};
struct PgrCatchWaitApi {
 uint32_t version,size;
 int (*install)(const PgrCatchWaitConfig *); // 1 retry bootstrap/classes;0 ready;negative fail
 int (*update)(int allowed,uint32_t lease_ms); // bounded2500ms; no persistent enable
 int (*sample)(PgrCatchWaitSample *);
 int (*next_event)(PgrCatchWaitEvent *);
 int (*stop)(); // terminal pass-through, no trampoline destruction
};
static_assert(sizeof(PgrCatchWaitConfig)==48,"config");
static_assert(sizeof(PgrCatchWaitSample)==48,"sample");
static_assert(sizeof(PgrCatchWaitEvent)==32,"event");
static_assert(sizeof(PgrCatchWaitApi)==48,"API");
extern "C" const void *pgrazor_catch_wait_api(uint32_t);
