// Replays one MAME capture through the recompiled code and compares the
// final CPU state and memory with what MAME observed.
//
// usage: compare_case <case_dir> [function_vaddr_hex]
//
// A case directory (written by tools/mame_capture.py) holds
//   entry_regs.txt / exit_regs.txt   "name value_hex" for 32 GPRs, hi, lo, pc
//   entry_{sram,dram}.bin / exit_{sram,dram}.bin   raw memory dumps
// Exit code 0 only if every register and every byte of SRAM and DRAM match.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ki/runtime.h"
#ifdef KI_COMPARE_FULL
#include "ki/kinst_board.h"
#endif

namespace {

const char* kGprNames[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                             "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                             "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

std::map<std::string, uint64_t> read_regs(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::map<std::string, uint64_t> regs;
    std::string name, value;
    while (in >> name >> value) regs[name] = std::stoull(value, nullptr, 16);
    return regs;
}

std::vector<uint8_t> read_file(const std::string& path, size_t expected) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() != expected) {
        std::fprintf(stderr, "%s: size %zu, expected %zu\n", path.c_str(), data.size(), expected);
        std::exit(2);
    }
    return data;
}

// Prints byte ranges where a and b differ; returns the number of differing bytes.
size_t diff_ranges(const char* label, uint32_t base, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                   bool print) {
    size_t total = 0;
    int shown = 0;
    for (size_t i = 0; i < a.size();) {
        if (a[i] == b[i]) { ++i; continue; }
        size_t j = i;
        while (j < a.size() && a[j] != b[j]) ++j;
        total += j - i;
        if (print && shown++ < 8)
            std::printf("    %s 0x%08zx-0x%08zx (%zu bytes)\n", label, base + i, base + j - 1, j - i);
        i = j;
    }
    return total;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <case_dir> [function_vaddr_hex]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    auto entry = read_regs(dir + "/entry_regs.txt");
    auto expect = read_regs(dir + "/exit_regs.txt");
    uint32_t func = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2], nullptr, 16))
                             : static_cast<uint32_t>(entry.at("pc"));

    ki::Context c;
    for (int i = 0; i < 32; ++i) c.r[i] = entry.at(kGprNames[i]);
    c.hi = entry.at("hi");
    c.lo = entry.at("lo");
    // COP1 state is present in captures made after FPU support was added.
    const bool has_fpu = entry.count("ccr31") && expect.count("ccr31");
    if (has_fpu) {
        for (int i = 0; i < 32; ++i) c.f[i] = entry.at("fpr" + std::to_string(i));
        c.fcsr = static_cast<uint32_t>(entry.at("ccr31"));
    }
    c.sram = read_file(dir + "/entry_sram.bin", ki::kSramSize);
    c.dram = read_file(dir + "/entry_dram.bin", ki::kDramSize);
    const auto entry_sram = c.sram, entry_dram = c.dram;
    const auto exit_sram = read_file(dir + "/exit_sram.bin", ki::kSramSize);
    const auto exit_dram = read_file(dir + "/exit_dram.bin", ki::kDramSize);

#ifdef KI_COMPARE_FULL
    // Whole-program variant: the board model answers I/O (inputs released,
    // disk from KI_DISK, boot ROM from KI_ROM), timing and interrupts run.
    static std::vector<uint8_t> disk, rom;
    if (const char* p = std::getenv("KI_DISK")) disk = read_file(p, 131076608);
    if (const char* p = std::getenv("KI_ROM")) rom = read_file(p, 0x80000);
    static ki::KinstBoard board(disk, rom);
    board.set_logical_geometry(14, 40);
    c.io = &board;
    if (entry.count("count")) {
        c.cp0[ki::kCp0Status] = entry.at("sr");
        c.cp0[ki::kCp0Compare] = entry.at("compare");
        c.count_base = static_cast<uint32_t>(entry.at("count"));
        c.last_count = static_cast<uint32_t>(entry.at("count"));
    }
    ki::schedule(c);
