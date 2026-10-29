#pragma once
#include <stddef.h>
#include <stdint.h>

// Feature 47 (API 1). Reusable IL2CPP metadata DISCOVERY/VALIDATION infrastructure: the compiled C++
// port of the structural reflection primitives embedded in diagnostics/quick-close.js (bind, text,
// typeName, checkBytes, executable, and prepare's class/field/method validators raw / refField /
// enumI32Field / valueField / method) and the equivalent loops in quick-ball.js and
// runtime/features/routes.js. It exists so per-feature native modules stop duplicating large
// reflection loops (overnight handoff item 3): it resolves and validates EXACT supported metadata and
// hands back the stable Il2CppClass* / FieldInfo* / MethodInfo* pointers the existing per-feature
// configs already consume (see routes_actions.h / catch_completion.h config slots).
//
// SCOPE / NOT (see .artifacts/native-migration/runtime-metadata-contract.txt for the full rationale):
//  * Pure STRUCTURAL validation. NO managed runtime invokes, NO field_static_get_value, NO hooks,
//    NO Gum/Dobby, NO GC handles, NO thread attach. The metadata pointers it returns are stable and
//    this module roots nothing. Enum-literal static VALUES and GC-handle (pointerHandleAbi)
//    fingerprints stay in the per-feature owners.
//  * NO raw/guessed addresses as trust input and NO pointers taken from a JSON/settings transport.
//    Inputs are DATA (namespace/name strings, exact offsets, flag masks, signature type-name strings,
//    an expected module-relative code address + prologue bytes). The only pointers accepted at bind
//    are the il2cpp exports (resolved natively by the host via dlopen/dlsym, or supplied as fixtures
//    by the compiled harness) and the real loaded-module base/size.
//  * NO callback into JS and NO production-readiness claim. A successful compile or fixture harness
//    run is NOT evidence that real translated-ARM64 resolution under Houdini works on the game.
//  * Internal C++ interface: the single hidden getter below is NOT added to pgrazor.map; the
//    coordinator registers it under query_feature(47) (same pattern as features 24/43).

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RuntimeMetadata native ABI requires LP64");

enum PgrMetaInfo {
  PGR_MD_FEATURE_ID     = 47,
  PGR_MD_API_VERSION    = 1,
  PGR_MD_CONFIG_VERSION = 1,
  PGR_MD_QUERY_VERSION  = 1,
  PGR_MD_SAMPLE_VERSION = 1,
  PGR_MD_NAME_MAX       = 256,   // UTF8 name byte ceiling (quick-close LIMITS.nameBytes)
  PGR_MD_ASM_MAX        = 256,   // domain assembly scan ceiling (quick-close LIMITS.assemblies)
  PGR_MD_SCAN_MAX       = 512,   // class_get_methods / class_get_nested_types iteration ceiling
  PGR_MD_PARAM_MAX      = 16,    // method parameter ceiling
  PGR_MD_CODE_MAX       = 4096,  // executable range ceiling (bytes)
  PGR_MD_PROLOGUE_MAX   = 256    // prologue compare ceiling (bytes)
};

