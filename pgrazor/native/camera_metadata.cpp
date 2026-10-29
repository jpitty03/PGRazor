#include "camera_metadata.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unistd.h>
#include <link.h>

namespace {
using namespace pgr_camera_schema;

std::mutex g_resolving;

const PgrMetaApi *metadata() {
  return static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));
}

const PgrBootApi *bootstrap() {
  return static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(1));
}

bool attached(const PgrCameraMetaConfig &c) {
  return c.owner_tid && c.owner_tid == static_cast<uint64_t>(gettid()) &&
         reinterpret_cast<void *(*)()>(c.thread_current)() != nullptr;
}

double now_ms(const PgrCameraMetaConfig &c) {
  if (c.extra && c.extra->clock_ms) return c.extra->clock_ms(c.extra->clock_ctx);
  struct timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1000000.0;
}

// camera-probe.js text(p): null throws 'Null metadata string', >= 256 throws 'Metadata string exceeds bound'.
int read_meta_string(const char *p, std::string *out) {
  if (!p) return PGR_CAM_ERR_NULL_STRING;
  size_t n = 0;
  while (n < PGR_CAM_NAME_MAX && p[n] != '\0') ++n;
  if (n == PGR_CAM_NAME_MAX) return PGR_MD_ERR_NAME_TOO_LONG;
  out->assign(p, n);
  return 0;
}

// camera-probe.js typeName(api, type): null type throws 'Null metadata type', null allocated string
// throws 'Null allocated type name', always frees non-null allocated string via il2cpp_free.
int read_type_name(const PgrCameraMetaConfig &c, const void *type, std::string *out) {
  if (!type) return PGR_CAM_ERR_NULL_TYPE;
  auto get = reinterpret_cast<char *(*)(const void *)>(c.binding.exports->type_get_name);
  auto freep = reinterpret_cast<void (*)(void *)>(c.binding.exports->mem_free);
  char *raw = get(type);
  if (!raw) return PGR_MD_ERR_NULL_TYPE_NAME;
  int rc = read_meta_string(raw, out);
  freep(raw);
  return rc;
}

int set_status_reason(PgrCameraMetaOutput *out, int32_t status, const char *fmt,
                      const char *a = nullptr, const char *b = nullptr) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_CAM_OUT_VERSION;
  out->size = sizeof(*out);
  out->status = status;
  if (a && b) std::snprintf(out->reason, sizeof(out->reason), fmt, a, b);
  else if (a) std::snprintf(out->reason, sizeof(out->reason), fmt, a);
  else std::snprintf(out->reason, sizeof(out->reason), "%s", fmt ? fmt : "");
  return status;
}

bool mandatory_meta_exports_ok(const PgrMetaExports &e) {
  // Exact legacy SIGNATURES subset carried by PgrMetaExports. Extra47-only
  // dependencies may be absent without removing passive camera capabilities.
  return e.domain_get && e.domain_get_assemblies && e.assembly_get_image && e.class_from_name &&
         e.type_get_name && e.mem_free && e.class_get_method_from_name && e.method_get_class &&
         e.method_get_name && e.method_get_flags && e.method_get_param_count && e.method_get_return_type;
}

bool all_meta_exports_ok(const PgrMetaExports &e) {
  const void *const *slots = reinterpret_cast<const void *const *>(&e);
  for (size_t i = 0; i < sizeof(PgrMetaExports) / sizeof(void *); ++i)
    if (!slots[i]) return false;
  return true;
}

const void *add_off(const void *p, uint64_t off) {
  uintptr_t a = reinterpret_cast<uintptr_t>(p);
  if (!p || a > UINTPTR_MAX - off) return nullptr;
  return reinterpret_cast<const void *>(a + off);
}

