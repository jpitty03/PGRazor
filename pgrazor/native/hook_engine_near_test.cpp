#include "hook_engine.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

static void require(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "hook-engine-near: %s\n", what); std::abort(); }
}
static uintptr_t replacement(uintptr_t) { return 99; }

// Padding arithmetic for the 0.435.2 libil2cpp executable segment (RVA
// 0x45fd3a0..0xe296c10, 16K ELF alignment). The ELF-aligned range starts one
// 4K page before the segment's first mapped page; the 4K-aligned retry starts
// on that page and keeps 928 bytes (58 islands) in front of the code.
namespace padding_check {
constexpr uintptr_t base = 0x7000000000, begin = base + 0x45fd3a0, end = base + 0xe296c10;
constexpr PgrPaddingRanges wide = pgr_padding_ranges(begin, end, 16384);
constexpr PgrPaddingRanges narrow = pgr_padding_ranges(begin, end, 4096);
static_assert(wide.before_lo == base + 0x45fc000 && wide.before_hi == begin, "ELF-aligned lead");
static_assert(narrow.before_lo == base + 0x45fd000 && narrow.before_hi == begin, "4K-aligned lead");
static_assert(narrow.before_hi - narrow.before_lo == 928, "4K lead holds 58 islands");
static_assert(narrow.before_lo > wide.before_lo && narrow.after_hi <= wide.after_hi,
    "4K ranges lie inside the ELF-aligned ranges");
static_assert(narrow.after_lo == end && narrow.after_hi == base + 0xe297000, "4K-aligned tail");
}

int main() {
    PgrHookDiag diag{};
    require(pgrazor_hook_diag(nullptr) == -1 && pgrazor_hook_diag(&diag) == 0 &&
        diag.version == 1 && diag.size == sizeof(diag) && diag.failures == 0 &&
        diag.reason == PGR_HOOK_REASON_NONE && diag.island_source == PGR_HOOK_ISLAND_NONE,
        "clean diagnostic before any hook");
#if defined(__aarch64__)
    using Method = uintptr_t (*)(uintptr_t);
    const long page = sysconf(_SC_PAGESIZE);
    require(page > 0, "page size");
    auto *code = static_cast<uint32_t *>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(code != MAP_FAILED, "map fixture");
    // RET (4 bytes); MOV X0,#22/RET; LDR X0,[X0,#24]/RET (8 bytes);
    // MOV X0,#33/RET. Every neighboring method begins immediately after RET.
    const uint32_t pristine[] = {0xd65f03c0, 0xd28002c0, 0xd65f03c0,
        0xf9400c00, 0xd65f03c0, 0xd2800420, 0xd65f03c0};
    std::memcpy(code, pristine, sizeof(pristine));
    __builtin___clear_cache(reinterpret_cast<char *>(code),
        reinterpret_cast<char *>(code) + sizeof(pristine));
    require(mprotect(code, page, PROT_READ | PROT_EXEC) == 0, "fixture RX");
    Method volatile tiny = reinterpret_cast<Method>(code);
    Method volatile neighbor = reinterpret_cast<Method>(code + 1);
    Method volatile getter = reinterpret_cast<Method>(code + 3);
    Method volatile after_getter = reinterpret_cast<Method>(code + 5);
    uintptr_t object[] = {0, 0, 0, 42};
    require(tiny(17) == 17 && neighbor(0) == 22 &&
        getter(reinterpret_cast<uintptr_t>(object)) == 42 && after_getter(0) == 33,
        "pristine fixture behavior");
    void *tiny_original = nullptr, *getter_original = nullptr;
    require(pgrazor_hook(code, reinterpret_cast<void *>(replacement), &tiny_original) == 0,
        "hook four-byte method");
    require((code[0] & 0xfc000000u) == 0x14000000u, "single B patch");
    require(std::memcmp(code + 1, pristine + 1, 6 * sizeof(uint32_t)) == 0,
        "all following bytes preserved");
    require(tiny(17) == 99 && reinterpret_cast<Method>(tiny_original)(17) == 17 &&
        neighbor(0) == 22, "tiny replacement and original");
    require(pgrazor_hook(code + 3, reinterpret_cast<void *>(replacement), &getter_original) == 0,
        "hook eight-byte getter");
    require((code[3] & 0xfc000000u) == 0x14000000u && code[4] == pristine[4] &&
        code[5] == pristine[5] && code[6] == pristine[6], "getter neighbors preserved");
    require(getter(reinterpret_cast<uintptr_t>(object)) == 99 &&
        reinterpret_cast<Method>(getter_original)(reinterpret_cast<uintptr_t>(object)) == 42 &&
        after_getter(0) == 33, "getter replacement and original");
    require(pgrazor_unhook(code + 3) == 0 && pgrazor_unhook(code) == 0, "unhook both");
    require(std::memcmp(code, pristine, sizeof(pristine)) == 0, "restore original bytes");
    require(munmap(code, page) == 0, "unmap fixture");
    require(pgrazor_hook_diag(&diag) == 0 && diag.failures == 0 &&
        diag.island_source == PGR_HOOK_ISLAND_MMAP && diag.island_pages == 1,
        "successful hooks report an mmap island and no failure");
    void *unused = nullptr;
    require(pgrazor_hook(nullptr, reinterpret_cast<void *>(replacement), &unused) == -1 &&
        pgrazor_hook_diag(&diag) == 0 && diag.failures == 1 &&
        diag.reason == PGR_HOOK_REASON_BAD_ARGS && diag.target == 0,
        "first failure is latched");
    std::puts("hook-engine-near: PASS");
#else
    std::puts("hook-engine-near: SKIP (ARM64 instruction fixture)");
#endif
    return 0;
}
