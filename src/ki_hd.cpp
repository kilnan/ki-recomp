#include "ki_hd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <miniz.h>

#include "ki/hooks.h"
#include "ki_sprites.h"

namespace ki {

namespace {

HdRenderer* g_hd = nullptr;

uint32_t argb_of(uint16_t v) {  // BGR555, as KinstBoard::framebuffer_rgb
    auto e = [](unsigned v5) { return (v5 << 3) | (v5 >> 2); };
    return 0xff000000u | e(v & 31) << 16 | e((v >> 5) & 31) << 8 | e((v >> 10) & 31);
}

uint16_t pixel(const Context& c, uint32_t bank, int x, int y) {
    x = std::clamp(x, 0, 319);
    y = std::clamp(y, 0, 239);
    uint32_t a = bank + (y * 320 + x) * 2;
    return static_cast<uint16_t>(c.sram[a] | c.sram[a + 1] << 8);
}

}  // namespace

bool HdRenderer::load_pack(const std::string& dir) {
    std::ifstream in(dir + "/index.txt");
    if (!in) return false;
    dir_ = dir;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key, image;
        Image im;
        if (!(ls >> key >> image >> im.ox >> im.oy >> im.w >> im.h)) continue;
        im.file = dir + "/" + image + ".ki4";
        pack_[std::stoull(key, nullptr, 16)] = im;
    }
    std::ifstream sh(dir + "/shapes.txt");
    while (std::getline(sh, line)) {
        std::istringstream ls(line);
        std::string key, image;
        Image im;
        if (!(ls >> key >> image >> im.ox >> im.oy >> im.w >> im.h)) continue;
        im.file = dir + "/" + image + ".ki4";
        im.kix_file = dir + "/" + image + ".kix";
        shapes_[std::stoull(key, nullptr, 16)] = im;
    }
    return !pack_.empty();
}

void HdRenderer::attach() {
    g_hd = this;
    hooks::sprite = on_before;
    hooks::sprite_after = on_after;
}

void HdRenderer::detach() {
    hooks::sprite = nullptr;
    hooks::sprite_after = nullptr;
    g_hd = nullptr;
}

bool HdRenderer::load(Image& im) {
    // .ki4: "KIHD", u32 width, u32 height, u32 deflate size, zlib data of ARGB8888 pixels (LE)
    im.loaded = true;
    std::ifstream f(im.file, std::ios::binary);
    char magic[4];
    uint32_t hdr[3];
    if (!f.read(magic, 4) || std::memcmp(magic, "KIHD", 4) || !f.read(reinterpret_cast<char*>(hdr), sizeof hdr)) {
        im.missing = true;
        return false;
    }
    std::vector<unsigned char> z(hdr[2]);
    f.read(reinterpret_cast<char*>(z.data()), hdr[2]);
    im.iw = static_cast<int>(hdr[0]);
    im.ih = static_cast<int>(hdr[1]);
    im.argb.resize(static_cast<size_t>(im.iw) * im.ih);
    mz_ulong n = static_cast<mz_ulong>(im.argb.size() * 4);
    if (mz_uncompress(reinterpret_cast<unsigned char*>(im.argb.data()), &n, z.data(), hdr[2]) != MZ_OK) {
        im.missing = true;
        return false;
    }
    return true;
}

bool HdRenderer::load_kix(Image& im) {
    im.kix_loaded = true;
    std::ifstream f(im.kix_file, std::ios::binary);
    char magic[4];
    uint32_t dims[2];
    if (!f.read(magic, 4) || std::memcmp(magic, "KIIX", 4) || !f.read(reinterpret_cast<char*>(dims), sizeof dims))
        return false;
    im.palette.resize(0x100);
    im.entries.resize(static_cast<size_t>(dims[0]) * dims[1]);
    if (!f.read(reinterpret_cast<char*>(im.palette.data()), 0x100) ||
        !f.read(reinterpret_cast<char*>(im.entries.data()), static_cast<std::streamsize>(im.entries.size())) ||
        static_cast<int>(dims[0]) != im.w || static_cast<int>(dims[1]) != im.h) {
        im.entries.clear();
        return false;
    }
    return true;
}

