// Runs the recompiled Killer Instinct program on the board model, headless.
//
// usage: ki_run --boot <dir> --disk <kinst.img> --rom <ki-l15d.u98>
//               [--frames N] [--dump-every K] [--out <dir>] [--inputs <file>]
//
// --inputs replays a script of "frame port bit value" lines (port 1 or 2, bit
// mask of the P1/P2 port, value 1 = pressed), e.g. from tools/make_input_script.py.
//
// --boot is a boot-state snapshot (tools/mame_capture.py --entry-only at
// 0x88000000): SRAM, DRAM and CPU/COP0 registers as the original boot ROM
// leaves them when it enters the game. Framebuffers are written as PPM files.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "ki/kinst_board.h"
#include "ki/runtime.h"
#include "ki_boot.h"

namespace {

void write_ppm(const std::string& path, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n320 240\n255\n");
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    std::string boot, disk, rom, out = ".", inputs, sound_roms, wav_out;
    static uint32_t dump_at = 0;
    static unsigned long dump_hit = 1;
    static uint64_t call_log_from = ~0ull;  // frame from which function entries are logged
    static std::vector<uint32_t> call_log;
    static std::string trace_path;
    static uint64_t trace_max = 1000000;
    static std::string dump_dir;
    uint64_t frames = 600, dump_every = 60;
    static uint64_t dram_from = ~0ull, dram_to = 0;
    static uint64_t save_at = 0, load_at = 0;
    static bool loaded_once = false;
    static ki::SaveState state;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--boot") boot = v;
        else if (k == "--disk") disk = v;
        else if (k == "--rom") rom = v;
        else if (k == "--frames") frames = std::stoull(v);
        else if (k == "--dump-every") dump_every = std::stoull(v);
        else if (k == "--out") out = v;
        else if (k == "--inputs") inputs = v;
        else if (k == "--dump-at") {  // ADDR[:N] = N-th entry (default 1)
            size_t colon = v.find(':');
            dump_at = static_cast<uint32_t>(std::stoul(v.substr(0, colon), nullptr, 16));
            if (colon != std::string::npos) dump_hit = std::stoul(v.substr(colon + 1));
        }
        else if (k == "--call-log-from") call_log_from = std::stoull(v);
        else if (k == "--trace-out") trace_path = v;
        else if (k == "--trace-max") trace_max = std::stoull(v);
        else if (k == "--sound-roms") sound_roms = v;
        else if (k == "--wav-out") wav_out = v;
        else if (k == "--save-at") save_at = std::stoull(v);  // save-state test: save at frame N ...
        else if (k == "--load-at") load_at = std::stoull(v);  // ... and load it back at frame M (once)
        else if (k == "--dram-frames") {  // A-B: write DRAM at each VBLANK of frames A..B (dram_<frame>.bin)
            size_t dash = v.find('-');
            dram_from = std::stoull(v.substr(0, dash));
            dram_to = dash == std::string::npos ? dram_from : std::stoull(v.substr(dash + 1));
        }
    }
    if (boot.empty() || disk.empty() || rom.empty()) {
        std::fprintf(stderr, "usage: %s --boot <dir> --disk <img> --rom <u98> [--frames N] [--dump-every K] [--out dir]\n",
                     argv[0]);
        return 2;
    }

