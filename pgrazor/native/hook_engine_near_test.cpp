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

int main() {
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
    std::puts("hook-engine-near: PASS");
#else
    std::puts("hook-engine-near: SKIP (ARM64 instruction fixture)");
#endif
    return 0;
}
