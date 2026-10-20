// HD sprite rendering for ki_play: the game still draws its 320x240
// framebuffers; next to each one a 4x copy is kept, where sprites with an HD
// replacement are drawn in high resolution and everything else is upscaled
// from the original pixels, in drawing order.
//
// An HD pack is a folder with index.txt ("key image ox oy w h" per line, from
// ki_run --extract-sprites keys.txt) and one <image>.ki4 per image (tools/hd/).
#pragma once

#include <cstdint>
#include <cstdio>
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
    size_t shape_count() const { return shapes_.size(); }
    // Installs the sprite hooks (one renderer at a time).
    void attach();
    void detach();
    // The HD picture of a framebuffer (SRAM offset 0x30000 or 0x58000), ARGB8888, kW x kH.
    const std::vector<uint32_t>& frame(const Context& c, uint32_t bank);
    unsigned hd_sprites_drawn() const { return hd_drawn_; }
    // Blitter calls with no HD image, per header and mode (to find what a pack misses).
    void print_misses(FILE* f) const;
    uint64_t panorama_pixels_hd() const { return pano_hd_; }
    uint64_t panorama_pixels_kept() const { return pano_kept_; }

private:
    struct Image {
        std::string file;
        int ox = 0, oy = 0, w = 0, h = 0;  // placement at scale 1, in original pixels
        bool loaded = false, missing = false;
        int iw = 0, ih = 0;
        std::vector<uint32_t> argb;
        // For recolouring (shapes.txt): the original's palette and palette entry per pixel (.kix).
        std::string kix_file;
        bool kix_loaded = false;
        std::vector<uint8_t> palette, entries;
    };
    struct Rect {
        int x0, y0, x1, y1;  // original pixels, inclusive
    };
    struct Bank {
        std::vector<uint16_t> shadow = std::vector<uint16_t>(320 * 240, 0xffff);
        std::vector<uint32_t> hd = std::vector<uint32_t>(kW * kH, 0xff000000u);
        // Where HD images were drawn: the HD copy there no longer matches the
        // shadow, so it is upscaled again when the game next draws this bank.
        std::vector<Rect> hd_rects;
    };
    void invalidate(const Context& c, uint32_t bank);
    // A stage panorama (panoramas.txt): its original pixels (to tell where the
    // game drew over its copy) and the HD picture, (w * 4) x (h * 4).
    struct Panorama {
        std::string file;
        int w = 0, stride = 0, h = 0;
        bool loaded = false;
        std::vector<uint16_t> original;
        std::vector<uint32_t> hd;
    };
    static void on_panorama(Context& c);
    static void on_panorama_after(Context& c);
    bool load_panorama(Panorama& p);
    std::unordered_map<uint64_t, Panorama> panoramas_;  // key: FNV of the first row
    uint32_t pano_regs_[6] = {};                      // v0 v1 a0 a1 a2 a3 at the copy's entry
    static void on_before(Context& c);
    static void on_after(Context& c);
    Bank& bank_of(uint32_t offset) { return banks_[offset == 0x58000 ? 1 : 0]; }
    void sync(const Context& c, uint32_t bank);  // upscale what changed since the last sync
    bool load(Image& im);
    bool load_kix(Image& im);
    // The HD image of `base` (same shape) for the palette now in the zero page, cached.
    Image* recoloured(const Context& c, Image& base);
    void draw(Bank& b, const Image& im, int x0, int y0, int x1, int y1, bool mirror);

    std::string dir_;
    std::unordered_map<uint64_t, Image> pack_;
    std::unordered_map<uint64_t, Image> shapes_;    // shape key -> base image
    std::unordered_map<uint64_t, Image> variants_;  // (base image, palette) -> recoloured
    Bank banks_[2];
    // the current blitter call
    uint64_t key_ = 0, shape_ = 0;
    uint32_t bank_ = 0, last_bank_ = ~0u;
    int32_t s7_ = 0, t8_ = 0, t1_ = 4096, t3_ = 4096, t4_ = 2, mode_ = 0;
    unsigned hd_drawn_ = 0;
    uint64_t pano_hd_ = 0, pano_kept_ = 0;  // panorama pixels replaced / left to the game
    uint32_t header_ = 0;
    std::unordered_map<uint64_t, unsigned> misses_;  // header << 32 | mode -> calls
};

}  // namespace ki
