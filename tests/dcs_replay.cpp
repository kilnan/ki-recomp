// Replays a DCS command stream captured in MAME (tools/mame_dcs_capture.py) on
// the DCS model and compares the DAC output with MAME's recording.
//
// usage: dcs_replay <capture dir> <sound rom dir> [out.wav]
//
// The writes go through the same register semantics as KinstBoard (sound_reset
// bit 0, rising edge of sound_control bit 1 latches sound_data). Both runs start
// at power-on, so the outputs are compared sample by sample without an offset.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "ki/dcs.h"
#include "ki_boot.h"

namespace {

std::vector<int16_t> read_wav_mono(const std::string& path) {
    std::vector<uint8_t> f = ki::read_file_or_die(path);
    // Find the "data" chunk; take the first channel.
    size_t p = 12;
    int channels = 1;
    while (p + 8 <= f.size()) {
        uint32_t len = f[p + 4] | f[p + 5] << 8 | f[p + 6] << 16 | static_cast<uint32_t>(f[p + 7]) << 24;
        if (!std::string(reinterpret_cast<char*>(&f[p]), 4).compare("fmt ")) channels = f[p + 10];
        if (!std::string(reinterpret_cast<char*>(&f[p]), 4).compare("data")) {
            std::vector<int16_t> s;
            for (size_t i = p + 8; i + 2 * channels <= p + 8 + len && i + 1 < f.size(); i += 2 * channels)
                s.push_back(static_cast<int16_t>(f[i] | f[i + 1] << 8));
            return s;
        }
        p += 8 + len;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <capture dir> <sound rom dir> [out.wav]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    ki::Dcs dcs(ki::read_sound_roms_or_die(argv[2]));
    std::ifstream in(dir + "/writes.txt");
    unsigned long long t_ns;
    std::string addr_s, data_s;
    uint32_t sound_control = 0, sound_data = 0;
    size_t writes = 0;
    while (in >> t_ns >> addr_s >> data_s) {
        uint32_t addr = static_cast<uint32_t>(std::stoul(addr_s, nullptr, 16));
        uint32_t data = static_cast<uint32_t>(std::stoul(data_s, nullptr, 16));
        dcs.run_until(t_ns / 100);  // 10 MHz: 100 ns per DSP cycle
        if (addr == 0x10000088) dcs.reset_w(data & 1);
        else if (addr == 0x10000090) {
            if (!(sound_control & 2) && (data & 2)) dcs.data_w(static_cast<uint16_t>(sound_data));
            sound_control = data;
        } else if (addr == 0x10000098) sound_data = data;
        ++writes;
    }
    std::vector<int16_t> mame = read_wav_mono(dir + "/mame.wav");
    dcs.run_until(static_cast<uint64_t>(mame.size()) * ki::Dcs::kClock / 31250);
    const std::vector<int16_t>& ours = dcs.samples();
    if (argc > 3) ki::write_wav(argv[3], ours, 31250);

    // Both streams start at power-on, but MAME's DAC device (dmadac) buffers the
    // transfers, and its latency changes when the DSP restarts the DAC (e.g. after
    // the game reboots the DSP). So the streams are compared segment by segment:
    // when they stop matching, the next 2000 MAME samples are searched for within
    // +-2000 samples of ours; a match there starts a new segment.
    auto matches = [&](size_t im, size_t io, size_t len) {
        if (im + len > mame.size() || io + len > ours.size()) return false;
        for (size_t k = 0; k < len; ++k)
            if (mame[im + k] != ours[io + k]) return false;
        return true;
    };
    size_t im = 0, io = 0, equal = 0, compared = 0, segments = 0;
    long lag = 0;
    bool aligned = false;
    while (im < mame.size() && io < ours.size()) {
        if (aligned && mame[im] == ours[io]) {
            ++equal, ++compared, ++im, ++io;
            continue;
        }
        // (re)align: skip silence, then look for a 2000-sample match nearby
        while (im < mame.size() && mame[im] == 0) ++im;
        if (im + 2000 > mame.size()) break;
        bool found = false;
        for (long d = 0; d <= 2000 && !found; ++d)
            for (long sgn : {1L, -1L}) {
                long cand = static_cast<long>(im) + lag + sgn * d;
                if (cand >= 0 && matches(im, static_cast<size_t>(cand), 2000)) {
                    lag = cand - static_cast<long>(im);
                    io = static_cast<size_t>(cand);
                    found = true;
                    break;
                }
            }
        if (!found) {
            std::printf("no match near MAME sample %zu (%.3f s)\n", im, im / 31250.0);
            ++compared, ++im;
            io = static_cast<size_t>(std::max(0L, static_cast<long>(im) + lag));
            aligned = false;
            continue;
        }
        ++segments;
        std::printf("segment %zu from MAME sample %zu (%.3f s): ours = MAME %+ld samples\n", segments, im,
                    im / 31250.0, lag);
        aligned = true;
    }
    std::printf("%zu writes replayed; %zu segments; equal %zu of %zu compared samples (%.4f%%)\n", writes, segments,
                equal, compared, compared ? 100.0 * equal / compared : 0.0);
    size_t n = compared;
    return equal == n && n > 0 ? 0 : 1;
}