// Independent default range proof for a partial metadata export table. Same
// readable/host-exec or nonwritable ELF-PF_X rule as feature47, without binding
// invented export pointers into its immutable singleton.
int partial_segment(const void *address,uint64_t length,PgrMetaSegment *out) {
  *out={};const uintptr_t a=reinterpret_cast<uintptr_t>(address);
  if(!length||length>UINTPTR_MAX-a)return -1;
  FILE *fp=std::fopen("/proc/self/maps","re");if(!fp)return -1;
  char line[512];
  while(std::fgets(line,sizeof(line),fp)){
    uintptr_t lo=0,hi=0;char perms[8]{};
    if(std::sscanf(line,"%zx-%zx %7s",&lo,&hi,perms)!=3||hi<=lo||a<lo||a>=hi||length>hi-a)continue;
    out->found=1;out->readable=perms[0]=='r';out->writable=perms[1]=='w';out->host_exec=perms[2]=='x';
    out->seg_base=reinterpret_cast<const void *>(lo);out->seg_size=hi-lo;break;
  }
  std::fclose(fp);
  if(out->found&&!out->host_exec&&!out->writable){
    struct Proof{uintptr_t address;uint64_t length;bool executable;};Proof proof{a,length,false};
    dl_iterate_phdr([](dl_phdr_info *info,size_t,void *raw)->int{
      auto &p=*static_cast<Proof *>(raw);
      for(size_t i=0;i<info->dlpi_phnum;++i){
        const auto &ph=info->dlpi_phdr[i];const uintptr_t bias=info->dlpi_addr;
        if(ph.p_type!=PT_LOAD||!(ph.p_flags&PF_X)||!ph.p_memsz||ph.p_vaddr>UINTPTR_MAX-bias)continue;
        const uintptr_t lo=bias+ph.p_vaddr;
        if(ph.p_memsz>UINTPTR_MAX-lo||p.address<lo||p.address-lo>ph.p_memsz||p.length>ph.p_memsz-(p.address-lo))continue;
        p.executable=true;return 1;
      }return 0;
    },&proof);out->elf_exec=proof.executable?1u:0u;
  }
  return 0;
}

int check_code_span(const PgrMetaApi *md, bool md_bound, const PgrCameraMetaConfig &c,
                    const void *address, uint64_t length, const void *prologue, uint32_t prologue_len) {
  if (!address || !length || length > PGR_MD_CODE_MAX) return PGR_MD_ERR_CODE_RANGE;
  if (md_bound) {
    PgrMetaCodeQuery q{};
    q.version = 1;
    q.size = sizeof(q);
    q.address = address;
    q.length = length;
    q.prologue = prologue;
    q.prologue_len = prologue_len;
    return md->validate_code(&q);
  }
  uintptr_t a = reinterpret_cast<uintptr_t>(address);
  uintptr_t base = reinterpret_cast<uintptr_t>(c.binding.module_base);
  if ((a & 3u) != 0 || a < base || a - base > c.binding.module_size ||
      length > c.binding.module_size - (a - base))
    return PGR_MD_ERR_CODE_RANGE;
  PgrMetaSegment seg{};
  const int segment_rc=c.binding.seg_query?c.binding.seg_query(c.binding.seg_ctx,address,length,&seg):partial_segment(address,length,&seg);
  if(segment_rc!=0)return PGR_MD_ERR_SEGMENT;
  if (seg.found != 1 || seg.readable != 1 || seg.host_exec > 1 || seg.elf_exec > 1 ||
      seg.writable > 1 || seg.reserved || !(seg.host_exec || (!seg.writable && seg.elf_exec)))
    return PGR_MD_ERR_SEGMENT;
  uintptr_t sbase = reinterpret_cast<uintptr_t>(seg.seg_base);
  if (!seg.seg_size || sbase > UINTPTR_MAX - seg.seg_size || a < sbase ||
      a - sbase > seg.seg_size || length > seg.seg_size - (a - sbase))
    return PGR_MD_ERR_SEGMENT;
  if (prologue_len != 0) {
    if (!prologue || prologue_len > PGR_MD_PROLOGUE_MAX || prologue_len > length)
      return PGR_MD_ERR_PROLOGUE;
    if (std::memcmp(address, prologue, prologue_len) != 0) return PGR_MD_ERR_PROLOGUE;
  }
  return 0;
}

bool code_readable(const PgrMetaApi *md, bool md_bound, const PgrCameraMetaConfig &c,
                   const void *addr, uint64_t len) {
  return check_code_span(md, md_bound, c, addr, len, nullptr, 0) == 0;
}

bool code_words(const PgrMetaApi *md, bool md_bound, const PgrCameraMetaConfig &c,
                const void *addr, const uint32_t *words, uint32_t count) {
  if (!addr || !words || !count) return false;
  return check_code_span(md, md_bound, c, addr, static_cast<uint64_t>(count) * 4u,
                         words, count * 4u) == 0;
}