// APIs are serialized with try-lock; concurrent/reentrant calls return BUSY without
// changing singleton/sample. Output structs are cleared even on failure/BUSY.
// Every table function returns 0 on success/VALID, or one of these PRECISE negative classifications
// (a direct 1:1 map of the throw-reasons in quick-close prepare, so a failing feature can be told
// exactly which structural check failed).
enum PgrMetaStatus {
  PGR_MD_OK                 = 0,
  PGR_MD_ERR_NULL_ARG       = -1,
  PGR_MD_ERR_BAD_VERSION    = -2,
  PGR_MD_ERR_BAD_SIZE       = -3,
  PGR_MD_ERR_NOT_BOUND      = -4,
  PGR_MD_ERR_MISSING_EXPORT = -5,   // bind(): a mandatory export pointer or module range was null
  PGR_MD_ERR_BOUND_MISMATCH = -6,   // bind(): a different config at an already-bound resolver
  PGR_MD_ERR_NAME_TOO_LONG  = -7,   // a supplied name exceeded PGR_MD_NAME_MAX (name-limit)
  PGR_MD_ERR_LIMIT          = -8,   // assembly / member / param bound exceeded
  PGR_MD_ERR_NOT_FOUND      = -9,   // class / field / method not found
  PGR_MD_ERR_NAME_MISMATCH  = -10,  // resolved member name != requested
  PGR_MD_ERR_OWNER_MISMATCH = -11,  // field_get_parent / method_get_class != owner
  PGR_MD_ERR_OFFSET         = -12,  // field offset != requested exact offset
  PGR_MD_ERR_FLAGS          = -13,  // forbidden flag bits set / required flag bits clear
  PGR_MD_ERR_KIND           = -14,  // field kind (ref/value/enum-int32) not satisfied
  PGR_MD_ERR_SIZE           = -15,  // value-type size not the required / accepted size
  PGR_MD_ERR_TYPE           = -16,  // field / return / param type name did not match
  PGR_MD_ERR_STATIC         = -17,  // static/instance flag != requested
  PGR_MD_ERR_GENERIC        = -18,  // method is generic (always rejected)
  PGR_MD_ERR_INFLATED       = -19,  // method is inflated and allow_inflated was 0
  PGR_MD_ERR_PARAM_COUNT    = -20,  // method_get_param_count != requested
  PGR_MD_ERR_CODE_RANGE     = -22,  // code address outside module / misaligned / addr != base+rva
  PGR_MD_ERR_SEGMENT        = -23,  // not a readable executable (host or ELF-PF_X guest) segment
  PGR_MD_ERR_PROLOGUE       = -24,  // prologue bytes did not match (unsupported-instructions)
  PGR_MD_ERR_DUPLICATE      = -25,  // class resolved to different handles across images
  PGR_MD_ERR_BUSY           = -27,  // concurrent/reentrant API call; shared state untouched
  PGR_MD_ERR_NULL_TYPE_NAME = -26   // type_get_name returned null
};

// Field KIND selector (validate_field). ANY = offset/flags only (quick-close raw()); REF = reference
// field (refField); VALUE = value-type field (valueField); ENUM_I32 = Int32-backed enum (enumI32Field).
enum PgrMetaFieldKind {
  PGR_MD_FIELD_ANY      = 0,
  PGR_MD_FIELD_REF      = 1,
  PGR_MD_FIELD_VALUE    = 2,
  PGR_MD_FIELD_ENUM_I32 = 3
};

// Type-name match mode for field/return/param type strings. EXACT is the quick-close default; the
// others cover the regex predicates quick-close uses for generic/inflated collection types. Full
// regex predicates remain the caller's responsibility (see contract note 1).
enum PgrMetaMatch {
  PGR_MD_MATCH_ANY      = 0,  // type string null -> skip the check
  PGR_MD_MATCH_EXACT    = 1,
  PGR_MD_MATCH_PREFIX   = 2,
  PGR_MD_MATCH_SUFFIX   = 3,
  PGR_MD_MATCH_CONTAINS = 4
};

// Result of a segment lookup for [addr,addr+size). found==1 only when a single mapping/segment
// contains the whole range. readable/host_exec from the host mapping; elf_exec when an ELF PT_LOAD
// with PF_X covers the range (translated ARM64 guest code the host mapped read-only under Houdini).
// Flags must be0/1; ELF fallback additionally requires writable==0. Unreleased API1
// appends writable/reserved to preserve prior field offsets; rebuild every caller.
struct PgrMetaSegment {
  uint32_t found;
  uint32_t readable;
  uint32_t host_exec;
  uint32_t elf_exec;
  const void *seg_base;
  uint64_t seg_size;
  uint32_t writable; // host mapping writable: cannot authorize non-exec guest fallback
  uint32_t reserved; // zero
};
static_assert(sizeof(PgrMetaSegment) == 40, "PgrMetaSegment is 40 bytes on LP64 (unreleased API1)");