#endif
    ki::RecompFunc fn = ki::lookup_function(func);
    if (!fn) {
        std::fprintf(stderr, "function %08x is not in the recompiled table\n", func);
        return 2;
    }
    try {
        fn(c);
        ki::drain(c);  // tail calls to other recompiled functions
    } catch (const ki::GuestFault& f) {
        std::printf("FAIL %s: guest fault %d at pc=%08x addr=%08x\n", dir.c_str(), static_cast<int>(f.fault), f.pc,
                    f.addr);
        return 1;
    }

    bool ok = true;
    std::printf("case %s (function %08x)\n", dir.c_str(), func);
    std::printf("  registers changed by MAME (entry -> exit) and recompiled result:\n");
    auto check = [&](const char* name, uint64_t before, uint64_t got) {
        uint64_t want = expect.at(name);
        bool match = got == want;
        ok &= match;
        if (before != want || !match)
            std::printf("    %-4s %016llx -> %016llx  recomp %016llx  %s\n", name, (unsigned long long)before,
                        (unsigned long long)want, (unsigned long long)got, match ? "ok" : "MISMATCH");
    };
    for (int i = 0; i < 32; ++i) check(kGprNames[i], entry.at(kGprNames[i]), c.r[i]);
    check("hi", entry.at("hi"), c.hi);
    check("lo", entry.at("lo"), c.lo);
    if (has_fpu) {
        // Only the low 32 bits of each FPR: the game uses single precision, and
        // MAME's DRC and interpreter disagree on the high half after 32-bit writes.
        for (int i = 0; i < 32; ++i) {
            std::string n = "fpr" + std::to_string(i);
            uint64_t before = ki::lo32(entry.at(n)), want = ki::lo32(expect.at(n)), got = ki::lo32(c.f[i]);
            bool match = got == want;
            ok &= match;
            if (before != want || !match)
                std::printf("    f%-3d %08llx -> %08llx  recomp %08llx  %s\n", i, (unsigned long long)before,
                            (unsigned long long)want, (unsigned long long)got, match ? "ok" : "MISMATCH");
        }
        uint32_t before = static_cast<uint32_t>(entry.at("ccr31")), want = static_cast<uint32_t>(expect.at("ccr31"));
        bool match = c.fcsr == want;
        ok &= match;
        if (before != want || !match)
            std::printf("    fcsr %08x -> %08x  recomp %08x  %s\n", before, want, c.fcsr, match ? "ok" : "MISMATCH");
    }
    // The recompiled function returns to its caller; MAME stopped at $ra.
    uint64_t ret = c.r[31];
    bool pc_ok = ki::lo32(ret) == ki::lo32(expect.at("pc"));
    ok &= pc_ok;
    std::printf("    pc   returns to %08x (MAME stopped at %08x) %s\n", ki::lo32(ret), ki::lo32(expect.at("pc")),
                pc_ok ? "ok" : "MISMATCH");

    std::printf("  memory modified by MAME during the call:\n");
    size_t ms = diff_ranges("sram", ki::kSramBase | 0x80000000u, entry_sram, exit_sram, true);
    size_t md = diff_ranges("dram", ki::kDramBase | 0x80000000u, entry_dram, exit_dram, true);
    if (ms + md == 0) std::printf("    (none)\n");
    size_t rs = diff_ranges("sram", ki::kSramBase | 0x80000000u, c.sram, exit_sram, false);
    size_t rd = diff_ranges("dram", ki::kDramBase | 0x80000000u, c.dram, exit_dram, false);
    std::printf("  memory recomp vs MAME exit: %zu differing bytes in SRAM, %zu in DRAM\n", rs, rd);
    if (rs + rd) {
        diff_ranges("sram", ki::kSramBase | 0x80000000u, c.sram, exit_sram, true);
        diff_ranges("dram", ki::kDramBase | 0x80000000u, c.dram, exit_dram, true);
    }
    ok &= (rs + rd) == 0;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", dir.c_str());
    return ok ? 0 : 1;
}