const void *branch_target(const PgrMetaApi *md, bool md_bound, const PgrCameraMetaConfig &c,
                          const void *addr) {
  if (!code_readable(md, md_bound, c, addr, 4)) return nullptr;
  uint32_t word = *reinterpret_cast<const uint32_t *>(addr);
  if ((word >> 26) != 5u) return nullptr;  // AArch64 B imm26, never BL or indirect target
  int64_t imm = static_cast<int64_t>(word & 0x03ffffffu);
  if (imm & 0x02000000LL) imm -= 0x04000000LL;
  int64_t byte_off = imm * 4;
  uintptr_t base = reinterpret_cast<uintptr_t>(addr);
  uintptr_t target = 0;
  if (byte_off >= 0) {
    uint64_t uoff = static_cast<uint64_t>(byte_off);
    if (base > UINTPTR_MAX - uoff) return nullptr;
    target = base + static_cast<uintptr_t>(uoff);
  } else {
    uint64_t uoff = static_cast<uint64_t>(-byte_off);
    if (base < uoff) return nullptr;
    target = base - static_cast<uintptr_t>(uoff);
  }
  const void *tptr = reinterpret_cast<const void *>(target);
  return code_readable(md, md_bound, c, tptr, 4) ? tptr : nullptr;
}

// pointerHandleAbi (camera-probe.js lines 80..111): read-only code fingerprint of
// il2cpp_gchandle_new / il2cpp_gchandle_get_target / il2cpp_gchandle_free.
bool pointer_handle_abi(const PgrMetaApi *md, bool md_bound, const PgrCameraMetaConfig &c) {
  const PgrCameraMetaExtra *ex = c.extra;
  if (!ex->gchandle_new || !ex->gchandle_get_target || !ex->gchandle_free) return false;
  if (!code_words(md, md_bound, c, ex->gchandle_new, kGcCreateWords, kGcCreateWordCount)) return false;
  const void *create_body = branch_target(md, md_bound, c, add_off(ex->gchandle_new, kGcCreateBranchOff));
  const void *get_body = branch_target(md, md_bound, c, ex->gchandle_get_target);
  const void *free_body = branch_target(md, md_bound, c, ex->gchandle_free);
  if (!create_body || !get_body || !free_body) return false;
  return code_words(md, md_bound, c, add_off(create_body, kGcCreateBodyOff), &kGcCreateBodyWord, 1) &&
         code_words(md, md_bound, c, get_body, &kGcGetHead, 1) &&
         code_words(md, md_bound, c, add_off(get_body, kGcGetTailOff), kGcGetTail, 2) &&
         code_words(md, md_bound, c, free_body, &kGcFreeHead, 1) &&
         code_words(md, md_bound, c, add_off(free_body, kGcFreeTailOff), kGcFreeTail, 2);
}

// Validate a continuous map-gate method (get_MainCamera, get_cullingMask, NameToLayer) preserving
// camera-probe.js evaluation order (throw on null/overlong string or type; unavailable on missing or
// signature mismatch) and also verifying through feature 47's validate_method.
int validate_map_gate_method(const PgrMetaApi *md, bool md_bound, const PgrCameraMetaConfig &c, const void *owner,
                             const char *key, const char *want_ret, uint32_t is_static,
                             uint32_t param_count, const char *param0_type,
                             PgrCameraMetaOutput *out, const void **method_out) {
  auto method_by_name = reinterpret_cast<void *(*)(const void *, const char *, int)>(
      c.binding.exports->class_get_method_from_name);
  auto get_mclass = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->method_get_class);
  auto get_mname = reinterpret_cast<const char *(*)(const void *)>(c.binding.exports->method_get_name);
  auto get_pcount = reinterpret_cast<uint32_t (*)(const void *)>(c.binding.exports->method_get_param_count);
  auto get_mflags = reinterpret_cast<uint32_t (*)(const void *, uint32_t *)>(c.binding.exports->method_get_flags);
  auto get_ret = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->method_get_return_type);
  auto is_generic = reinterpret_cast<bool (*)(const void *)>(c.binding.exports->method_is_generic);
  auto is_inflated = reinterpret_cast<bool (*)(const void *)>(c.binding.exports->method_is_inflated);
  auto get_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.binding.exports->method_get_param);

  void *m = method_by_name(owner, key, static_cast<int>(param_count));
  if (!m) return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-method-missing:%s", key);

  if (get_mclass(m) != owner)
    return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-signature-mismatch:%s", key);

  std::string mname;
  int rc = read_meta_string(get_mname(m), &mname);
  if (rc != 0) return rc;
  if (mname != key)
    return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-signature-mismatch:%s", key);

  uint32_t impl = 0;
  uint32_t flags = get_mflags(m, &impl);
  if (get_pcount(m) != param_count || ((flags & 0x10u) != 0) != (is_static != 0))
    return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-signature-mismatch:%s", key);

  std::string rname;
  rc = read_type_name(c, get_ret(m), &rname);
  if (rc != 0) return rc;
  if (rname != want_ret || is_generic(m) || is_inflated(m))
    return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-signature-mismatch:%s", key);

  if (param_count == 1) {
    std::string pname;
    rc = read_type_name(c, get_param(m, 0), &pname);
    if (rc != 0) return rc;
    if (pname != param0_type)
      return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-signature-mismatch:%s", key);
  }

  // Also run through feature 47's structural validator to confirm agreement and record telemetry.
  PgrMetaMethodQuery mq{};
  mq.version = 1;
  mq.size = sizeof(mq);
  mq.owner = owner;
  mq.name = key;
  mq.return_type = want_ret;
  mq.return_match = PGR_MD_MATCH_EXACT;
  mq.param_count = param_count;
  if (param_count == 1) {
    mq.param_types[0] = param0_type;
    mq.param_match[0] = PGR_MD_MATCH_EXACT;
  }
  mq.is_static = is_static;
  mq.allow_inflated = 0;
  PgrMetaMethodResult mr{};
  rc = md_bound?md->validate_method(&mq, &mr):0;
  if (rc != 0 || (md_bound && mr.method != m))
    return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-signature-mismatch:%s", key);

  *method_out = m;
  return 0;
}