    static ki::Context c;
    // --boot rom: start at the reset vector, like the real board (needs no MAME snapshot).
    const bool cold = boot == "rom";
    uint32_t entry = cold ? ki::cold_reset(c) : ki::load_boot_state(c, boot);
    static ki::KinstBoard board(ki::read_file_or_die(disk), ki::read_file_or_die(rom));
    c.io = &board;
    static std::unique_ptr<ki::Dcs> dcs;
    static std::string wav_path;
    wav_path = wav_out;
    if (!sound_roms.empty()) {
        dcs = std::make_unique<ki::Dcs>(ki::read_sound_roms_or_die(sound_roms));
        board.dcs = dcs.get();
    }
    if (cold) board.set_cycles_to_vblank(0);  // MAME's screen starts a VBLANK at time 0 (every frame since)
    else if (entry != 0x88000000u) board.set_logical_geometry(14, 40);  // set by the boot code (IDE command 0x91)
    {
        std::ifstream scr(boot + "/screen.json");  // mid-game snapshots record the beam position
        std::string text((std::istreambuf_iterator<char>(scr)), std::istreambuf_iterator<char>());
        size_t p = text.find("\"to_vblank_ns\":");

        if (p != std::string::npos)
            board.set_cycles_to_vblank(std::stoull(text.substr(p + 15)) / 10);  // 100 MHz: 10 ns per cycle
    }
    struct Event {
        uint64_t frame;
        int port;
        uint32_t bit;
        int value;
    };
    std::vector<Event> script;
    if (!inputs.empty()) {
        std::ifstream in(inputs);
        Event e;
        while (in >> e.frame >> e.port >> std::hex >> e.bit >> std::dec >> e.value) script.push_back(e);
    }
    size_t next_event = 0;
    auto t0 = std::chrono::steady_clock::now();
    board.on_vblank = [&](ki::Context& ctx, uint64_t frame) {
        if (frame == call_log_from) ctx.call_log = &call_log;
        if (save_at && frame == save_at && !loaded_once) state.request(ctx);
        if (load_at && frame == load_at && !loaded_once && state.valid()) {
            // Save-state test: the frame at load_at is written before and after the load.
            write_ppm(out + "/before_load.ppm", board.framebuffer_rgb(ctx));
            loaded_once = true;
            throw ki::LoadStateRequest{};
        }
        if (load_at && loaded_once && frame == load_at) write_ppm(out + "/after_load.ppm", board.framebuffer_rgb(ctx));
        if (frame >= dram_from && frame <= dram_to) {
            char name[64];
            std::snprintf(name, sizeof name, "/dram_%05llu.bin", static_cast<unsigned long long>(frame));
            if (FILE* f = std::fopen((out + name).c_str(), "wb")) {
                std::fwrite(ctx.dram.data(), 1, ctx.dram.size(), f);
                std::fclose(f);
            }
        }
        while (next_event < script.size() && script[next_event].frame <= frame) {
            const Event& e = script[next_event++];
            uint32_t& port = e.port == 1 ? board.p1 : board.p2;
            port = e.value ? (port & ~e.bit) : (port | e.bit);
        }
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
        if (frame + 60 == frames) board.io_histogram_on = true;  // last second only
        if (frame >= frames) {
            if (!call_log.empty()) {
                FILE* f = std::fopen((out + "/calls.txt").c_str(), "w");
                for (uint32_t a : call_log) std::fprintf(f, "%08x\n", a);
                std::fclose(f);
            }
            std::printf("I/O accesses in the last 60 frames (9xxxxxxx = write):");
            for (auto& [a, n] : board.io_histogram()) std::printf(" %08x:%llu", a, static_cast<unsigned long long>(n));
            std::printf("\nrecent function entries (oldest first):");
            for (uint32_t k = 0; k < 64; ++k) {
                if (k % 8 == 0) std::printf("\n   ");
                std::printf(" %08x", ctx.recent[(ctx.recent_pos + k) & 63]);
            }
            std::printf("\nsaved frame Count [0x88089f08] = %08x\n",
                        ctx.dram[0x89f08] | ctx.dram[0x89f09] << 8 | ctx.dram[0x89f0a] << 16 |
                            static_cast<uint32_t>(ctx.dram[0x89f0b]) << 24);
            std::printf("interrupts %llu compare %08llx count %08llx\n",
                        static_cast<unsigned long long>(ctx.interrupts_taken),
                        static_cast<unsigned long long>(ctx.cp0[ki::kCp0Compare]),
                        static_cast<unsigned long long>(ki::mfc0(ctx, ki::kCp0Count, 0)));
            std::printf("safe points where service ran (oldest first):");
            for (uint32_t k = 0; k < 64; ++k) {
                if (k % 8 == 0) std::printf("\n   ");
                std::printf(" %08x", ctx.recent_service_pc[(ctx.recent_service_pos + k) & 63]);
            }
            std::printf("\n");
            uint32_t s8 = ki::lo32(ctx.r[30]);
            std::printf("s8 %08x:", s8);
            if (s8 - 0x88000000u < ki::kDramSize - 16)
                for (int i = 0; i < 16; ++i) std::printf(" %02x", ctx.dram[s8 - 0x88000000u + i]);
            if (dcs) {
                std::printf("\nDCS: %zu samples at %.1f Hz, %llu DSP cycles", dcs->samples().size(), dcs->sample_rate(),
                            static_cast<unsigned long long>(dcs->cycles()));
                if (!wav_path.empty())
                    ki::write_wav(wav_path, dcs->samples(), static_cast<uint32_t>(dcs->sample_rate() + 0.5));
            }
            std::printf("\nstatus %08llx cause %08llx  sound writes %llu\n",
                        static_cast<unsigned long long>(ctx.cp0[ki::kCp0Status]),
                        static_cast<unsigned long long>(ki::mfc0(ctx, ki::kCp0Cause, 0)),
                        static_cast<unsigned long long>(board.stats.sound_writes));
            std::exit(0);
        }
    };
    if (dump_at) {
        // --dump-at ADDR: save SRAM/DRAM and registers at the first entry of a function, then exit.
        dump_dir = out;
        c.watch_entry = dump_at;
        c.on_watch = [](ki::Context& ctx, uint32_t) {
            static unsigned long hits = 0;
            if (++hits < dump_hit) return;
            auto save = [](const std::string& path, const std::vector<uint8_t>& data) {
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f) { std::fwrite(data.data(), 1, data.size(), f); std::fclose(f); }
            };
            save(dump_dir + "/dump_sram.bin", ctx.sram);
            // Registers in the snapshot format (tools/mame_capture.py REGS), so the
            // dump can itself be used as --boot state.
            static const char* names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                            "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                            "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
            for (const char* fname : {"/dump_regs.txt", "/entry_regs.txt"}) {
                FILE* rf = std::fopen((dump_dir + fname).c_str(), "w");
                if (!rf) continue;
                auto put = [&](const char* n, uint64_t v) { std::fprintf(rf, "%s %016llx\n", n, static_cast<unsigned long long>(v)); };
                for (int i = 0; i < 32; ++i) put(names[i], ctx.r[i]);
                put("hi", ctx.hi);
                put("lo", ctx.lo);
                put("pc", ctx.watch_entry);
                for (int i = 0; i < 32; ++i) put(("fpr" + std::to_string(i)).c_str(), ctx.f[i]);
                put("ccr31", ctx.fcsr);
                put("sr", ctx.cp0[ki::kCp0Status]);
                put("cause", ki::mfc0(ctx, ki::kCp0Cause, 0));
                put("epc", ctx.cp0[ki::kCp0Epc]);
                put("count", ki::mfc0(ctx, ki::kCp0Count, 0));
                put("compare", ctx.cp0[ki::kCp0Compare]);
                std::fclose(rf);
            }
            save(dump_dir + "/entry_sram.bin", ctx.sram);
            save(dump_dir + "/entry_dram.bin", ctx.dram);
            if (FILE* sf = std::fopen((dump_dir + "/screen.json").c_str(), "w")) {
                std::fprintf(sf, "{\"to_vblank_ns\": %llu}\n",
                             static_cast<unsigned long long>((board.next_event(ctx) - ctx.cycles) * 10));
                std::fclose(sf);
            }
            if (!call_log.empty()) {
                FILE* f = std::fopen((dump_dir + "/calls.txt").c_str(), "w");
                for (uint32_t a : call_log) std::fprintf(f, "%08x\n", a);
                std::fclose(f);
            }
            save(dump_dir + "/dump_dram.bin", ctx.dram);
            std::printf("dumped at %08x after %llu cycles\n", ctx.watch_entry, static_cast<unsigned long long>(ctx.cycles));
            std::exit(0);
        };
    }
    if (call_log_from == 0) c.call_log = &call_log;
    static FILE* trace_file = nullptr;
    static uint64_t traced = 0;
    if (!trace_path.empty()) {
        // Requires ki_run_trace (code generated with --trace): one PC per line, like MAME's trace.
        trace_file = std::fopen(trace_path.c_str(), "w");
        c.trace_fn = [](ki::Context&, uint32_t pc) {
            std::fprintf(trace_file, "%08X\n", pc);
            if (++traced >= trace_max) {
                std::fclose(trace_file);
                std::exit(0);
            }
        };
    }
    if (dcs) state.bind(board, dcs.get(), ki::read_sound_roms_or_die(sound_roms));
    else state.bind(board, nullptr, {});
    ki::schedule(c);
    bool ran = false;
    for (;;) {
        try {
            ran = ki::run_entry(c, entry);
            break;
        } catch (const ki::LoadStateRequest&) {
            entry = state.restore(c);
            // replay the input script from the restored frame
            next_event = 0;
            std::printf("state loaded at frame %llu\n", static_cast<unsigned long long>(load_at));
        }
    }
    if (!ran) {
        std::printf("  dram 0x88100000:");
        for (int i = 0; i < 16; ++i) std::printf(" %02x", c.dram[0x100000 + i]);
        std::printf("\n");
        write_ppm(out + "/fault.ppm", board.framebuffer_rgb(c));
        return 1;
    }
    return 0;
}
