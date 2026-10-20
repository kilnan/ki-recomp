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
#include <map>
#include <set>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "ki/hooks.h"
#include "ki/kinst_board.h"
#include "ki/runtime.h"
#include "ki_boot.h"
#include "ki_fight.h"
#include "ki_sprites.h"
#ifdef KI_HAVE_HD
#include "ki_hd.h"
#endif

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
    static ki::FightChoice fight;
    static uint64_t board_frame = 0;  // the frame being run (for KI_MEM_PROBE)
    static uint64_t call_log_from = ~0ull;  // frame from which function entries are logged
    static std::vector<uint32_t> call_log;
    static std::string trace_path;
    static uint64_t trace_max = 1000000;
    static std::string dump_dir;
    uint64_t frames = 600, dump_every = 60;
    static uint64_t dram_from = ~0ull, dram_to = 0;
    static uint64_t save_at = 0, load_at = 0, log_sprites = 0;
    static std::string sprite_dir, hd_pack;
#ifdef KI_HAVE_HD
    static ki::HdRenderer hd;
#endif
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
        else if (k == "--log-sprites") log_sprites = std::stoull(v);  // print the blitter calls of frame N
        else if (k == "--extract-sprites") sprite_dir = v;  // write each distinct sprite drawn to DIR
        else if (k == "--extract-character") {  // BASE:SIZE[:P,P,...] (hex; palettes decimal): every sprite of this character data
            size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
            std::vector<int> palettes;
            if (c2 != std::string::npos)
                for (size_t p = c2 + 1; p < v.size();) {
                    size_t q = v.find(',', p);
                    if (q == std::string::npos) q = v.size();
                    palettes.push_back(std::stoi(v.substr(p, q - p)));
                    p = q + 1;
                }
            ki::extract_character(static_cast<uint32_t>(std::stoul(v.substr(0, c1), nullptr, 16)),
                                  static_cast<uint32_t>(std::stoul(v.substr(c1 + 1, c2 - c1 - 1), nullptr, 16)), palettes);
        }
        else if (k == "--hd-pack") hd_pack = v;  // also write 4x frames (hd_frame_N.ppm) with this HD pack
        else if (k == "--opponent") ki::parse_characters(v, fight.opponents);  // as ki_play
        else if (k == "--stage") ki::parse_numbers(v, ki::kStageCount, fight.stages);
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
        board_frame = frame + 1;
        if (frame == call_log_from) ctx.call_log = &call_log;
        if (log_sprites && frame == log_sprites) {
            std::printf("sprites drawn in frame %llu: hdr(v1) [h0 h2 h4 h6] x(s7) y(t8) sx(t1) sy(t3) t4 s2 mode(s6) a0 a3 s4 s5 gp s8\n",
                        static_cast<unsigned long long>(frame));
            ki::hooks::sprite = [](ki::Context& g) {
                auto r = [&](int i) { return static_cast<int32_t>(ki::lo32(g.r[i])); };
                uint32_t h = ki::lo32(g.r[3]);
                auto hw = [&](int off) {
                    uint32_t a = h + off - 0x88000000u;
                    return a + 1 < g.dram.size() ? static_cast<int16_t>(g.dram[a] | g.dram[a + 1] << 8) : 0;
                };
                std::printf("  %08x [%5d %5d %5d %5d] %6d %6d %5d %5d %6d %6d %3d %6d %6d %6d %6d %4d %4d\n", h, hw(0),
                            hw(2), hw(4), hw(6), r(23), r(24), r(9), r(11), r(12), r(18), r(22), r(4), r(7), r(20),
                            r(21), r(28), r(30));
                if (std::getenv("KI_LOG_PALETTE")) {  // the palette: zero page 0..0xff (see ki_sprites.cpp)
                    std::printf("    ra %08x palette ", ki::lo32(g.r[31]));
                    for (int i = 0; i < 0x100; ++i) std::printf("%02x", g.dram[0x90000 + i]);
                    std::printf("\n");
                }
            };
        } else if (log_sprites && frame == log_sprites + 1) {
            ki::hooks::sprite = nullptr;
        }
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
#ifdef KI_HAVE_HD
            if (!hd_pack.empty()) {
                const std::vector<uint32_t>& px = hd.frame(ctx, board.displayed_bank());
                std::snprintf(name, sizeof name, "/hd_frame_%05llu.ppm", static_cast<unsigned long long>(frame));
                if (FILE* f = std::fopen((out + name).c_str(), "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", ki::HdRenderer::kW, ki::HdRenderer::kH);
                    for (uint32_t p : px) { std::fputc((p >> 16) & 255, f); std::fputc((p >> 8) & 255, f); std::fputc(p & 255, f); }
                    std::fclose(f);
                }
            }
