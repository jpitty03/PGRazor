// Host fixture: compile with gym_automation.cpp and the host Android log shim.
// Including the implementation deliberately exercises its private poll state machine.
#include <cassert>
#include <cstdio>
#include <map>
#include "gym_runtime.cpp"

namespace {
std::map<uintptr_t, void*> test_roots;
uintptr_t test_next_handle = 1;
unsigned test_allocations = 0, test_frees = 0, test_polls = 0;
unsigned test_events = 0, test_unexpected_runtime = 0;
int test_result = PGR_VR_DONE;
int test_objects[8]{};
bool test_active = true, test_map = true, test_fresh = true;
bool test_coordinates = true, test_changing_sequence = false;
unsigned test_samples = 0;
int test_sample(PgrRuntimeTransportSample *out) {
 *out = {};
 out->snapshot_fresh = test_fresh;
 out->snapshot_seq = 100 + (test_changing_sequence ? test_samples : 0);
 ++test_samples;
 return 0;
}
int test_get_bool(uint32_t field, int *out) {
 switch (field) {
 case 0: *out = 1; break; // accountManagementAllowed is deliberately true.
 case 3: *out = test_active; break;
 case 16: *out = 1; break;
 case 38: *out = test_map; break;
 default: assert(false && "unexpected permission boolean");
 }
 return 0;
}
int test_get_double(uint32_t field, double *out, int *has) {
 assert(field < 2);
 *out = field == 0 ? 37.0 : -122.0;
 *has = test_coordinates;
 return 0;
}
int test_get_string(uint32_t field, char *out, uint32_t cap, uint32_t *len) {
 assert(field == 8);
 constexpr char config[] = "1;1;1;0;50;50";
 *len = sizeof(config) - 1;
 assert(cap >= *len);
 memcpy(out, config, *len);
 return 0;
}

uintptr_t test_root_new(void *object, int pinned) {
 assert(object && pinned == 0);
 const auto handle = test_next_handle++;
 assert(test_roots.emplace(handle, object).second);
 ++test_allocations;
 return handle;
}
void *test_root_get(uintptr_t handle) {
 const auto it = test_roots.find(handle);
 assert(it != test_roots.end());
 return it->second;
}
void test_root_free(uintptr_t handle) {
 assert(test_roots.erase(handle) == 1);
 ++test_frees;
}
void *test_forbidden_class(void*) {
 ++test_unexpected_runtime;
 assert(false && "canceled/rejected poll must not interpret managed objects");
 return nullptr;
}
int test_emit(uint32_t opcode, const int64_t *scalars, uint32_t scalar_count,
              const PgrRuntimeTransportString *strings, uint32_t string_count) {
 assert(opcode == 19 && !scalars && scalar_count == 0);
 assert(strings && string_count == 2);
 ++test_events;
 return 0;
}
int test_poll(void *promise, uint32_t phase, uint64_t tid,
              PgrVgpResultsResult *out) {
 assert(promise == &test_objects[1] && phase == 1 && tid == 123);
 ++test_polls;
 *out = {};
 if (test_result == PGR_VR_DONE) {
  out->version = 1;
  out->size = sizeof(*out);
  out->state = 1;
  out->value_handle = test_root_new(&test_objects[7], 0);
 }
 return test_result;
}
PgrRuntimeTransportApi test_transport{};
PgrVgpResultsApi test_results{1, sizeof(PgrVgpResultsApi), nullptr, test_poll, nullptr};

Permission test_setup(int phase) {
 assert(test_roots.empty());
 pending = Pending{};
 history = History{};
 paused = false;
 status.clear();
 next_scan = 0;
 owner = 123;
 test_allocations = test_frees = test_polls = test_events = 0;
 test_unexpected_runtime = 0;
 test_result = PGR_VR_DONE;
 boot = {};
 boot.runtime.gchandle_new = reinterpret_cast<void*>(test_root_new);
 boot.runtime.gchandle_get_target = reinterpret_cast<void*>(test_root_get);
 boot.runtime.gchandle_free = reinterpret_cast<void*>(test_root_free);
 boot.runtime.object_get_class = reinterpret_cast<void*>(test_forbidden_class);
 test_transport = {};
 test_transport.emit = test_emit;
 pending.gym = Root(&test_objects[0]);
 pending.promise = Root(&test_objects[1]);
 pending.reflection = Root(&test_objects[2]);
 pending.details = Root(&test_objects[3]);
 pending.bag = Root(&test_objects[4]);
 pending.gm = Root(&test_objects[5]);
 pending.buddy = Root(&test_objects[6]);
 // Keep enumerator empty: disposal would legitimately require managed metadata.
 pending.phase = phase;
 pending.config = "original";
 pending.id = "gym-fixture";
 pending.defender = 42;
 pending.since = clock_ms();
 Permission p;
 p.allowed = true;
 p.config = pending.config;
 p.settings.deploy = p.settings.feed = true;
 return p;
}
void test_cleared(unsigned polls, unsigned events) {
 assert(pending.phase == 0);
 assert(!pending.gym.h && !pending.promise.h && !pending.reflection.h);
 assert(!pending.details.h && !pending.enumerator.h && !pending.bag.h);
 assert(!pending.gm.h && !pending.buddy.h);
 assert(test_roots.empty() && test_allocations == test_frees);
 assert(test_polls == polls && test_events == events);
 assert(test_unexpected_runtime == 0);
}
}

