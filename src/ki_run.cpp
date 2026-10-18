// Runs the recompiled Killer Instinct program on the board model, headless.
//
// usage: ki_run --boot <dir> --disk <kinst.img> --rom <ki-l15d.u98>
//               [--frames N] [--dump-every K] [--out <dir>]
//
// --boot is a boot-state snapshot (tools/mame_capture.py --entry-only at
// 0x88000000): SRAM, DRAM and CPU/COP0 registers as the original boot ROM
// leaves them when it enters the game. Framebuffers are written as PPM files.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ki/kinst_board.h"
#include "ki/runtime.h"

namespace {

const char* kGpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                        "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::map<std::string, uint64_t> read_regs(const std::string& path) {
    std::ifstream in(path);
    std::map<std::string, uint64_t> regs;
    std::string name, value;
    while (in >> name >> value) regs[name] = std::stoull(value, nullptr, 16);
    return regs;
}

void write_ppm(const std::string& path, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n320 240\n255\n");
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    std::string boot, disk, rom, out = ".";
    uint64_t frames = 600, dump_every = 60;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--boot") boot = v;
        else if (k == "--disk") disk = v;
        else if (k == "--rom") rom = v;
        else if (k == "--frames") frames = std::stoull(v);
        else if (k == "--dump-every") dump_every = std::stoull(v);
        else if (k == "--out") out = v;
    }
    if (boot.empty() || disk.empty() || rom.empty()) {
        std::fprintf(stderr, "usage: %s --boot <dir> --disk <img> --rom <u98> [--frames N] [--dump-every K] [--out dir]\n",
                     argv[0]);
        return 2;
    }

    static ki::Context c;
    auto regs = read_regs(boot + "/entry_regs.txt");
    for (int i = 0; i < 32; ++i) c.r[i] = regs.at(kGpr[i]);
    c.hi = regs.at("hi");
    c.lo = regs.at("lo");
    for (int i = 0; i < 32; ++i) c.f[i] = regs.at("fpr" + std::to_string(i));
    c.fcsr = static_cast<uint32_t>(regs.at("ccr31"));
    c.cp0[ki::kCp0Status] = regs.at("sr");
    c.cp0[ki::kCp0Cause] = regs.at("cause") & ~0xff00ull;  // pending lines are recomputed
    c.cp0[ki::kCp0Epc] = regs.at("epc");
    c.cp0[ki::kCp0Compare] = regs.at("compare");
    c.count_base = static_cast<uint32_t>(regs.at("count"));
    c.last_count = static_cast<uint32_t>(regs.at("count"));
    c.sram = read_file(boot + "/entry_sram.bin");
    c.dram = read_file(boot + "/entry_dram.bin");

    static ki::KinstBoard board(read_file(disk), read_file(rom));
    c.io = &board;
    auto t0 = std::chrono::steady_clock::now();
    board.on_vblank = [&](ki::Context& ctx, uint64_t frame) {
        if (frame % dump_every == 0 || frame >= frames) {
            char name[64];
            std::snprintf(name, sizeof name, "/frame_%05llu.ppm", static_cast<unsigned long long>(frame));
            write_ppm(out + name, board.framebuffer_rgb(ctx));
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("frame %llu  cycles %llu  host %.2fs  ide cmds %llu  sectors %llu/%llu  unknown io %u\n",
                        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(ctx.cycles), secs,
                        static_cast<unsigned long long>(board.stats.ide_commands),
                        static_cast<unsigned long long>(board.stats.sectors_read),
                        static_cast<unsigned long long>(board.stats.sectors_written), board.stats.unknown_io);
            std::fflush(stdout);
        }
        if (frame >= frames) std::exit(0);
    };
    ki::schedule(c);

    uint32_t entry = ki::lo32(c.r[31]);  // the boot ROM jumps to $ra = 0x88000000
    ki::RecompFunc fn = ki::lookup_function(entry);
    if (!fn) {
        std::fprintf(stderr, "entry %08x not recompiled\n", entry);
        return 2;
    }
    try {
        fn(c);
        std::printf("entry function returned (unexpected)\n");
    } catch (const ki::GuestFault& f) {
        std::printf("guest fault %d at pc=%08x addr=%08x after %llu cycles (frame ~%llu)\n", static_cast<int>(f.fault),
                    f.pc, f.addr, static_cast<unsigned long long>(c.cycles),
                    static_cast<unsigned long long>(c.cycles / ki::KinstBoard::kCyclesPerFrame));
        for (int i = 0; i < 32; i += 4)
            std::printf("  %-4s %016llx  %-4s %016llx  %-4s %016llx  %-4s %016llx\n", kGpr[i],
                        static_cast<unsigned long long>(c.r[i]), kGpr[i + 1], static_cast<unsigned long long>(c.r[i + 1]),
                        kGpr[i + 2], static_cast<unsigned long long>(c.r[i + 2]), kGpr[i + 3],
                        static_cast<unsigned long long>(c.r[i + 3]));
        std::printf("  dram 0x88100000:");
        for (int i = 0; i < 16; ++i) std::printf(" %02x", c.dram[0x100000 + i]);
        std::printf("\n");
        write_ppm(out + "/fault.ppm", board.framebuffer_rgb(c));
        return 1;
    }
    return 0;
}