HdRenderer::Image* HdRenderer::recoloured(const Context& c, Image& base) {
    if (!base.loaded) load(base);
    if (!base.kix_loaded) load_kix(base);
    if (base.missing || base.entries.empty()) return nullptr;
    const uint8_t* pal = &c.dram[0x90000];
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 0x100; ++i) h = (h ^ pal[i]) * 1099511628211ull;
    h ^= std::hash<std::string>{}(base.file);
    auto it = variants_.find(h);
    if (it != variants_.end()) return &it->second;
    // Per palette entry, the colour difference between now and the original;
    // over the image, upscaled bilinearly and added to the HD colours. Inside
    // an area of one entry this is the exact new colour; the detail stays.
    auto rgb = [](const uint8_t* p, int n, int ch) {
        uint16_t v = static_cast<uint16_t>(p[2 * n] | p[2 * n + 1] << 8);
        int c5 = (v >> (5 * ch)) & 31;
        return (c5 << 3) | (c5 >> 2);
    };
    int delta[128][3];
    bool same = true;
    for (int n = 0; n < 128; ++n)
        for (int ch = 0; ch < 3; ++ch) {
            delta[n][ch] = rgb(pal, n, ch) - rgb(base.palette.data(), n, ch);
            same = same && delta[n][ch] == 0;
        }
    Image& v = variants_[h];
    v = base;
    v.entries.clear();
    v.palette.clear();
    if (same) return &v;
    const int w = base.w, ht = base.h;
    auto d_at = [&](int x, int y, int ch) {
        x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, ht - 1);
        uint8_t e = base.entries[static_cast<size_t>(y) * w + x];
        return e == 0xff ? 0 : delta[e & 0x7f][ch];
    };
    for (int y = 0; y < v.ih; ++y) {
        float fy = (y + 0.5f) * ht / v.ih - 0.5f;
        int y0 = static_cast<int>(std::floor(fy));
        float wy = fy - y0;
        for (int x = 0; x < v.iw; ++x) {
            uint32_t& px = v.argb[static_cast<size_t>(y) * v.iw + x];
            if (!(px >> 24)) continue;
            float fx = (x + 0.5f) * w / v.iw - 0.5f;
            int x0 = static_cast<int>(std::floor(fx));
            float wx = fx - x0;
            uint32_t out = px & 0xff000000u;
            for (int ch = 0; ch < 3; ++ch) {
                float d = (d_at(x0, y0, ch) * (1 - wx) + d_at(x0 + 1, y0, ch) * wx) * (1 - wy) +
                          (d_at(x0, y0 + 1, ch) * (1 - wx) + d_at(x0 + 1, y0 + 1, ch) * wx) * wy;
                int shift = 16 - 8 * ch;  // BGR555 bits 0-4 are red (ARGB bits 16-23), as argb_of
                int val = static_cast<int>((px >> shift) & 255) + static_cast<int>(std::lround(d));
                out |= static_cast<uint32_t>(std::clamp(val, 0, 255)) << shift;
            }
            px = out;
        }
    }
    return &v;
}