extern "C" __attribute__((visibility("default"))) void *il2cpp_object_get_virtual_method(void *receiver,void *base){
 assert(receiver==&test_objects[0]&&base==&test_objects[1]);return &test_objects[2];
}
void *test_virtual_invoke(void *m,void *receiver,void **args,void **exception){
 assert(m==&test_objects[2]&&receiver==&test_objects[0]);
 if(args)assert(*static_cast<int64_t*>(args[0])==123456);
 *exception=nullptr;return &test_objects[3];
}
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t) {
 assert(false && "poll fixture must not bootstrap");
 return nullptr;
}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t) {
 assert(false && "poll fixture must not resolve metadata");
 return nullptr;
}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t version) {
 assert(version == 1);
 return &test_transport;
}
extern "C" const void *pgrazor_vgp_results_api(uint32_t version) {
 assert(version == 1);
 return &test_results;
}

int main() {
 // Exercise the real typed transport permission gate before poll fixtures.
 test_transport = {};
 test_transport.sample = test_sample;
 test_transport.get_bool = test_get_bool;
 test_transport.get_double = test_get_double;
 test_transport.get_string = test_get_string;
 test_active = false;
 assert(!permission().allowed); // Slot 0 cannot substitute for active slot 3.
 test_active = true;
 auto permitted = permission();
 assert(permitted.allowed && permitted.settings.deploy && permitted.settings.feed);
 assert(permitted.config == "1;1;1;0;50;50" && permitted.seq == 100);
 test_fresh = false;
 assert(!permission().allowed);
 test_fresh = true;
 test_changing_sequence = true;
 test_samples = 0;
 assert(!permission().allowed);
 test_changing_sequence = false;
 test_coordinates = false;
 assert(!permission().allowed);
 test_coordinates = true;
 test_map = false;
 assert(!permission().allowed);
 test_map = true;
 assert(permission().allowed);
 for (int phase : {2, 3, 6}) {
  for (bool config_change : {false, true}) {
   auto p = test_setup(phase);
   if (config_change) p.config = "changed";
   else p.allowed = false;
   poll(p);
   test_cleared(1, 0);
   assert(test_allocations == 8); // Seven pending roots plus adopted response.
   assert(!paused && history.feeds.empty() && history.gyms.empty());
   assert(next_scan == 0);
  }
 }
 for (int phase : {2, 3, 6}) {
  auto p = test_setup(phase);
  pending.since = clock_ms() - 15001;
  poll(p);
  test_cleared(0, 1);
  assert(paused && status.find("timed out") != std::string::npos);
 }
 for (bool config_change : {false, true}) {
  auto p = test_setup(5);
  if (config_change) p.config = "changed";
  else p.allowed = false;
  poll(p);
  test_cleared(0, 0);
  assert(!paused);
 }
 {
  auto p = test_setup(3);
  test_result = PGR_VR_REJECTED;
  poll(p);
  test_cleared(1, 1);
  assert(!history.feed_allowed(42, clock_ms()));
  assert(!history.feed_attempt(42, clock_ms()));
  assert(history.feed_allowed(43, clock_ms()));
  assert(next_scan > clock_ms() && !paused);
 }
 {
  auto p = test_setup(4);
  p.config = "changed";
  poll(p);
  test_cleared(0, 0);
  assert(!paused && next_scan == 0);
 }
 {
  test_setup(1);
  const auto now=clock_ms();
  inspected["gym-fixture"]=now+60000;
  inspected["other-gym"]=now+60000;
  history.refuse(42,now);
  retry_no_action(now);
  test_cleared(0,0);
  assert(next_scan==now+5000 && inspected.at("gym-fixture")==now+5000);
  assert(inspected.at("other-gym")==now+60000);
  assert(!paused && !history.feed_allowed(42,now+5000));
 }
 {
  test_setup(1);clear_pending();
  boot.handle=dlopen(nullptr,RTLD_NOW);assert(boot.handle);
  boot.runtime.runtime_invoke=reinterpret_cast<void*>(test_virtual_invoke);
  Root receiver(&test_objects[0]);
  assert(virtual_call(receiver,&test_objects[1])==&test_objects[3]);
  int64_t deadline=123456;void *args[]={&deadline};
  assert(virtual_call(receiver,&test_objects[1],args)==&test_objects[3]);
  receiver.reset();assert(test_roots.empty());dlclose(boot.handle);boot.handle=nullptr;
 }
 std::puts("PASS: virtual gym getter/setter dispatch; gym runtime cancellation, timeout, rejection and bounded no-action retry");
}
