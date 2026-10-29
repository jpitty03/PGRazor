#include "hook_engine.h"
#include "dobby.h"
#include <mutex>
#if defined(__aarch64__)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
namespace { std::mutex engine_mutex; }
#if defined(__aarch64__)
namespace {
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
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long lo = 0, hi = 0;
        if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, permissions) == 3 &&
            begin >= lo && end <= hi && permissions[0] == 'r' &&
            permissions[1] != 'w' && permissions[3] == 'p') { found = true; break; }
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
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X) || (ph.p_flags & PF_W) ||
            ph.p_filesz != ph.p_memsz) continue;
        const uintptr_t begin = info->dlpi_addr + ph.p_vaddr;
        if (ph.p_memsz > UINTPTR_MAX - begin) continue;
        const uintptr_t end = begin + ph.p_memsz;
        if (end > UINTPTR_MAX - search.page_size + 1) continue;
        const uintptr_t ranges[][2] = {
            {begin & ~(search.page_size - 1), begin & ~uintptr_t(3)},
            {(end + 3) & ~uintptr_t(3), (end + search.page_size - 1) & ~(search.page_size - 1)}
        };
        for (const auto &range : ranges) {
            const uintptr_t lo = range[0], hi = range[1];
            if (hi <= lo || hi - lo < 16 || !branch_reaches(search.source, lo)) continue;
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
            if (overlap || !private_readable_range(lo, hi)) continue;
            island_pages.push_back({lo, 0, hi - lo});
            search.found = true;
            return 1;
        }
    }
    return 1;
}
void *near_island(void *address, void *replacement) {
    const uintptr_t source = reinterpret_cast<uintptr_t>(address);
    constexpr size_t slot_size = 16;
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
        if (!maps) return nullptr;
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
                allocated = mmap(reinterpret_cast<void *>(candidate), page_size,
                    PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
            // Restrict padding to at most a 4K boundary even on larger-page
            // systems, minimizing the reserved part of an executable segment.
            PaddingSearch search{source, page_size < 4096 ? page_size : 4096, false};
            dl_iterate_phdr(padding_callback, &search);
            if (!search.found) return nullptr;
        } else {
            island_pages.push_back({reinterpret_cast<uintptr_t>(allocated), 0, page_size});
        }
        chosen = &island_pages.back();
    }
    const uintptr_t entry = chosen->base + chosen->used;
    // LDR X17, #8; BR X17; absolute destination. X17 is an ABI scratch register.
    uint32_t code[4] = {0x58000051u, 0xd61f0220u, 0, 0};
    std::memcpy(code + 2, &replacement, sizeof(replacement));
    if (DobbyCodePatch(reinterpret_cast<void *>(entry),
        reinterpret_cast<uint8_t *>(code), sizeof(code)) != 0) return nullptr;
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
    if (!address || !replacement || !original || (reinterpret_cast<uintptr_t>(address) & 3)) return -1;
    enable_near_once();
    // Passing an already-near destination prevents Dobby's near plugin from
    // silently falling back to a 12/16-byte patch over adjacent tiny methods.
    void *island = near_island(address, replacement);
    if (!island) return -1;
    return DobbyHook(address, island, original);
#else
    return DobbyHook(address, replacement, original);
#endif
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
