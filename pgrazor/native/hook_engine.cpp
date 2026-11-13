#include "hook_engine.h"
#include "dobby.h"
#include <mutex>
#include <cerrno>
#include <android/log.h>
#if defined(__aarch64__)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
namespace {
std::mutex engine_mutex;
// Guarded by engine_mutex. The failure fields latch the first failed hook.
PgrHookDiag hook_diag{1, sizeof(PgrHookDiag), 0, 0, 0, 0, 0, 0, 0, 0, 0};
void record_failure(void *address, int32_t reason, int32_t rc, int32_t error,
                    uint32_t gaps, uint32_t padding) {
    if (hook_diag.failures++) return;
    hook_diag.reason = reason; hook_diag.rc = rc; hook_diag.error = error;
    hook_diag.gaps = gaps; hook_diag.padding = padding;
    hook_diag.target = reinterpret_cast<uintptr_t>(address);
}
}
#if defined(__aarch64__)
namespace {
// What the current near_island() call tried; copied into hook_diag on failure.
struct IslandAttempt { uint32_t gaps; uint32_t padding; int32_t error; };
IslandAttempt attempt{};
struct IslandPage { uintptr_t base; size_t used; size_t size; };
std::vector<IslandPage> island_pages;
struct IslandSlot { uintptr_t entry; void *replacement; };
std::vector<IslandSlot> island_slots;
bool branch_reaches(uintptr_t from, uintptr_t to) {
    return !(to & 3) && (to >= from ? to - from < (uintptr_t(1) << 27)
                                         : from - to < (uintptr_t(1) << 27));
}
void enable_near_once() {
    static bool enabled = false;
    if (!enabled) { dobby_enable_near_branch_trampoline(); enabled = true; }
}
// Translated processes may reserve the entire guest address space in host maps.
// In that case use only ELF segment alignment padding, never arbitrary zeroes
// inside code/data. These virtual bytes are outside every formal LOAD segment;
// their file-offset aliases remain unchanged through private-mapping COW.
bool private_readable_range(uintptr_t begin, uintptr_t end) {
    FILE *maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[1024], permissions[5];
    bool found = false;
    uintptr_t cursor = begin;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long lo = 0, hi = 0;
        if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, permissions) != 3) continue;
        if (hi <= cursor) continue;
        if (lo > cursor || permissions[0] != 'r' || permissions[1] == 'w' ||
            permissions[3] != 'p') break;
        cursor = hi < end ? hi : end;
        if (cursor == end) { found = true; break; }
    }
    std::fclose(maps);
    return found;
}
struct PaddingSearch { uintptr_t source; uintptr_t page_size; bool found; };
int padding_callback(dl_phdr_info *info, size_t, void *opaque) {
    auto &search = *static_cast<PaddingSearch *>(opaque);
    bool owns_source = false;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
        const uintptr_t begin = info->dlpi_addr + ph.p_vaddr;
        if (begin <= search.source && search.source - begin < ph.p_memsz) owns_source = true;
    }
    if (!owns_source) return 0;
    attempt.padding |= PGR_HOOK_PAD_OWNER;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X) || (ph.p_flags & PF_W) ||
            ph.p_filesz != ph.p_memsz) continue;
        const uintptr_t begin = info->dlpi_addr + ph.p_vaddr;
        if (ph.p_memsz > UINTPTR_MAX - begin) continue;
        const uintptr_t end = begin + ph.p_memsz;
        // Honor the ELF's alignment (bounded to 16K), including on 4K hosts.
        // New game builds can leave too little padding inside the last 4K page.
        // All candidate bytes still require private read-only mappings and must
        // lie outside every LOAD segment; never claim code/data or unmapped gaps.
        //
        // On a 4K host the ELF-aligned range can start in the gap the linker
        // leaves between segments, which is not readable, so the whole range is
        // refused. When that happens retry with the 4K-aligned range, which lies
        // inside the segment's own first/last mapped page. This is the range
        // every build before the ELF-alignment change used.
        uintptr_t alignments[2] = {search.page_size, 0};
        size_t alignment_count = 1;
        if (ph.p_align && !(ph.p_align & (ph.p_align - 1))) {
            const uintptr_t elf_alignment = ph.p_align < 16384 ? ph.p_align : 16384;
            if (elf_alignment > search.page_size) {
                alignments[0] = elf_alignment;
                alignments[1] = search.page_size;
                alignment_count = 2;
            }
        }
        for (size_t a = 0; a < alignment_count; ++a) {
            const uintptr_t alignment = alignments[a];
            const bool page_aligned = alignment == search.page_size;
            if (a) attempt.padding |= PGR_HOOK_PAD_PAGE_RETRY;
            if (end > UINTPTR_MAX - alignment + 1) continue;
            const PgrPaddingRanges padding = pgr_padding_ranges(begin, end, alignment);
            const uintptr_t ranges[][2] = {
                {padding.before_lo, padding.before_hi}, {padding.after_lo, padding.after_hi}
            };
            for (const auto &range : ranges) {
                const uintptr_t lo = range[0], hi = range[1];
                if (hi <= lo || hi - lo < 16 || !branch_reaches(search.source, lo)) continue;
                attempt.padding |= PGR_HOOK_PAD_CANDIDATE;
                bool overlap = false;
                for (size_t j = 0; j < info->dlpi_phnum; ++j) {
                    const auto &other = info->dlpi_phdr[j];
                    if (other.p_type != PT_LOAD || !other.p_memsz) continue;
                    const uintptr_t other_begin = info->dlpi_addr + other.p_vaddr;
                    if (other.p_memsz > UINTPTR_MAX - other_begin ||
                        (lo < other_begin + other.p_memsz && other_begin < hi)) overlap = true;
                }
                for (const auto &used : island_pages)
                    if (lo < used.base + used.size && used.base < hi) overlap = true;
                if (overlap) { attempt.padding |= PGR_HOOK_PAD_OVERLAP; continue; }
                if (!private_readable_range(lo, hi)) {
                    attempt.padding |= PGR_HOOK_PAD_NOT_READABLE;
                    continue;
                }
                island_pages.push_back({lo, 0, hi - lo});
                hook_diag.island_source = page_aligned ? PGR_HOOK_ISLAND_PAGE_PADDING
                                                       : PGR_HOOK_ISLAND_ELF_PADDING;
                search.found = true;
                return 1;
            }
        }
    }
    return 1;
}
void *near_island(void *address, void *replacement) {
    const uintptr_t source = reinterpret_cast<uintptr_t>(address);
    constexpr size_t slot_size = 16;
    attempt = {};
    // Islands are immutable once published and can serve later reinstalls or
    // multiple entrypoints sharing the same replacement without consuming more
    // finite ELF padding. Keep them alive for in-flight calls after unhooking.
    for (const auto &slot : island_slots)
        if (slot.replacement == replacement && branch_reaches(source, slot.entry))
            return reinterpret_cast<void *>(slot.entry);
    IslandPage *chosen = nullptr;
    for (auto &page : island_pages) {
        if (page.size - page.used >= slot_size && branch_reaches(source, page.base + page.used)) {
            chosen = &page;
            break;
        }
    }
    if (!chosen) {
        const long page_result = sysconf(_SC_PAGESIZE);
        if (page_result <= 0) return nullptr;
        const uintptr_t page_size = static_cast<uintptr_t>(page_result);
        const uintptr_t reach = uintptr_t(1) << 27;
        const uintptr_t low = source > reach ? source - reach : page_size;
        const uintptr_t high = source <= UINTPTR_MAX - reach ? source + reach : UINTPTR_MAX;
        FILE *maps = std::fopen("/proc/self/maps", "r");
        if (!maps) {
            attempt.error = errno;
            attempt.padding |= PGR_HOOK_PAD_MAPS_FAILED;
            return nullptr;
        }
        char line[1024];
        uintptr_t previous_end = low;
        void *allocated = MAP_FAILED;
        while (std::fgets(line, sizeof(line), maps)) {
            unsigned long long begin_value = 0, end_value = 0;
            if (std::sscanf(line, "%llx-%llx", &begin_value, &end_value) != 2) continue;
            const uintptr_t begin = static_cast<uintptr_t>(begin_value);
            const uintptr_t end = static_cast<uintptr_t>(end_value);
            if (end <= low) continue;
            const uintptr_t candidate = (previous_end + page_size - 1) & ~(page_size - 1);
            const uintptr_t gap_end = begin < high ? begin : high;
            if (candidate < gap_end && gap_end - candidate >= page_size &&
                branch_reaches(source, candidate)) {
                // A hint never replaces a live mapping if another thread takes
                // the gap. Accept only addresses that still satisfy B's range.
                ++attempt.gaps;
                allocated = mmap(reinterpret_cast<void *>(candidate), page_size,
                    PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (allocated == MAP_FAILED) attempt.error = errno;
                if (allocated != MAP_FAILED) {
                    if (branch_reaches(source, reinterpret_cast<uintptr_t>(allocated))) break;
                    munmap(allocated, page_size);
                    allocated = MAP_FAILED;
                }
            }
            if (end > previous_end) previous_end = end;
            if (previous_end >= high) break;
        }
        std::fclose(maps);
        if (allocated == MAP_FAILED) {
            // Start with host-page alignment; the callback also considers the
            // owning executable segment's bounded ELF alignment.
            PaddingSearch search{source, page_size < 4096 ? page_size : 4096, false};
            attempt.padding |= PGR_HOOK_PAD_SEARCHED;
            dl_iterate_phdr(padding_callback, &search);
            if (!search.found) return nullptr;
        } else {
            island_pages.push_back({reinterpret_cast<uintptr_t>(allocated), 0, page_size});
            hook_diag.island_source = PGR_HOOK_ISLAND_MMAP;
        }
        hook_diag.island_pages = static_cast<uint32_t>(island_pages.size());
        chosen = &island_pages.back();
    }
    const uintptr_t entry = chosen->base + chosen->used;
    // LDR X17, #8; BR X17; absolute destination. X17 is an ABI scratch register.
    uint32_t code[4] = {0x58000051u, 0xd61f0220u, 0, 0};
    std::memcpy(code + 2, &replacement, sizeof(replacement));
    if (DobbyCodePatch(reinterpret_cast<void *>(entry),
        reinterpret_cast<uint8_t *>(code), sizeof(code)) != 0) {
        attempt.error = errno;
        attempt.padding |= PGR_HOOK_PAD_WRITE_FAILED;
        return nullptr;
    }
    chosen->used += slot_size;
    island_slots.push_back({entry, replacement});
    return reinterpret_cast<void *>(entry);
}
}
#endif
int pgrazor_hook(void *address, void *replacement, void **original) {
    std::lock_guard<std::mutex> guard(engine_mutex);
#if defined(__aarch64__)
    // Compiling the plugin does not activate it. Prefer a single instruction
    // branch so adjacent small game methods retain their entry instructions.
    // Scheduler export veneers are also resolved before reaching this engine.
    if (!address || !replacement || !original || (reinterpret_cast<uintptr_t>(address) & 3)) {
        record_failure(address, PGR_HOOK_REASON_BAD_ARGS, 0, 0, 0, 0);
        return -1;
    }
    enable_near_once();
    // Passing an already-near destination prevents Dobby's near plugin from
    // silently falling back to a 12/16-byte patch over adjacent tiny methods.
    void *island = near_island(address, replacement);
    if (!island) {
        record_failure(address, PGR_HOOK_REASON_NEAR_ISLAND, 0, attempt.error,
            attempt.gaps, attempt.padding);
        __android_log_print(ANDROID_LOG_WARN, "PGRazor",
            "hook-install-failed target=%p reason=near-island gaps=%u padding=%u errno=%d",
            address, attempt.gaps, attempt.padding, attempt.error);
        return -1;
    }
    errno = 0;
    const int rc = DobbyHook(address, island, original);
    if (rc != 0 || !*original) {
        const int error = errno;
        record_failure(address, PGR_HOOK_REASON_DOBBY, rc, error, attempt.gaps, attempt.padding);
        __android_log_print(ANDROID_LOG_WARN, "PGRazor",
            "hook-install-failed target=%p reason=dobby rc=%d errno=%d", address, rc, error);
    }
    return rc;
#else
    errno = 0;
    const int rc = DobbyHook(address, replacement, original);
    if (rc != 0) record_failure(address, PGR_HOOK_REASON_DOBBY, rc, errno, 0, 0);
    return rc;
#endif
}
int pgrazor_hook_diag(PgrHookDiag *out) {
    if (!out) return -1;
    std::lock_guard<std::mutex> guard(engine_mutex);
    *out = hook_diag;
    return 0;
}
int pgrazor_unhook(void *address) {
    std::lock_guard<std::mutex> guard(engine_mutex);
    return DobbyDestroy(address);
}
int pgrazor_instrument(void *address, dobby_instrument_callback_t callback) {
    std::lock_guard<std::mutex> guard(engine_mutex);
#if defined(__aarch64__)
    enable_near_once();
#endif
    return DobbyInstrument(address, callback);
}