#endif
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
#ifdef KI_HAVE_HD
            if (!hd_pack.empty()) {
                hd.print_misses(stdout);
                std::printf("HD sprites: %llu exact, %llu recoloured\n", static_cast<unsigned long long>(hd.hits_exact_),
                            static_cast<unsigned long long>(hd.hits_shape_));
                std::printf("HD panorama pixels: %llu replaced, %llu left to the game\n",
                            static_cast<unsigned long long>(hd.panorama_pixels_hd()),
                            static_cast<unsigned long long>(hd.panorama_pixels_kept()));
                std::printf("HD pre-rendered blocks: %llu replaced, %llu left to the game\n",
                            static_cast<unsigned long long>(hd.prerendered_blocks_hd()),
                            static_cast<unsigned long long>(hd.prerendered_blocks_kept()));
            }
#endif
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
    if (!sprite_dir.empty()) ki::start_sprite_extraction(sprite_dir);
    if (!fight.opponents.empty() || !fight.stages.empty()) ki::install_fight_choice(&fight);
#ifdef KI_MEM_PROBE
    // KI_MEM_PROBE=LO-HI[:FRAME]: accesses made by code in [LO, HI), per 1 KB page of address, during FRAME.
    if (const char* probe = std::getenv("KI_MEM_PROBE")) {
        static uint32_t lo, hi;
        static uint64_t probe_frame;
        unsigned long long f = 0;
        std::sscanf(probe, "%x-%x:%llu", &lo, &hi, &f);
        probe_frame = f;
        static std::map<uint64_t, std::pair<unsigned, unsigned>> pages;  // page -> loads, stores
        static std::set<uint64_t> active_frames;
        ki::g_mem_probe = [](ki::Context& ctx, uint32_t va, int size, uint32_t pc, bool store) {
            if (pc - lo >= hi - lo || (probe_frame && board_frame != probe_frame)) return;
            static uint32_t va_lo = 0, va_hi = 0;  // KI_MEM_PROBE_VA=LO-HI: only these addresses
            static const bool va_set = [] {
                if (const char* v = std::getenv("KI_MEM_PROBE_VA")) return std::sscanf(v, "%x-%x", &va_lo, &va_hi) == 2;
                return false;
            }();
            if (va_set && va - va_lo >= va_hi - va_lo) return;
            // KI_MEM_TRACE=FILE: also every access in order (pc, address, size, store), binary.
            static FILE* trace = std::getenv("KI_MEM_TRACE") ? std::fopen(std::getenv("KI_MEM_TRACE"), "wb") : nullptr;
            if (trace) {
                uint32_t rec[3] = {pc, va, static_cast<uint32_t>(size) | (store ? 0x100u : 0u)};
                std::fwrite(rec, 4, 3, trace);
            }
            static const bool by_pc = std::getenv("KI_MEM_PROBE_BY_PC") != nullptr;  // key: code page instead
            auto& e = pages[by_pc ? pc >> 6 : va >> 10];
            active_frames.insert(board_frame);
            (store ? e.second : e.first)++;
            (void)ctx;
        };
        std::atexit([] {
            std::fprintf(stderr, "probe frames:");  // as ranges
            for (auto it = active_frames.begin(); it != active_frames.end();) {
                uint64_t a = *it, b = a;
                while (++it != active_frames.end() && *it == b + 1) ++b;
                std::fprintf(stderr, " %llu-%llu", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
            }
            std::fprintf(stderr, "\n");
            for (const auto& [page, n] : pages)
                std::fprintf(stderr, "probe %08llx loads %u stores %u\n",
                             static_cast<unsigned long long>(page << (std::getenv("KI_MEM_PROBE_BY_PC") ? 6 : 10)), n.first,
                             n.second);
        });
    }
#endif
#ifdef KI_HAVE_HD
    if (!hd_pack.empty()) {
        if (!hd.load_pack(hd_pack)) std::fprintf(stderr, "no HD pack in %s\n", hd_pack.c_str());
        hd.attach();
    }
#endif
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