void HdRenderer::sync(const Context& c, uint32_t bank) {
    Bank& b = bank_of(bank);
    const uint16_t* lo = reinterpret_cast<const uint16_t*>(&c.sram[bank]);  // little-endian host
    for (int y = 0; y < 240; ++y) {
        if (!std::memcmp(lo + y * 320, &b.shadow[y * 320], 640)) continue;  // most rows are unchanged
        for (int x = 0; x < 320; ++x) {
            uint16_t v = lo[y * 320 + x];
            uint16_t& s = b.shadow[y * 320 + x];
            if (v == s) continue;
            s = v;
            // Bilinear 4x: subpixel u of pixel x samples x-1/x (u < 2) or x/x+1,
            // with weights in eighths.
            static const int kNear[4] = {-1, -1, 1, 1}, kWfar[4] = {3, 1, 1, 3};
            int nb[3][3][3];  // [dy+1][dx+1][channel]
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    uint16_t p = lo[std::clamp(y + dy, 0, 239) * 320 + std::clamp(x + dx, 0, 319)];
                    nb[dy + 1][dx + 1][0] = (p & 31) << 3 | (p & 31) >> 2;
                    nb[dy + 1][dx + 1][1] = ((p >> 5) & 31) << 3 | ((p >> 5) & 31) >> 2;
                    nb[dy + 1][dx + 1][2] = ((p >> 10) & 31) << 3 | ((p >> 10) & 31) >> 2;
                }
            for (int v4 = 0; v4 < kScale; ++v4) {
                int oy = kNear[v4], wy = kWfar[v4];
                uint32_t* row = &b.hd[(y * kScale + v4) * kW + x * kScale];
                for (int u4 = 0; u4 < kScale; ++u4) {
                    int ox = kNear[u4], wx = kWfar[u4];
                    int c00[3], out[3];
                    for (int ch = 0; ch < 3; ++ch) {
                        c00[ch] = nb[1][1][ch] * (8 - wx) * (8 - wy) + nb[1][1 + ox][ch] * wx * (8 - wy) +
                                  nb[1 + oy][1][ch] * (8 - wx) * wy + nb[1 + oy][1 + ox][ch] * wx * wy;
                        out[ch] = (c00[ch] + 32) >> 6;
                    }
                    row[u4] = 0xff000000u | static_cast<uint32_t>(out[0]) << 16 | static_cast<uint32_t>(out[1]) << 8 |
                              static_cast<uint32_t>(out[2]);
                }
            }
        }
    }
}

void HdRenderer::draw(Bank& b, const Image& im, int x0, int y0, int x1, int y1, bool mirror) {
    // Maps the HD image onto original pixels x0..x1, y0..y1 (inclusive).
    int dx0 = x0 * kScale, dy0 = y0 * kScale, dw = (x1 - x0 + 1) * kScale, dh = (y1 - y0 + 1) * kScale;
    for (int y = 0; y < dh; ++y) {
        int ty = dy0 + y;
        if (ty < 0 || ty >= kH) continue;
        int sy = std::min(im.ih - 1, y * im.ih / dh);
        for (int x = 0; x < dw; ++x) {
            int tx = dx0 + x;
            if (tx < 0 || tx >= kW) continue;
            int sx = std::min(im.iw - 1, x * im.iw / dw);
            if (mirror) sx = im.iw - 1 - sx;
            uint32_t s = im.argb[static_cast<size_t>(sy) * im.iw + sx];
            unsigned a = s >> 24;
            if (!a) continue;
            uint32_t& d = b.hd[static_cast<size_t>(ty) * kW + tx];
            if (a == 255) {
                d = s;
                continue;
            }
            uint32_t out = 0xff000000u;
            for (int sh = 0; sh < 24; sh += 8)
                out |= ((((s >> sh) & 255) * a + ((d >> sh) & 255) * (255 - a)) / 255) << sh;
            d = out;
        }
    }
}

void HdRenderer::on_before(Context& c) {
    HdRenderer& r = *g_hd;
    r.bank_ = (c.dram[0x86200] & 1) ? 0x58000 : 0x30000;  // the blitter's target (0x88010900)
    r.sync(c, r.bank_);
    r.key_ = sprite_key(c);
    r.shape_ = sprite_shape_key(c);
    r.header_ = lo32(c.r[3]);
    r.s7_ = static_cast<int32_t>(lo32(c.r[23]));
    r.t8_ = static_cast<int32_t>(lo32(c.r[24]));
    r.t1_ = static_cast<int32_t>(lo32(c.r[9]));
    r.t3_ = static_cast<int32_t>(lo32(c.r[11]));
    r.t4_ = static_cast<int32_t>(lo32(c.r[12]));
    r.mode_ = static_cast<int32_t>(lo32(c.r[22]));
}