// Pluggable segment resolver. ctx is BindConfig.seg_ctx. Returns 0 if the lookup was performed
// (out.found says whether the range was contained), negative on a resolver fault. Injected as a
// native C++ function pointer only; never a value crossing JSON.
typedef int (*pgr_meta_seg_fn)(void *ctx, const void *addr, uint64_t size, PgrMetaSegment *out);

// The 31 mandatory il2cpp exports, in fixed order, stored as void* and reinterpret_cast in the impl
// (matching the routes_actions/catch_completion config style). Only metadata-discovery exports; NO
// runtime_invoke / field_static_get_value / gchandle_* here by design. Resolved by the host via
// dlopen/dlsym on the real libil2cpp, or supplied as fixtures by the compiled harness.
struct PgrMetaExports {
  void *domain_get;                 //  0  void* (void)
  void *domain_get_assemblies;      //  1  void* (void* domain, size_t* count)
  void *assembly_get_image;         //  2  void* (void* assembly)
  void *class_from_name;            //  3  void* (void* image, const char* ns, const char* name)
  void *class_get_type;             //  4  void* (void* klass)
  void *type_get_name;              //  5  char* (void* type)  [caller frees via il2cpp_free]
  void *mem_free;                   //  6  void  (void* ptr)   [il2cpp_free]
  void *class_get_field_from_name;  //  7  void* (void* klass, const char* name)
  void *field_get_parent;           //  8  void* (void* field)
  void *field_get_flags;            //  9  int   (void* field)
  void *field_get_offset;           // 10  size_t(void* field)
  void *field_get_type;             // 11  void* (void* field)
  void *class_from_type;            // 12  void* (void* type)
  void *class_is_valuetype;         // 13  bool  (void* klass)
  void *class_is_enum;              // 14  bool  (void* klass)
  void *class_enum_basetype;        // 15  void* (void* klass)
  void *class_value_size;           // 16  int   (void* klass, int* align)
  void *class_get_method_from_name; // 17  void* (void* klass, const char* name, int argc)
  void *method_get_class;           // 18  void* (void* method)
  void *method_get_name;            // 19  char* (void* method)  [interned; NOT freed]
  void *method_get_flags;           // 20  uint32(void* method, uint32* iflags)
  void *method_get_param_count;     // 21  uint32(void* method)
  void *method_get_param;           // 22  void* (void* method, uint32 index)  [returns type]
  void *method_get_return_type;     // 23  void* (void* method)
  void *method_is_generic;          // 24  bool  (void* method)
  void *method_is_inflated;         // 25  bool  (void* method)
  void *debug_get_method_info;      // 26  bool  (void* method, void* info)  [info[0] = code addr]
  void *class_is_assignable_from;   // 27  bool  (void* klass, void* other)
  void *class_get_methods;          // 28  void* (void* klass, void** iter)
  void *class_get_nested_types;     // 29  void* (void* klass, void** iter)
  void *class_get_declaring_type;   // 30  void* (void* klass)
};
static_assert(sizeof(PgrMetaExports) == 248, "PgrMetaExports must remain 31 LP64 pointers");

// bind() configuration. exports + the REAL loaded-module range are mandatory. seg_query null uses the
// default /proc/self/maps + dl_iterate_phdr resolver; debug_prologue_len 0 skips
// checking that debug_get_method_info is executable and matches the supplied bytes
// on this call. Optional prologue proof is revalidated on every bind, not part of
// immutable identity (exports/module/segment resolver). A stronger later proof
// may coexist with bootstrap's zero-length proof; bad proof never changes binding.
struct PgrMetaBindConfig {
  uint32_t version;              // == PGR_MD_CONFIG_VERSION
  uint32_t size;                 // == sizeof(PgrMetaBindConfig)
  const PgrMetaExports *exports; // mandatory; every listed export must be non-null
  const void *module_base;       // real il2cpp module mapping base (not a guessed address)
  uint64_t module_size;          // module mapping size (> 0)
  pgr_meta_seg_fn seg_query;     // null -> default resolver
  void *seg_ctx;                 // passed to seg_query
  const void *debug_prologue;    // optional expected bytes of the debug_get_method_info export
  uint32_t debug_prologue_len;   // 0 -> skip the debug_get_method_info ABI check
  uint32_t reserved;             // 0
};
static_assert(sizeof(PgrMetaBindConfig) == 64, "PgrMetaBindConfig must remain 64 bytes on LP64");