int resolve(const PgrCameraMetaConfig *input, PgrCameraMetaOutput *out) {
  if (!out) return PGR_MD_ERR_NULL_ARG;
  std::memset(out, 0, sizeof(*out));
  if (!input || input->version != PGR_CAM_CFG_VERSION || input->size != sizeof(*input) ||
      !input->thread_current || input->sample_camera > 1u || input->continuous > 1u ||
      !input->binding.exports || input->binding.version != PGR_MD_CONFIG_VERSION ||
      input->binding.size != sizeof(PgrMetaBindConfig) || input->binding.reserved != 0)
    return PGR_MD_ERR_BAD_VERSION;
  if (!input->extra || input->extra->version != PGR_CAM_EXTRA_VERSION ||
      input->extra->size != sizeof(PgrCameraMetaExtra) || input->extra->reserved != 0 ||
      !input->extra->image_get_name)
    return PGR_CAM_ERR_EXTRA_MISSING;

  std::unique_lock<std::mutex> lock(g_resolving, std::try_to_lock);
  if (!lock.owns_lock()) return PGR_MD_ERR_BUSY;

  PgrCameraMetaConfig c = *input;
  PgrCameraMetaExtra extra_copy = *input->extra;
  PgrMetaExports exports_copy = *input->binding.exports;
  c.extra = &extra_copy;
  c.binding.exports = &exports_copy;

  const uintptr_t module_base = reinterpret_cast<uintptr_t>(c.binding.module_base);
  const uintptr_t thread_address = reinterpret_cast<uintptr_t>(c.thread_current);
  if (!module_base || !c.binding.module_size || c.binding.module_size > UINTPTR_MAX - module_base)
    return PGR_MD_ERR_CODE_RANGE;
  if (!c.binding.seg_query &&
      (thread_address < module_base || thread_address - module_base > c.binding.module_size ||
       4 > c.binding.module_size - (thread_address - module_base)))
    return PGR_MD_ERR_CODE_RANGE;
  if (!mandatory_meta_exports_ok(exports_copy)) return PGR_MD_ERR_MISSING_EXPORT;
  if (!attached(c)) return PGR_CAM_ERR_ATTACH;

  const PgrMetaApi *md = metadata();
  if (!md || md->version != 1 || md->size != sizeof(PgrMetaApi)) return PGR_CAM_ERR_METADATA;

  if (!c.binding.seg_query) {
    // Frida reports a page-rounded mapping; feature 47 reports the canonical PT_LOAD extent
    // (consistent with features 53/54/55/56).
    const void *canonical = nullptr;
    uint64_t extent = 0;
    int status = md->resolve_module("libil2cpp.so", &canonical, &extent);
    if (status) return status;
    const long page = sysconf(_SC_PAGESIZE);
    if (canonical != c.binding.module_base || !extent || extent > c.binding.module_size || page <= 0 ||
        c.binding.module_size - extent >= static_cast<uint64_t>(page))
      return PGR_MD_ERR_CODE_RANGE;
    c.binding.module_size = extent;
  }

  bool md_bound = false;
  if (all_meta_exports_ok(exports_copy)) {
    int brc = md->bind(&c.binding);
    if (brc) return brc;
    md_bound = true;
  }

  // Cross-check with feature 54 (runtime_bootstrap) pull-only snapshot if active on this module.
  const PgrBootApi *boot_api = bootstrap();
  PgrBootBinding boot{};
  bool boot_ready = false;
  if (boot_api && boot_api->version == PGR_BOOT_API_VERSION &&
      boot_api->size == sizeof(PgrBootApi) && boot_api->snapshot) {
    int brc = boot_api->snapshot(&boot);
    if (brc == PGR_BOOT_ERR_BUSY) return PGR_MD_ERR_BUSY;
    if (brc == PGR_BOOT_OK && boot.module_base == c.binding.module_base) {
      if (boot.module_size != c.binding.module_size) return PGR_CAM_ERR_BOOTSTRAP;
      boot_ready = true;
    }
  }

  auto domain_get = reinterpret_cast<void *(*)()>(exports_copy.domain_get);
  auto domain_assemblies = reinterpret_cast<void **(*)(const void *, size_t *)>(exports_copy.domain_get_assemblies);
  auto assembly_image = reinterpret_cast<void *(*)(const void *)>(exports_copy.assembly_get_image);
  auto image_name = reinterpret_cast<const char *(*)(const void *)>(c.extra->image_get_name);
  auto class_from_name = reinterpret_cast<void *(*)(const void *, const char *, const char *)>(
      exports_copy.class_from_name);
  auto method_by_name = reinterpret_cast<void *(*)(const void *, const char *, int)>(
      exports_copy.class_get_method_from_name);
  auto get_mclass = reinterpret_cast<void *(*)(const void *)>(exports_copy.method_get_class);
  auto get_mname = reinterpret_cast<const char *(*)(const void *)>(exports_copy.method_get_name);
  auto get_pcount = reinterpret_cast<uint32_t (*)(const void *)>(exports_copy.method_get_param_count);
  auto get_mflags = reinterpret_cast<uint32_t (*)(const void *, uint32_t *)>(exports_copy.method_get_flags);
  auto get_ret = reinterpret_cast<void *(*)(const void *)>(exports_copy.method_get_return_type);

  // 1) Domain and bounded assembly/image scan (LIMITS.assemblies = 128, LIMITS.scanMs = 3000).
  const void *domain = c.domain ? c.domain : domain_get();
  if (!domain) return set_status_reason(out, PGR_CAM_RETRY, "assemblies-not-ready");
  if (boot_ready && boot.domain != domain) return PGR_CAM_ERR_BOOTSTRAP;

  size_t total = 0;
  void **assemblies = domain_assemblies(domain, &total);
  if (total == 0) return set_status_reason(out, PGR_CAM_RETRY, "assemblies-not-ready");
  if (!assemblies) return PGR_CAM_ERR_NULL_ASSEMBLIES;

  const size_t limit = total > PGR_CAM_ASM_LIMIT ? PGR_CAM_ASM_LIMIT : total;
  const double deadline = now_ms(c) + static_cast<double>(PGR_CAM_SCAN_MS);
  const void *core_image = nullptr;
  const void *game_image = nullptr;
  for (size_t i = 0; i < limit; ++i) {
    if (!attached(c)) return PGR_CAM_ERR_ATTACH;
    if (now_ms(c) >= deadline) return set_status_reason(out, PGR_CAM_UNAVAILABLE, "metadata-scan-budget");
    const void *assembly = assemblies[i];
    if (!assembly) continue;
    const void *candidate = assembly_image(assembly);
    if (!candidate) continue;
    std::string iname;
    int rc = read_meta_string(image_name(candidate), &iname);
    if (rc != 0) return rc;
    if (iname == "UnityEngine.CoreModule.dll") core_image = candidate;
    if (c.continuous && iname == "holo-game.dll") game_image = candidate;
    if (core_image && (!c.continuous || game_image)) break;
  }
  if (!core_image) return set_status_reason(out, PGR_CAM_RETRY, "core-module-not-found-within-bound");
  if (c.continuous && !game_image) return set_status_reason(out, PGR_CAM_RETRY, "holo-game-not-found-within-bound");

  PgrCameraMetaOutput result{};
  result.version = PGR_CAM_OUT_VERSION;
  result.size = sizeof(result);
  result.core_image = core_image;
  result.game_image = game_image;

  // 2) CoreModule target classes and methods (Camera.get_main, Component.get_transform,
  //    Transform.get_forward, UnitySynchronizationContext.ExecuteTasks, and optional Object.GetInstanceID).
  for (uint32_t i = 0; i < kTargetCount; ++i) {
    const TargetSpec &t = kTargets[i];
    if (t.sample_only && !c.sample_camera) continue;
    if (!attached(c)) return PGR_CAM_ERR_ATTACH;

    void *klass = class_from_name(core_image, "UnityEngine", t.class_name);
    if (!klass) return set_status_reason(out, PGR_CAM_UNAVAILABLE, "missing-class:%s", t.class_name);

    void *method = method_by_name(klass, t.method_name, 0);
    if (!method) return set_status_reason(out, PGR_CAM_UNAVAILABLE, "missing-method:%s.%s", t.class_name, t.method_name);

    uint32_t impl = 0;
    uint32_t flags = get_mflags(method, &impl);
    std::string actual_type;
    int rc = read_type_name(c, get_ret(method), &actual_type);
    if (rc != 0) return rc;

    bool class_match = (get_mclass(method) == klass);
    std::string actual_name;
    if (class_match) {
      rc = read_meta_string(get_mname(method), &actual_name);
      if (rc != 0) return rc;
    }
    if (!class_match || actual_name != t.method_name || get_pcount(method) != 0 ||
        ((flags & 0x10u) != 0) != (t.is_static != 0) || actual_type != t.return_type) {
      return set_status_reason(out, PGR_CAM_UNAVAILABLE, "signature-mismatch:%s.%s", t.class_name, t.method_name);
    }

    result.classes[t.class_idx] = klass;
    result.methods[t.method_idx] = method;
    if (t.method_idx == M_EXECUTE_TASKS) {
      result.execute = method;
      if (boot_ready && boot.execute_tasks != method) return PGR_CAM_ERR_BOOTSTRAP;
    }
  }

  // 3) Preparatory Vector3 x/y/z field validation (camera-probe.js lines 160..178).
  if (!exports_copy.class_get_field_from_name || !exports_copy.field_get_type ||
      !exports_copy.field_get_parent || !exports_copy.field_get_flags) {
    result.vector_fields = PGR_CAM_VF_EXPORTS_UNAVAILABLE;
  } else {
    if (!attached(c)) return PGR_CAM_ERR_ATTACH;
    void *vector = class_from_name(core_image, "UnityEngine", "Vector3");
    result.classes[C_VECTOR3] = vector;
    if (!vector) {
      result.vector_fields = PGR_CAM_VF_CLASS_UNAVAILABLE;
    } else {
      auto field_by_name = reinterpret_cast<void *(*)(const void *, const char *)>(
          exports_copy.class_get_field_from_name);
      auto field_parent = reinterpret_cast<void *(*)(const void *)>(exports_copy.field_get_parent);
      auto field_flags = reinterpret_cast<int (*)(const void *)>(exports_copy.field_get_flags);
      auto field_type = reinterpret_cast<void *(*)(const void *)>(exports_copy.field_get_type);

      result.vector_fields = PGR_CAM_VF_VERIFIED;
      for (uint32_t f = 0; f < kFieldCount; ++f) {
        if (!attached(c)) return PGR_CAM_ERR_ATTACH;
        void *field = field_by_name(vector, kVectorFieldNames[f]);
        if (!field || field_parent(field) != vector || (static_cast<uint32_t>(field_flags(field)) & 0x10u) != 0) {
          result.vector_fields = PGR_CAM_VF_SIGNATURE_MISMATCH;
          break;
        }
        std::string ftype;
        int rc = read_type_name(c, field_type(field), &ftype);
        if (rc != 0) return rc;
        if (ftype != "System.Single") {
          result.vector_fields = PGR_CAM_VF_SIGNATURE_MISMATCH;
          break;
        }
        result.fields[f] = field;
      }
      if (result.vector_fields != PGR_CAM_VF_VERIFIED) {
        for (uint32_t f = 0; f < kFieldCount; ++f) result.fields[f] = nullptr;
      }
    }
  }

  // 4) sampleCamera verification (camera-probe.js lines 180..199).
  if (c.sample_camera) {
    if (result.vector_fields != PGR_CAM_VF_VERIFIED) {
      const char *vf_reason =
          (result.vector_fields == PGR_CAM_VF_EXPORTS_UNAVAILABLE) ? "vector-fields-exports-unavailable"
          : (result.vector_fields == PGR_CAM_VF_CLASS_UNAVAILABLE) ? "vector-fields-class-unavailable"
                                                                   : "vector-fields-signature-mismatch";
      return set_status_reason(out, PGR_CAM_UNAVAILABLE, vf_reason);
    }

    if (!attached(c)) return PGR_CAM_ERR_ATTACH;
    if (!pointer_handle_abi(md, md_bound, c))
      return set_status_reason(out, PGR_CAM_UNAVAILABLE, "gc-handle-abi-unverified");

    struct ExportGate { const void *ptr; const char *name; };
    const ExportGate sampling_exports[] = {
      {c.extra->runtime_invoke,                  "runtime_invoke"},
      {c.extra->object_get_class,                "object_get_class"},
      {c.extra->object_unbox,                    "object_unbox"},
      {c.extra->gchandle_new,                    "gchandle_new"},
      {c.extra->gchandle_get_target,             "gchandle_get_target"},
      {c.extra->gchandle_free,                   "gchandle_free"},
      {c.extra->field_get_value,                 "field_get_value"},
      {c.extra->get_corlib,                      "get_corlib"},
      {exports_copy.class_is_assignable_from,    "class_is_assignable_from"},
      {exports_copy.class_is_valuetype,          "class_is_valuetype"},
      {exports_copy.class_value_size,            "class_value_size"},
    };
    for (const auto &g : sampling_exports) {
      if (!g.ptr) return set_status_reason(out, PGR_CAM_UNAVAILABLE, "sampling-export-missing:%s", g.name);
    }

    if (!attached(c)) return PGR_CAM_ERR_ATTACH;
    auto get_corlib = reinterpret_cast<void *(*)()>(c.extra->get_corlib);
    auto is_valuetype = reinterpret_cast<bool (*)(const void *)>(exports_copy.class_is_valuetype);
    auto value_size = reinterpret_cast<int (*)(const void *, int *)>(exports_copy.class_value_size);

    void *corlib = get_corlib();
    if (!corlib) return set_status_reason(out, PGR_CAM_UNAVAILABLE, "corlib-unavailable");
    result.corlib_image = corlib;

    void *integer = class_from_name(corlib, "System", "Int32");
    int alignment = 0;
    if (!integer || !is_valuetype(integer) || value_size(integer, &alignment) != 4 ||
        !is_valuetype(result.classes[C_VECTOR3])) {
      return set_status_reason(out, PGR_CAM_UNAVAILABLE, "boxed-value-type-mismatch");
    }
    result.classes[C_INT32] = integer;

    // 5) continuous map-gate verification (camera-probe.js lines 200..256).
    if (c.continuous) {
      if (!attached(c)) return PGR_CAM_ERR_ATTACH;
      auto is_assignable = reinterpret_cast<bool (*)(const void *, const void *)>(
          exports_copy.class_is_assignable_from);

      void *rig = class_from_name(game_image, "Niantic.Holoholo.CameraRig", "MainCameraRig");
      void *stack = class_from_name(game_image, "Niantic.Holoholo.Cameras", "CameraStack");
      void *string_cls = class_from_name(corlib, "System", "String");
      void *layer_mask = class_from_name(core_image, "UnityEngine", "LayerMask");
      if (!rig || !stack || !string_cls || !layer_mask ||
          !is_assignable(stack, rig) || is_valuetype(string_cls)) {
        return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-class-mismatch");
      }
      result.classes[C_RIG] = rig;
      result.classes[C_STACK] = stack;
      result.classes[C_STRING] = string_cls;
      result.classes[C_LAYERMASK] = layer_mask;

      const ExportGate map_exports[] = {
        {exports_copy.debug_get_method_info, "debug_get_method_info"},
        {exports_copy.method_is_generic,     "method_is_generic"},
        {exports_copy.method_is_inflated,    "method_is_inflated"},
        {exports_copy.method_get_param,      "method_get_param"},
        {c.extra->string_new,                "string_new"},
      };
      for (const auto &g : map_exports) {
        if (!g.ptr) return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-export-missing:%s", g.name);
      }
      // In production (seg_query == nullptr), debug_get_method_info is both the in-module ARM64 code
      // address and the callable export. Under the synthetic segment fixture (seg_query != nullptr),
      // binding.debug_prologue (with debug_prologue_len == 0) may supply the in-module code address
      // holding the 80-byte ARM64 prologue while exports.debug_get_method_info remains a host C++ stub.
      result.debug_address =
          (c.binding.seg_query && c.binding.debug_prologue && c.binding.debug_prologue_len == 0)
              ? c.binding.debug_prologue
              : exports_copy.debug_get_method_info;


      if (!attached(c)) return PGR_CAM_ERR_ATTACH;
      if (check_code_span(md, md_bound, c, result.debug_address, kDebugPrologueLen,
                          kDebugPrologue, kDebugPrologueLen) != 0) {
        return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-debug-info-abi-unverified");
      }

      // CameraStack.get_MainCamera() -> UnityEngine.Camera at RVA 0xae09554 with 2-word prologue.
      const void *main_cam_method = nullptr;
      int mrc = validate_map_gate_method(md, md_bound, c, stack, "get_MainCamera", "UnityEngine.Camera",
                                         /*is_static*/ 0, /*param_count*/ 0, nullptr, out, &main_cam_method);
      if (mrc != 0) return mrc;

      auto debug_info = reinterpret_cast<bool (*)(const void *, void *)>(exports_copy.debug_get_method_info);
      void *info[3] = {nullptr, nullptr, nullptr};
      if (!debug_info(main_cam_method, info))
        return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-code-unavailable:get_MainCamera");

      const void *code_addr = info[0];
      if (module_base > UINTPTR_MAX - PGR_CAM_MAIN_CAMERA_RVA) return PGR_CAM_ERR_RVA_OVERFLOW;
      const void *expected_addr = reinterpret_cast<const void *>(module_base + PGR_CAM_MAIN_CAMERA_RVA);
      if (!code_addr || (reinterpret_cast<uintptr_t>(code_addr) & 3u) != 0 || code_addr != expected_addr ||
          check_code_span(md, md_bound, c, code_addr, kGetMainCameraByteLen, nullptr, 0) != 0) {
        return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-gate-code-mismatch:get_MainCamera");
      }
      if (check_code_span(md, md_bound, c, code_addr, kGetMainCameraByteLen,
                          kGetMainCameraWords, kGetMainCameraByteLen) != 0) {
        return set_status_reason(out, PGR_CAM_UNAVAILABLE, "map-camera-getter-code-unverified");
      }
      result.methods[M_GET_MAIN_CAMERA] = main_cam_method;
      result.get_main_camera_address = code_addr;

      // Camera.get_cullingMask() -> System.Int32
      if (!attached(c)) return PGR_CAM_ERR_ATTACH;
      mrc = validate_map_gate_method(md, md_bound, c, result.classes[C_CAMERA], "get_cullingMask", "System.Int32",
                                     /*is_static*/ 0, /*param_count*/ 0, nullptr,
                                     out, &result.methods[M_GET_CULLING_MASK]);
      if (mrc != 0) return mrc;

      // LayerMask.NameToLayer(System.String) -> System.Int32
      if (!attached(c)) return PGR_CAM_ERR_ATTACH;
      mrc = validate_map_gate_method(md, md_bound, c, layer_mask, "NameToLayer", "System.Int32",
                                     /*is_static*/ 1, /*param_count*/ 1, "System.String",
                                     out, &result.methods[M_NAME_TO_LAYER]);
      if (mrc != 0) return mrc;
    }
  }

  if (!attached(c)) return PGR_CAM_ERR_ATTACH;
  result.status = PGR_CAM_OK;
  result.target_count = c.sample_camera ? 5u : 4u;
  *out = result;
  return PGR_CAM_OK;
}

const PgrCameraMetaApi g_api = {
  PGR_CAM_API_VERSION,
  sizeof(PgrCameraMetaApi),
  sizeof(PgrCameraMetaConfig),
  sizeof(PgrCameraMetaOutput),
  resolve
};

}  // namespace

extern "C" const void *pgrazor_camera_metadata_api(uint32_t version) {
  return version == PGR_CAM_API_VERSION ? &g_api : nullptr;
}
