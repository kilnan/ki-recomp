#include "ki_sprites.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "ki/hooks.h"

namespace ki::gen {
void func_880107a8_original(ki::Context& c);
}

namespace ki {

namespace {

std::string g_dir;
std::set<uint64_t> g_seen_keys, g_seen_images;
unsigned g_written = 0;

uint64_t fnv(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

uint32_t bank_offset(const Context& c) {
    // 0x88010900: the blitter draws into 0x30000 or 0x58000 per bit 0 of 0x88086200.
    return (c.dram[0x86200] & 1) ? 0x58000 : 0x30000;
}

struct Bbox {
    int x0 = 320, y0 = 240, x1 = -1, y1 = -1;
};

// Draws the sprite of the current call alone on a background of `fill`,
// returns the bank's 320x240 pixels. The caller restores all state.
std::vector<uint16_t> draw_alone(Context& c, const Context& args, uint16_t fill, int32_t s7, int32_t t8) {
    uint32_t bank = bank_offset(args);
    for (int i = 0; i < 320 * 240; ++i) {
        c.sram[bank + 2 * i] = static_cast<uint8_t>(fill);
        c.sram[bank + 2 * i + 1] = static_cast<uint8_t>(fill >> 8);
    }
    std::memcpy(c.r, args.r, sizeof c.r);
    c.r[23] = static_cast<uint64_t>(static_cast<int64_t>(s7));
    c.r[24] = static_cast<uint64_t>(static_cast<int64_t>(t8));
    c.next_event = ~0ull;  // no interrupts or board events during the extra draw
    gen::func_880107a8_original(c);
    drain(c);
    std::vector<uint16_t> px(320 * 240);
    for (int i = 0; i < 320 * 240; ++i) px[i] = c.sram[bank + 2 * i] | c.sram[bank + 2 * i + 1] << 8;
    return px;
}

Bbox bbox_of(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    Bbox bb;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
            if (a[y * 320 + x] != 0x0000 || b[y * 320 + x] != 0x7fff) {
                bb.x0 = std::min(bb.x0, x), bb.y0 = std::min(bb.y0, y);
                bb.x1 = std::max(bb.x1, x), bb.y1 = std::max(bb.y1, y);
            }
    return bb;
}

void probe_geometry(Context& c) {
    // Draws the current sprite at one position with different scales and mirroring
    // and prints the bounding boxes, to fit the blitter's placement rule.
    const Context saved = c;
    const uint8_t* hdr = &c.dram[lo32(c.r[3]) - 0x88000000u];
    std::printf("geometry of %08x: hdr %d %d %d %d\n", lo32(c.r[3]), static_cast<int16_t>(hdr[0] | hdr[1] << 8),
                static_cast<int16_t>(hdr[2] | hdr[3] << 8), static_cast<int16_t>(hdr[4] | hdr[5] << 8),
                static_cast<int16_t>(hdr[6] | hdr[7] << 8));
    struct Case { int32_t sx, sy, t4, s2; };
    const Case cases[] = {{4096, 4096, 2, 640}, {8192, 4096, 2, 640}, {4096, 8192, 2, 640}, {2048, 2048, 2, 640},
                          {4096, 4096, -2, 640}, {8192, 4096, -2, 640}, {4096, 4096, 2, -640}};
    for (const Case& k : cases) {
        Context args = saved;
        args.r[9] = static_cast<uint64_t>(static_cast<int64_t>(k.sx));
        args.r[11] = static_cast<uint64_t>(static_cast<int64_t>(k.sy));
        args.r[12] = static_cast<uint64_t>(static_cast<int64_t>(k.t4));
        args.r[18] = static_cast<uint64_t>(static_cast<int64_t>(k.s2));
        args.r[28] = 0, args.r[30] = 240;
        try {
            auto a = draw_alone(c, args, 0x0000, 160 * 256, 120 * 256), b = draw_alone(c, args, 0x7fff, 160 * 256, 120 * 256);
            Bbox bb = bbox_of(a, b);
            std::printf("  sx %5d sy %5d t4 %2d s2 %4d -> x %d..%d y %d..%d\n", k.sx, k.sy, k.t4, k.s2, bb.x0, bb.x1, bb.y0, bb.y1);
        } catch (const GuestFault&) {
            std::printf("  sx %d sy %d t4 %d s2 %d -> fault\n", k.sx, k.sy, k.t4, k.s2);
        }
    }
    c = saved;
}

void on_sprite(Context& c) {
    uint32_t h = lo32(c.r[3]);
    if (h - 0x88000000u >= c.dram.size() - 1024) return;
    const uint8_t* hdr = &c.dram[h - 0x88000000u];
    if (lo32(c.r[22]) == 48) return;  // mode 48: the squashed dark shadow of a sprite drawn normally too
    static int probes = std::getenv("KI_SPRITE_GEOMETRY") ? 3 : 0;
    const uint8_t* ph = &c.dram[h - 0x88000000u];
    if (probes > 0 && lo32(c.r[22]) == 0 && static_cast<int16_t>(ph[6] | ph[7] << 8) > 60) {
        --probes;
        probe_geometry(c);
    }
    uint64_t key = sprite_key(c);
    if (!g_seen_keys.insert(key).second) return;

    const Context saved = c;  // the extra draws must leave no trace
    // A plain copy in the sprite's own mode: scale 1, no mirroring, full-screen clip window.
    Context args = saved;
    args.r[9] = args.r[11] = 4096;    // t1, t3: scale (4.12)
    args.r[12] = 2;                   // t4 > 0: not mirrored
    args.r[18] = 640;                 // s2 > 0: not flipped (also the framebuffer pitch in bytes)
    args.r[28] = 0;                   // gp: clip top
    args.r[30] = 240;                 // s8: clip bottom
    // Positions are in 1/256 pixel. Draw at the centre, then move the sprite
    // so that its bounding box starts at (8, 8).
    int32_t x = 160 * 256, y = 120 * 256, drawn_x = x, drawn_y = y;
    std::vector<uint16_t> a, b;
    Bbox bb;
    try {
        for (int pass = 0; pass < 2; ++pass) {
            drawn_x = x, drawn_y = y;
            a = draw_alone(c, args, 0x0000, x, y);
            b = draw_alone(c, args, 0x7fff, x, y);
            bb = bbox_of(a, b);
            if (bb.x1 < 0) break;
            if (bb.x0 == 8 && bb.y0 == 8) break;
            x += (8 - bb.x0) * 256;
            y += (8 - bb.y0) * 256;
        }
    } catch (const GuestFault& f) {
        std::fprintf(stderr, "sprite %08x: redraw faulted at %08x, skipped\n", h, f.pc);
        bb = Bbox{};
    }
    c = saved;
    if (bb.x1 < 0) return;
    int w = bb.x1 - bb.x0 + 1, ht = bb.y1 - bb.y0 + 1;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * ht * 3);
    for (int yy = 0; yy < ht; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            int i = (bb.y0 + yy) * 320 + bb.x0 + xx;
            uint8_t* p = &rgb[(static_cast<size_t>(yy) * w + xx) * 3];
            if (a[i] == b[i]) {  // drawn the same on both backgrounds: an opaque sprite pixel
                uint16_t v = a[i];
                p[0] = static_cast<uint8_t>((v & 31) << 3), p[1] = static_cast<uint8_t>(((v >> 5) & 31) << 3),
                p[2] = static_cast<uint8_t>(((v >> 10) & 31) << 3);
            } else {
                p[0] = 0, p[1] = 255, p[2] = 0;  // transparent: key colour
            }
        }
    uint64_t image = fnv(rgb.data(), rgb.size(), static_cast<uint64_t>(w) << 32 | static_cast<uint32_t>(ht));
    // Where the image sits relative to the anchor (s7, t8) at scale 1, in pixels.
    int ox = bb.x0 - drawn_x / 256, oy = bb.y0 - drawn_y / 256;
    if (FILE* f = std::fopen((g_dir + "/keys.txt").c_str(), "a")) {
        std::fprintf(f, "%016llx %016llx %d %d %d %d\n", static_cast<unsigned long long>(key),
                     static_cast<unsigned long long>(image), ox, oy, w, ht);
        std::fclose(f);
    }
    if (!g_seen_images.insert(image).second) return;
    char name[64];
    std::snprintf(name, sizeof name, "/%016llx.ppm", static_cast<unsigned long long>(image));
    if (FILE* f = std::fopen((g_dir + name).c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, ht);
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
    }
    if (FILE* f = std::fopen((g_dir + "/index.txt").c_str(), "a")) {
        std::fprintf(f, "%016llx header %08x size %dx%d hdr %d %d %d %d\n", static_cast<unsigned long long>(image), h,
                     w, ht, static_cast<int16_t>(hdr[0] | hdr[1] << 8), static_cast<int16_t>(hdr[2] | hdr[3] << 8),
                     static_cast<int16_t>(hdr[4] | hdr[5] << 8), static_cast<int16_t>(hdr[6] | hdr[7] << 8));
        std::fclose(f);
    }
    ++g_written;
}

}  // namespace

uint64_t sprite_key(const Context& c) {
    uint32_t h = lo32(c.r[3]);
    if (h - 0x88000000u >= c.dram.size() - 1024) return 0;
    // Same data and same palette = same image. Pixels are bytes used as zero-page
    // addresses of halfwords (0x88010f50: lbu; lh 0(reg)), so the palette is the
    // zero page's first 256 bytes; the rest of the page holds other variables.
    return fnv(&c.dram[0x90000], 0x100, fnv(&c.dram[h - 0x88000000u], 512));
}

void start_sprite_extraction(const std::string& dir) {
    g_dir = dir;
    hooks::sprite = on_sprite;
}

void stop_sprite_extraction() { hooks::sprite = nullptr; }

unsigned extracted_sprites() { return g_written; }

}  // namespace ki