struct PgrMetaClassQuery {
  uint32_t version;         // == PGR_MD_QUERY_VERSION
  uint32_t size;            // == sizeof(PgrMetaClassQuery)
  const char *name_space;   // namespace (may be ""), e.g. "Niantic.Holoholo.NUI"
  const char *name;         // short class name, e.g. "GuiController"
  const char *canonical;    // optional full type_get_name expectation; null -> ns.name EXACT
  uint32_t canonical_match; // PgrMetaMatch for canonical (0 -> EXACT when canonical non-null)
  uint32_t reserved;
};
static_assert(sizeof(PgrMetaClassQuery) == 40, "PgrMetaClassQuery must remain 40 bytes on LP64");

struct PgrMetaClassResult {
  uint32_t version;    // == PGR_MD_QUERY_VERSION
  uint32_t size;       // == sizeof(PgrMetaClassResult)
  const void *klass;   // validated Il2CppClass*
  const void *type;    // class_get_type(klass)
};
static_assert(sizeof(PgrMetaClassResult) == 24, "PgrMetaClassResult must remain 24 bytes on LP64");

struct PgrMetaFieldQuery {
  uint32_t version;       // == PGR_MD_QUERY_VERSION
  uint32_t size;          // == sizeof(PgrMetaFieldQuery)
  const void *owner;      // Il2CppClass* (from resolve_class); field_get_parent must equal this
  const char *name;       // field name
  uint64_t offset;        // required exact field_get_offset
  uint32_t forbidden_mask;// flag bits that must be CLEAR (quick-close raw(): 0x50; literal paths vary)
  uint32_t required_mask; // flag bits that must be SET (quick-close enum-literal: 0x8050)
  uint32_t kind;          // PgrMetaFieldKind
  uint32_t value_size;    // VALUE kind: required size; 0 -> accept 4 or 8 (quick-close valueField)
  const char *type_name;  // optional field type name; null -> skip
  uint32_t type_match;    // PgrMetaMatch for type_name
  uint32_t reserved;
};
static_assert(sizeof(PgrMetaFieldQuery) == 64, "PgrMetaFieldQuery must remain 64 bytes on LP64");

struct PgrMetaFieldResult {
  uint32_t version;        // == PGR_MD_QUERY_VERSION
  uint32_t size;           // == sizeof(PgrMetaFieldResult)
  const void *field;       // validated FieldInfo*
  const void *type;        // field_get_type(field)
  const void *type_class;  // class_from_type(type) (null for some encodings)
  uint64_t offset;         // field_get_offset(field)
  uint32_t flags;          // field_get_flags(field)
  uint32_t value_size;     // value-type size when KIND=VALUE/ENUM_I32, else 0
};
static_assert(sizeof(PgrMetaFieldResult) == 48, "PgrMetaFieldResult must remain 48 bytes on LP64");