void HdRenderer::on_after(Context& c) {
    HdRenderer& r = *g_hd;
    if (r.mode_ == 48) return;  // shadows are drawn normally; picked up by the next sync
    auto it = r.pack_.find(r.key_);
    Image* found = nullptr;
    if (it != r.pack_.end()) {
        found = &it->second;
    } else if (r.mode_ == 0) {  // another palette (costume, flash, lighting): recolour the shape's image
        auto sh = r.shapes_.find(r.shape_);
        if (sh != r.shapes_.end()) found = r.recoloured(c, sh->second);
    }
    if (!found) {
        ++r.misses_[static_cast<uint64_t>(r.header_) << 32 | static_cast<uint32_t>(r.mode_)];
        return;
    }
    Image& im = *found;
    if (!im.loaded) r.load(im);
    if (im.missing) return;
    // Where the whole sprite goes (it may be clipped): see TECHNICAL_NOTE "Sprite blitter".
    double sx = r.t1_ / 4096.0, sy = r.t3_ / 4096.0, X = r.s7_ / 256.0, Y = r.t8_ / 256.0;
    bool mirror = r.t4_ < 0;
    double left = mirror ? X - (im.ox + 1) * sx + 1 - im.w * sx : X + (im.ox + 1) * sx - 1;
    double top = Y + im.oy * sy;
    int x0 = static_cast<int>(std::lround(left)), y0 = static_cast<int>(std::lround(top));
    int x1 = x0 + static_cast<int>(std::lround(im.w * sx)) - 1, y1 = y0 + static_cast<int>(std::lround(im.h * sy)) - 1;
    // The pixels this call changed (everything else was synced just before it),
    // looked for around the predicted rectangle.
    Bank& b = r.bank_of(r.bank_);
    const uint16_t* lo = reinterpret_cast<const uint16_t*>(&c.sram[r.bank_]);
    int fx0 = 320, fy0 = 240, fx1 = -1, fy1 = -1;
    for (int y = std::max(0, y0 - 4); y <= std::min(239, y1 + 4); ++y)
        for (int x = std::max(0, x0 - 4); x <= std::min(319, x1 + 4); ++x)
            if (lo[y * 320 + x] != b.shadow[y * 320 + x]) {
                fx0 = std::min(fx0, x), fy0 = std::min(fy0, y), fx1 = std::max(fx1, x), fy1 = std::max(fy1, y);
            }
    if (fx1 < 0) return;
    // Unclipped: the footprint itself is exact.
    bool clipped = fx0 == 0 || fy0 == 0 || fx1 == 319 || fy1 == 239;
    if (!clipped && std::abs((fx1 - fx0) - (x1 - x0)) <= 2 && std::abs((fy1 - fy0) - (y1 - y0)) <= 2)
        x0 = fx0, y0 = fy0, x1 = fx1, y1 = fy1;
    // The pixels the call drew are taken as synced without being upscaled:
    // under the HD image, and around its soft edge, stays what was there
    // before the sprite, not the original sprite enlarged.
    for (int y = fy0; y <= fy1; ++y)
        std::memcpy(&b.shadow[y * 320 + fx0], lo + y * 320 + fx0, (fx1 - fx0 + 1) * 2);
    r.draw(b, im, x0, y0, x1, y1, mirror);
    ++r.hd_drawn_;
}

void HdRenderer::print_misses(FILE* f) const {
    std::vector<std::pair<uint64_t, unsigned>> v(misses_.begin(), misses_.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(f, "HD misses (%zu header/mode pairs): header mode calls\n", v.size());
    for (const auto& [k, n] : v)
        std::fprintf(f, "  %08x %3d %u\n", static_cast<uint32_t>(k >> 32), static_cast<int32_t>(static_cast<uint32_t>(k)), n);
}

const std::vector<uint32_t>& HdRenderer::frame(const Context& c, uint32_t bank) {
    sync(c, bank);
    return bank_of(bank).hd;
}

}  // namespace ki
