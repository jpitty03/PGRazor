// Standalone process only: simulate a second engine replacing our ARM64
// 12-byte entry jump with a 16-byte absolute jump. Never loaded into the game.
#include <dobby.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

extern "C" int pgr_chain_fixture(int);
asm(".text\n.balign 16\n.global pgr_chain_fixture\n"
    ".type pgr_chain_fixture, %function\npgr_chain_fixture:\n"
    "add w0, w0, #1\nadd w0, w0, #1\n"
    "add w0, w0, #1\nadd w0, w0, #1\nret\n"
    ".size pgr_chain_fixture, .-pgr_chain_fixture\n");

static int (*backup)(int);
static int first_hook(int value) { return backup(value) + 100; }
// A later engine's relocated ADRP/ADD/BR forwards to the first replacement.
static int second_hook(int value) { return first_hook(value) + 1000; }

int main() {
    if (pgr_chain_fixture(5) != 9) return 1;
    void *target = reinterpret_cast<void *>(pgr_chain_fixture);
    void *original = nullptr;
    if (DobbyHook(target, reinterpret_cast<void *>(first_hook), &original) != 0 || !original) return 2;
    backup = reinterpret_cast<int (*)(int)>(original);
    if (pgr_chain_fixture(5) != 109 || backup(5) != 9) return 3;
    uint32_t entry[4];
    std::memcpy(entry, target, sizeof entry);
    // This regression specifically needs Dobby's shorter ADRP/ADD/BR entry.
    if ((entry[0] & 0x9f000000u) != 0x90000000u || entry[2] != 0xd61f0220u) {
        std::puts("FAIL: fixture did not produce the 12-byte entry under test");
        return 4;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 5;
    auto page = reinterpret_cast<uintptr_t>(target) & ~(static_cast<uintptr_t>(page_size) - 1);
    if (mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_WRITE | PROT_EXEC)) return 6;
    // LDR X17, #8; BR X17; 64-bit replacement address.
    uint32_t absolute_jump[4] = {0x58000051u, 0xd61f0220u, 0, 0};
    auto replacement = reinterpret_cast<uintptr_t>(second_hook);
    std::memcpy(absolute_jump + 2, &replacement, sizeof replacement);
    std::memcpy(target, absolute_jump, sizeof absolute_jump);
    __builtin___clear_cache(static_cast<char *>(target), static_cast<char *>(target) + sizeof absolute_jump);
    if (mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_EXEC)) return 7;
    if (backup(5) != 9 || pgr_chain_fixture(5) != 1109) return 8;
    std::puts("PASS: 12-byte hook backup survives a later 16-byte entry replacement");
    return 0;
}
