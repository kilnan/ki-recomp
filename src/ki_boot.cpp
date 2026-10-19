#include "ki_boot.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

namespace ki {

namespace {
const char* kGpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                        "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
}  // namespace

std::vector<uint8_t> read_file_or_die(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::vector<uint8_t>> read_sound_roms_or_die(const std::string& dir) {
    std::vector<std::vector<uint8_t>> roms;
    for (const char* name : {"u10-l1", "u11-l1", "u12-l1", "u13-l1", "u33-l1", "u34-l1", "u35-l1", "u36-l1"})
        roms.push_back(read_file_or_die(dir + "/" + name));
    return roms;
}

void write_wav(const std::string& path, const std::vector<int16_t>& samples, uint32_t rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc((v >> (8 * i)) & 0xff, f); };
    auto u16 = [&](uint16_t v) { std::fputc(v & 0xff, f); std::fputc(v >> 8, f); };
    uint32_t bytes = static_cast<uint32_t>(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (int16_t s : samples) u16(static_cast<uint16_t>(s));
    std::fclose(f);
}

uint32_t load_boot_state(Context& c, const std::string& dir) {
    std::ifstream in(dir + "/entry_regs.txt");
    if (!in) {
        std::fprintf(stderr, "cannot open %s/entry_regs.txt\n", dir.c_str());
        std::exit(2);
    }
    std::map<std::string, uint64_t> regs;
    std::string name, value;
    while (in >> name >> value) regs[name] = std::stoull(value, nullptr, 16);
    for (int i = 0; i < 32; ++i) c.r[i] = regs.at(kGpr[i]);
    c.hi = regs.at("hi");
    c.lo = regs.at("lo");
    for (int i = 0; i < 32; ++i) c.f[i] = regs.at("fpr" + std::to_string(i));
    c.fcsr = static_cast<uint32_t>(regs.at("ccr31"));
    c.cp0[kCp0Status] = regs.at("sr");
    c.cp0[kCp0Cause] = regs.at("cause") & ~0xff00ull;  // pending lines are recomputed
    c.cp0[kCp0Epc] = regs.at("epc");
    c.cp0[kCp0Compare] = regs.at("compare");
    c.count_base = static_cast<uint32_t>(regs.at("count"));
    c.last_count = static_cast<uint32_t>(regs.at("count"));
    c.sram = read_file_or_die(dir + "/entry_sram.bin");
    c.dram = read_file_or_die(dir + "/entry_dram.bin");
    // Snapshots are taken at a function entry: the boot state at 0x88000000
    // (where the boot ROM jumps to $ra), or mid-game (tools/mame_snapshot.py).
    return lo32(regs.at("pc"));
}

uint32_t cold_reset(Context& c) {
    c = Context{};
    c.cp0[kCp0Status] = 0x00400004;  // BEV | ERL
    set_boot_function_table(kBootFunctionTable, kBootFunctionTableSize);
    return 0xbfc00000u;
}

bool run_entry(Context& c, uint32_t entry) {
    RecompFunc fn = lookup_function(entry);
    if (!fn) {
        std::fprintf(stderr, "entry %08x not recompiled\n", entry);
        return false;
    }
    try {
        uint32_t return_site = lo32(c.r[31]);
        fn(c);
        drain(c);
        // A mid-game snapshot starts inside a call; continue at its return site
        // (it must be an entry point, see KI_EXTRA_ENTRIES in CMakeLists.txt).
        if (RecompFunc cont = lookup_function(return_site); cont && entry != 0x88000000u) {
            cont(c);
            drain(c);
        }
        std::printf("entry function returned (unexpected)\n");
        return true;
    } catch (const GuestFault& f) {
        std::printf("guest fault %d at pc=%08x addr=%08x after %llu cycles, depth %u\n", static_cast<int>(f.fault),
                    f.pc, f.addr, static_cast<unsigned long long>(c.cycles), c.depth);
        std::printf("  recent function entries (oldest first):");
        for (uint32_t k = 0; k < 64; ++k) {
            if (k % 8 == 0) std::printf("\n   ");
            std::printf(" %08x", c.recent[(c.recent_pos + k) & 63]);
        }
        std::printf("\n");
        for (int i = 0; i < 32; i += 4)
            std::printf("  %-4s %016llx  %-4s %016llx  %-4s %016llx  %-4s %016llx\n", kGpr[i],
                        static_cast<unsigned long long>(c.r[i]), kGpr[i + 1],
                        static_cast<unsigned long long>(c.r[i + 1]), kGpr[i + 2],
                        static_cast<unsigned long long>(c.r[i + 2]), kGpr[i + 3],
                        static_cast<unsigned long long>(c.r[i + 3]));
        return false;
    }
}

}  // namespace ki
