#pragma once
#include <stddef.h>
#include <stdint.h>

// Mechanical constants from diagnostics/quick-summary.js (prepare + fixed policy
// limits). ARM64 shipped-build fingerprints; no discovery, hooks or policy logic.
namespace pgr_catch_wait_schema {
inline constexpr uint32_t kCoroutineOwner=0, kCaptureOwner=1, kResponseOwner=2, kWaitOwner=3;
struct ClassSpec { uint32_t owner; const char *name_space; const char *name; const char *canonical; };
inline constexpr ClassSpec kClasses[] = {
  {kCaptureOwner,"Niantic.Holoholo.Encounter","EncounterCaptureState","Niantic.Holoholo.Encounter.EncounterCaptureState"},
  {kResponseOwner,"Holoholo.Rpc","CatchPokemonOutProto","Holoholo.Rpc.CatchPokemonOutProto"},
  {kWaitOwner,"Niantic.Zeppelin.Scheduler","Wait","Niantic.Zeppelin.Scheduler.Wait"}
};
inline constexpr size_t kClassCount=3;
inline constexpr const char *kNestedName="ebk";
inline constexpr uint32_t kNestedDeclaringOwner=kCaptureOwner, kNestedMax=64;
// Coroutine is found by nested name + declaring owner, not guessed canonical name.
struct FieldSpec {
  const char *key; uint32_t owner; const char *name; const char *type_name;
  uint32_t exact_offset; uint32_t min_offset; uint32_t max_offset; bool offset_is_exact;
};
inline constexpr FieldSpec kFields[] = {
  {"state",kCoroutineOwner,"drny","System.Int32",0x10,16,4096,true},
  {"current",kCoroutineOwner,"drnz","Niantic.Zeppelin.Scheduler.ISchedule",0x18,16,4096,true},
  {"owner",kCoroutineOwner,"<>4__this","Niantic.Holoholo.Encounter.EncounterCaptureState",0x20,16,4096,true},
  {"response",kCoroutineOwner,"captureResults","Holoholo.Rpc.CatchPokemonOutProto",0x28,16,4096,true},
  {"branch",kCoroutineOwner,"drog","System.Boolean",0x60,16,4096,true},
  {"duration",kWaitOwner,"<SleepFor>k__BackingField","System.Single",0x10,16,4096,true},
  {"status",kResponseOwner,"Status","Holoholo.Rpc.CatchPokemonOutProto.Types.Status",0,16,4096,false}
};
inline constexpr size_t kFieldCount=7;
inline constexpr uint32_t kRejectedFieldFlags=0x50;
inline constexpr const char *kStatusEnum="Holoholo.Rpc.CatchPokemonOutProto.Types.Status";
inline constexpr const char *kStatusEnumBase="System.Int32";
inline constexpr uint32_t kStatusEnumSize=4, kStatusEnumAlignment=4;
inline constexpr const char *kSuccessLiteral="CatchSuccess";
inline constexpr uint32_t kSuccessFlagsMask=0x8050, kSuccessFlagsExpected=0x8050;
// CatchSuccess numeric value is read from the validated literal, never guessed.
inline constexpr uint8_t kMoveNextPrologue[] = {
  0xff, 0xc3, 0x02, 0xd1, 0xe8, 0x23, 0x00, 0xfd, 0xfd, 0x7b, 0x05, 0xa9,
  0xfc, 0x6f, 0x06, 0xa9, 0xfa, 0x67, 0x07, 0xa9, 0xf8, 0x5f, 0x08, 0xa9,
  0xf6, 0x57, 0x09, 0xa9, 0xf4, 0x4f, 0x0a, 0xa9
};
inline constexpr uint8_t kYieldTail[] = {
  0xa9, 0xc2, 0x44, 0xb9, 0xe8, 0x03, 0x13, 0xaa, 0x08, 0x10, 0x00, 0xbd,
  0x00, 0x8d, 0x01, 0xf8, 0x69, 0x01, 0x00, 0x34, 0x09, 0xfd, 0x4c, 0xd3,
  0x08, 0x81, 0x52, 0xd3, 0xca, 0x1f, 0x02, 0xf0, 0x4a, 0x01, 0x3f, 0x91,
  0x2b, 0x00, 0x80, 0x52, 0x48, 0x0d, 0x08, 0x8b, 0x60, 0x21, 0xc9, 0x9a,
  0x08, 0x19, 0x41, 0x91, 0x01, 0x81, 0x13, 0x91, 0x1d, 0x5b, 0x65, 0x96,
  0xe8, 0x00, 0x80, 0x52, 0x79, 0x00, 0x00, 0x14
};
inline constexpr uint8_t kYieldReturn[] = {
  0x68, 0x12, 0x00, 0xb9, 0x20, 0x00, 0x80, 0x52
};
inline constexpr uint8_t kActiveCoroutineStep[] = {
  0x00, 0x01, 0x3f, 0xd6, 0x00, 0x29, 0x20, 0x1e, 0x68, 0x42, 0x41, 0x39,
  0x60, 0x32, 0x00, 0xbd
};
inline constexpr uint8_t kSleepForPrologue[] = {
  0x00, 0x10, 0x40, 0xbd, 0xc0, 0x03, 0x5f, 0xd6
};
inline constexpr uint8_t kWaitCtorPrologue[] = {
  0xc0, 0x03, 0x5f, 0xd6
};
inline constexpr uint8_t kWaitClearPrologue[] = {
  0x1f, 0x10, 0x00, 0xb9, 0xc0, 0x03, 0x5f, 0xd6
};
inline constexpr uint8_t kDebugPrologue[] = {
  0x08, 0x10, 0x05, 0x90, 0x08, 0x45, 0x47, 0xf9, 0x48, 0x01, 0x00, 0xb4,
  0x09, 0x10, 0x05, 0x90, 0x29, 0x49, 0x47, 0xf9, 0xe9, 0x00, 0x00, 0xb4,
  0x0a, 0x04, 0x40, 0xf9, 0x1f, 0x01, 0x0a, 0xeb, 0xe8, 0x03, 0x1f, 0x2a,
  0x28, 0x01, 0x00, 0x54, 0x3f, 0x01, 0x0a, 0xeb, 0xe3, 0x00, 0x00, 0x54,
  0xa1, 0x00, 0x00, 0xb4, 0x08, 0x04, 0x40, 0xf9, 0x3f, 0x08, 0x00, 0xb9,
  0x3f, 0x08, 0x00, 0xf9, 0x28, 0x00, 0x00, 0xf9, 0x28, 0x00, 0x80, 0x52,
  0xe0, 0x03, 0x08, 0x2a, 0xc0, 0x03, 0x5f, 0xd6
};
static_assert(sizeof(kDebugPrologue)==80,"Exact debug_get_method_info ARM64 fingerprint");
struct MethodSpec {
  uint32_t owner; const char *name; const char *return_type;
  uint32_t param_count; bool is_static; bool allow_generic; bool allow_inflated;
  uint64_t rva; const uint8_t *prologue; size_t prologue_size;
};
inline constexpr MethodSpec kMethods[] = {
  {kCoroutineOwner,"MoveNext","System.Boolean",0,false,false,false,0xacf24fc,kMoveNextPrologue,sizeof(kMoveNextPrologue)},
  {kWaitOwner,"get_SleepFor","System.Single",0,false,false,false,0xe120fec,kSleepForPrologue,sizeof(kSleepForPrologue)},
  {kWaitOwner,".ctor","System.Void",0,false,false,false,0xe120fd8,kWaitCtorPrologue,sizeof(kWaitCtorPrologue)},
  {kWaitOwner,"Clear","System.Void",0,false,false,false,0xe1210a0,kWaitClearPrologue,sizeof(kWaitClearPrologue)}
};
inline constexpr size_t kMethodCount=4;
inline constexpr uint64_t kMoveNextRva=0xacf24fc, kMoveNextSpan=0x29e4;
struct CodeSite { uint64_t rva; const uint8_t *bytes; size_t size; };
inline constexpr CodeSite kYieldTailSite{0xacf4c84,kYieldTail,sizeof(kYieldTail)};
inline constexpr CodeSite kYieldReturnSite{0xacf4ea8,kYieldReturn,sizeof(kYieldReturn)};
inline constexpr CodeSite kStepSite{0xe1181b4,kActiveCoroutineStep,sizeof(kActiveCoroutineStep)};
inline constexpr const char *kDebugExport="il2cpp_debug_get_method_info";
inline constexpr uint32_t kLeaseMs=2500, kPollMs=250, kRetries=20;
inline constexpr uint32_t kAssembliesMax=256, kNameBytesMax=256;
inline constexpr uint32_t kQueueMax=128, kHitsPerSecondMax=1000;
inline constexpr double kMaxDurationSeconds=30.0, kMinimumDurationSeconds=0.1;
inline constexpr int32_t kYieldState=7, kYieldReturnValue=1;
} // namespace pgr_catch_wait_schema
