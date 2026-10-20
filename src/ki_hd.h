// HD sprite rendering for ki_play: the game still draws its 320x240
// framebuffers; next to each one a 4x copy is kept, where sprites with an HD
// replacement are drawn in high resolution and everything else is upscaled
// from the original pixels, in drawing order.
//
// An HD pack is a folder with index.txt ("key image ox oy w h" per line, from
// ki_run --extract-sprites keys.txt) and one <image>.ki4 per image (tools/hd/).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ki/runtime.h"

namespace ki {

class HdRenderer {
public:
    static constexpr int kScale = 4, kW = 320 * kScale, kH = 240 * kScale;

    // Loads the pack's index; images are read when first drawn. False if no pack.
    bool load_pack(const std::string& dir);
    size_t pack_size() const { return pack_.size(); }
    // Installs the sprite hooks (one renderer at a time).
    void attach();
    void detach();
    // The HD picture of a framebuffer (SRAM offset 0x30000 or 0x58000), ARGB8888, kW x kH.
    const std::vector<uint32_t>& frame(const Context& c, uint32_t bank);
    unsigned hd_sprites_drawn() const { return hd_drawn_; }

private:
    struct Image {
        std::string file;
        int ox = 0, oy = 0, w = 0, h = 0;  // placement at scale 1, in original pixels
        bool loaded = false, missing = false;
        int iw = 0, ih = 0;
        std::vector<uint32_t> argb;
    };
    struct Bank {
        std::vector<uint16_t> shadow = std::vector<uint16_t>(320 * 240, 0xffff);
        std::vector<uint32_t> hd = std::vector<uint32_t>(kW * kH, 0xff000000u);
    };
    static void on_before(Context& c);
    static void on_after(Context& c);
    Bank& bank_of(uint32_t offset) { return banks_[offset == 0x58000 ? 1 : 0]; }
    void sync(const Context& c, uint32_t bank);  // upscale what changed since the last sync
    bool load(Image& im);
    void draw(Bank& b, const Image& im, int x0, int y0, int x1, int y1, bool mirror);

    std::string dir_;
    std::unordered_map<uint64_t, Image> pack_;
    Bank banks_[2];
    // the current blitter call
    uint64_t key_ = 0;
    uint32_t bank_ = 0;
    int32_t s7_ = 0, t8_ = 0, t1_ = 4096, t3_ = 4096, t4_ = 2, mode_ = 0;
    unsigned hd_drawn_ = 0;
};

}  // namespace ki