struct PgrMetaMethodQuery {
  uint32_t version;       // == PGR_MD_QUERY_VERSION
  uint32_t size;          // == sizeof(PgrMetaMethodQuery)
  const void *owner;      // Il2CppClass*; method_get_class must equal this
  const char *name;       // method name (also exact-compared against method_get_name)
  const char *return_type;// optional return type name; null -> skip
  uint32_t return_match;  // PgrMetaMatch for return_type
  uint32_t param_count;   // expected param count (passed to class_get_method_from_name)
  const char *param_types[PGR_MD_PARAM_MAX]; // per-param type name; entry null -> skip that param
  uint32_t param_match[PGR_MD_PARAM_MAX];    // PgrMetaMatch per param
  uint32_t is_static;     // required static(1)/instance(0)
  uint32_t allow_inflated;// 0 reject inflated; generic is ALWAYS rejected
  const void *expected_code; // optional expected compiled address (module_base+rva); null -> skip
  const void *prologue;   // prologue bytes compared at the code address (required iff expected_code)
  uint32_t prologue_len;  // prologue length (1..PGR_MD_PROLOGUE_MAX; also the executable range)
  uint32_t reserved;
};
static_assert(sizeof(PgrMetaMethodQuery) == 264, "PgrMetaMethodQuery must remain 264 bytes on LP64");

struct PgrMetaMethodResult {
  uint32_t version;    // == PGR_MD_QUERY_VERSION
  uint32_t size;       // == sizeof(PgrMetaMethodResult)
  const void *method;  // validated MethodInfo*
  const void *code;    // compiled address (debug_get_method_info) when expected_code was supplied
  uint32_t flags;      // method_get_flags
  uint32_t iflags;     // implementation flags out-param
  uint32_t param_count;
  uint32_t reserved;
};
static_assert(sizeof(PgrMetaMethodResult) == 40, "PgrMetaMethodResult must remain 40 bytes on LP64");

// Standalone executable-range + prologue check (the quick-close executable()+checkBytes port). Used
// on its own for code whose MethodInfo is not involved (e.g. a resolved export address).
struct PgrMetaCodeQuery {
  uint32_t version;     // == PGR_MD_QUERY_VERSION
  uint32_t size;        // == sizeof(PgrMetaCodeQuery)
  const void *address;  // code address (must be in-module, 4-byte aligned)
  uint64_t length;      // executable range length (1..PGR_MD_CODE_MAX)
  const void *prologue; // optional prologue bytes; null -> skip
  uint32_t prologue_len;// 0 -> skip; else 1..min(length,PGR_MD_PROLOGUE_MAX)
  uint32_t reserved;
};
static_assert(sizeof(PgrMetaCodeQuery) == 40, "PgrMetaCodeQuery must remain 40 bytes on LP64");

struct PgrMetaSample {
  uint32_t version;           // == PGR_MD_SAMPLE_VERSION
  uint32_t size;              // == sizeof(PgrMetaSample)
  uint64_t classes_resolved;  // successful resolve_class
  uint64_t fields_validated;  // successful validate_field
  uint64_t methods_validated; // successful validate_method
  uint64_t code_checks;       // successful validate_code (standalone + method-embedded)
  uint64_t failures;          // total negative returns across all primitives
  uint32_t bound;             // 1 after a successful bind
  uint32_t reserved;
};
static_assert(sizeof(PgrMetaSample) == 56, "PgrMetaSample must remain 56 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrMetaApi {
  uint32_t version;  // == PGR_MD_API_VERSION
  uint32_t size;     // == sizeof(PgrMetaApi)
  int (*bind)(const PgrMetaBindConfig *config);
  int (*resolve_class)(const PgrMetaClassQuery *query, PgrMetaClassResult *out);
  int (*validate_field)(const PgrMetaFieldQuery *query, PgrMetaFieldResult *out);
  int (*validate_method)(const PgrMetaMethodQuery *query, PgrMetaMethodResult *out);
  int (*validate_code)(const PgrMetaCodeQuery *query);
  int (*resolve_module)(const char *soname, const void **base, uint64_t *size);
  int (*sample)(PgrMetaSample *out);
};
static_assert(sizeof(PgrMetaApi) == 64, "PgrMetaApi must remain a 64-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 47 and answers only on an exact version match.
const void *pgrazor_runtime_metadata_api(uint32_t version);

#ifdef __cplusplus
}
#endif
