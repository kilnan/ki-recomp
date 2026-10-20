#include "ki_hd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    std::ifstream pf(dir + "/panoramas.txt");
    while (std::getline(pf, line)) {
        std::istringstream ls(line);
        std::string key;
        Panorama pano;
        if (!(ls >> key >> pano.w >> pano.stride >> pano.h)) continue;
        pano.file = dir + "/pano_" + key;
        panoramas_[std::stoull(key, nullptr, 16)] = pano;
    }
    std::ifstream pr(dir + "/prerendered.txt");
    while (std::getline(pr, line)) {
        std::istringstream ls(line);
        std::string key;
        Prerendered stage;
        if (!(ls >> key >> stage.frames) || stage.frames <= 0) continue;
        stage.prefix = dir + "/pre_" + key + "_";
        stage.hd.resize(stage.frames);
        stage.tried.assign(stage.frames, false);
        prerendered_[std::stoull(key, nullptr, 16)] = stage;
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
    return !pack_.empty() || !panoramas_.empty() || !prerendered_.empty();
}

void HdRenderer::attach() {
    g_hd = this;
    hooks::sprite = on_before;
    hooks::sprite_after = on_after;
    hooks::panorama = on_panorama;
    hooks::panorama_after = on_panorama_after;
    // Pre-rendered stages: experimental (KI_HD_PRERENDERED=1). The block shift
    // model reproduces only some camera positions; see TECHNICAL_NOTE.
    if (std::getenv("KI_HD_PRERENDERED")) {
        hooks::prerendered = on_prerendered;
        hooks::prerendered_after = on_prerendered_after;
    }
}

void HdRenderer::detach() {
    hooks::sprite = nullptr;
    hooks::sprite_after = nullptr;
    hooks::panorama = nullptr;
    hooks::panorama_after = nullptr;
    hooks::prerendered = nullptr;
    hooks::prerendered_after = nullptr;
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
    // Transparent pixels take the entry of an opaque neighbour (a few passes),
    // so the colour correction near the edge is the edge's own, not zero:
    // otherwise the original's pixel staircase shows along the outline.
    std::vector<uint8_t> e = im.entries;
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<uint8_t> next = e;
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) {
                if (e[static_cast<size_t>(y) * im.w + x] != 0xff) continue;
                static const int kN[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : kN) {
                    int nx = x + d[0], ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= im.w || ny >= im.h) continue;
                    uint8_t v = e[static_cast<size_t>(ny) * im.w + nx];
                    if (v != 0xff) {
                        next[static_cast<size_t>(y) * im.w + x] = v;
                        break;
                    }
                }
            }
        e.swap(next);
    }
    im.entries = e;
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
    bool same = true;
    for (int n = 0; n < 128 && same; ++n)
        for (int ch = 0; ch < 3; ++ch) same = same && rgb(pal, n, ch) == rgb(base.palette.data(), n, ch);
    Image& v = variants_[h];
    v = base;
    v.entries.clear();
    v.palette.clear();
    if (same) return &v;
    // First a colour transform common to all entries (a stage's lighting, a
    // flash): new = M * old + t, fitted by least squares over the entries the
    // image uses, applied to the HD pixels themselves. What it does not explain
    // (a costume) is added per entry as before.
    unsigned used[128] = {};
    for (uint8_t e : base.entries)
        if (e != 0xff) ++used[e & 0x7f];
    double ata[4][4] = {}, atb[4][3] = {};
    for (int n = 0; n < 128; ++n) {
        if (!used[n]) continue;
        double x[4] = {double(rgb(base.palette.data(), n, 0)), double(rgb(base.palette.data(), n, 1)),
                       double(rgb(base.palette.data(), n, 2)), 1.0};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) ata[i][j] += used[n] * x[i] * x[j];
            for (int ch = 0; ch < 3; ++ch) atb[i][ch] += used[n] * x[i] * rgb(pal, n, ch);
        }
    }
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};  // identity if the fit is singular
    {
        double a[4][7];
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) a[i][j] = ata[i][j] + (i == j ? 1e-3 : 0);  // ridge, for flat images
            for (int ch = 0; ch < 3; ++ch) a[i][4 + ch] = atb[i][ch];
        }
        bool ok = true;
        for (int col = 0; col < 4 && ok; ++col) {
            int piv = col;
            for (int i = col + 1; i < 4; ++i)
                if (std::fabs(a[i][col]) > std::fabs(a[piv][col])) piv = i;
            if (std::fabs(a[piv][col]) < 1e-9) ok = false;
            else {
                for (int j = 0; j < 7; ++j) std::swap(a[col][j], a[piv][j]);
                for (int i = 0; i < 4; ++i)
                    if (i != col) {
                        double f = a[i][col] / a[col][col];
                        for (int j = 0; j < 7; ++j) a[i][j] -= f * a[col][j];
                    }
            }
        }
        if (ok)
            for (int ch = 0; ch < 3; ++ch)
                for (int i = 0; i < 4; ++i) m[ch][i] = a[i][4 + ch] / a[i][i];
    }
    auto apply = [&](int ch, double r, double g, double b) { return m[ch][0] * r + m[ch][1] * g + m[ch][2] * b + m[ch][3]; };
    int delta[128][3];
    for (int n = 0; n < 128; ++n)
        for (int ch = 0; ch < 3; ++ch)
            delta[n][ch] = static_cast<int>(std::lround(
                rgb(pal, n, ch) - apply(ch, rgb(base.palette.data(), n, 0), rgb(base.palette.data(), n, 1),
                                        rgb(base.palette.data(), n, 2))));
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
            double r0 = (px >> 16) & 255, g0 = (px >> 8) & 255, b0 = px & 255;
            for (int ch = 0; ch < 3; ++ch) {
                float d = (d_at(x0, y0, ch) * (1 - wx) + d_at(x0 + 1, y0, ch) * wx) * (1 - wy) +
                          (d_at(x0, y0 + 1, ch) * (1 - wx) + d_at(x0 + 1, y0 + 1, ch) * wx) * wy;
                int shift = 16 - 8 * ch;  // BGR555 bits 0-4 are red (ARGB bits 16-23), as argb_of
                int val = static_cast<int>(std::lround(apply(ch, r0, g0, b0) + d));
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

void HdRenderer::invalidate(const Context& c, uint32_t bank) {
    // A pixel the game redraws with the same value as before would otherwise
    // keep the HD image of the last frame (trails, fragments).
    Bank& b = bank_of(bank);
    const uint16_t* lo = reinterpret_cast<const uint16_t*>(&c.sram[bank]);
    for (const Rect& r : b.hd_rects)
        for (int y = r.y0; y <= r.y1; ++y)
            for (int x = r.x0; x <= r.x1; ++x) b.shadow[y * 320 + x] = static_cast<uint16_t>(~lo[y * 320 + x]);
    b.hd_rects.clear();
}

void HdRenderer::draw(Bank& b, const Image& im, int x0, int y0, int x1, int y1, bool mirror) {
    b.hd_rects.push_back({std::clamp(x0, 0, 319), std::clamp(y0, 0, 239), std::clamp(x1, 0, 319), std::clamp(y1, 0, 239)});
    if (b.hd_rects.back().x0 > b.hd_rects.back().x1 || b.hd_rects.back().y0 > b.hd_rects.back().y1) b.hd_rects.pop_back();
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
    if (r.bank_ != r.last_bank_) {  // the first sprite of a new frame in this bank
        r.invalidate(c, r.bank_);
        r.last_bank_ = r.bank_;
    }
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

bool HdRenderer::load_panorama(Panorama& p) {
    p.loaded = true;
    std::ifstream raw(p.file + ".raw", std::ios::binary);
    p.original.resize(static_cast<size_t>(p.stride) * p.h);
    if (!raw.read(reinterpret_cast<char*>(p.original.data()), static_cast<std::streamsize>(p.original.size() * 2))) {
        p.original.clear();
        return false;
    }
    Image im;
    im.file = p.file + ".ki4";
    if (!load(im) || im.missing || im.iw != p.w * kScale || im.ih != p.h * kScale) {
        p.original.clear();
        return false;
    }
    p.hd = std::move(im.argb);
    return true;
}

void HdRenderer::on_panorama(Context& c) {
    HdRenderer& r = *g_hd;
    const int regs[6] = {2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 6; ++i) r.pano_regs_[i] = lo32(c.r[regs[i]]);
}

void HdRenderer::on_panorama_after(Context& c) {
    HdRenderer& r = *g_hd;
    uint32_t dst = r.pano_regs_[0], src = r.pano_regs_[1];
    int col0 = static_cast<int32_t>(r.pano_regs_[2]), row0 = static_cast<int32_t>(r.pano_regs_[3]);
    int w = static_cast<int32_t>(r.pano_regs_[4]), rows = static_cast<int32_t>(r.pano_regs_[5]);
    uint32_t bank = dst & 0x1fffffffu;  // physical: the framebuffer's SRAM offset
    if ((bank != 0x30000 && bank != 0x58000) || w <= 0 || rows <= 0 || src - 0x88000000u >= c.dram.size()) return;
    const int stride = w + 4;
    uint32_t s0 = src - 0x88000000u;
    if (s0 + static_cast<uint64_t>(stride) * 2 > c.dram.size()) return;
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < stride * 2; ++i) key = (key ^ c.dram[s0 + i]) * 1099511628211ull;
    auto it = r.panoramas_.find(key);
    static const bool debug = std::getenv("KI_HD_DEBUG") != nullptr;
    if (debug) {
        static int shown = 0;
        if (shown++ < 4)
            std::fprintf(stderr, "panorama copy: src %08x col %d row %d w %d rows %d key %016llx%s\n", src, col0, row0, w,
                         rows, static_cast<unsigned long long>(key), it == r.panoramas_.end() ? " (not in pack)" : "");
    }
    if (it == r.panoramas_.end()) return;
    Panorama& p = it->second;
    if (!p.loaded) r.load_panorama(p);
    if (p.original.empty() || p.w != w || p.stride != stride) return;
    if (std::getenv("KI_HD_DEBUG")) {  // which column shift makes the copied rows match the source
        static int shown = 0;
        const uint16_t* fb = reinterpret_cast<const uint16_t*>(&c.sram[bank]);
        int row = std::max(0, -row0) + 2, y = row0 + row;
        if (shown++ < 4 && y >= 0 && y < 240 && row < p.h) {
            for (int shift = -8; shift <= 8; ++shift) {
                int ok = 0;
                for (int x = 0; x < 320; ++x)
                    ok += fb[y * 320 + x] == p.original[static_cast<size_t>(row) * stride + 4 + ((((col0 + x + shift) % w) + w) % w)];
                if (ok > 200) std::fprintf(stderr, "  row %d shift %d: %d/320 match\n", row, shift, ok);
            }
        }
    }
    // Synced first, so the pixels the copy changed are the only ones left.
    Bank& b = r.bank_of(bank);
    const uint16_t* lo = reinterpret_cast<const uint16_t*>(&c.sram[bank]);
    col0 = ((col0 % w) + w) % w;
    for (int row = 0; row < rows && row < p.h; ++row) {
        int y = row0 + row;
        if (y < 0 || y >= 240) continue;
        for (int x = 0; x < 320; ++x) {
            int col = (col0 + x) % w;
            uint16_t v = lo[y * 320 + x];
            // Where the game drew over its panorama (the billboards), it stays
            // to the game: upscaled from its own pixels by the next sync.
            // Each row starts with 4 pixels of margin, then the w columns that wrap around.
            if (v != p.original[static_cast<size_t>(row) * stride + 4 + col]) {
                ++r.pano_kept_;
                continue;
            }
            ++r.pano_hd_;
            b.shadow[y * 320 + x] = v;
            for (int sy = 0; sy < kScale; ++sy)
                std::memcpy(&b.hd[static_cast<size_t>(y * kScale + sy) * kW + x * kScale],
                            &p.hd[static_cast<size_t>(row * kScale + sy) * p.w * kScale + col * kScale], kScale * 4);
        }
    }
}

constexpr int kNoMatch = 1 << 20;

void HdRenderer::on_prerendered(Context& c) { g_hd->pre_dest_ = lo32(c.r[26]); }  // k0: the framebuffer

void HdRenderer::on_prerendered_after(Context& c) {
    // The stage file is loaded at 0x885b1000; its first word is the offset of
    // the frames (320x240, 16-bit, one after the other).
    HdRenderer& r = *g_hd;
    if (r.prerendered_.empty()) return;
    uint32_t bank = r.pre_dest_ & 0x1fffffffu;
    if (bank != 0x30000 && bank != 0x58000) return;
    constexpr uint32_t kStage = 0x5b1000;
    constexpr int kFrame = 320 * 240;
    uint32_t first = c.dram[kStage] | c.dram[kStage + 1] << 8 | c.dram[kStage + 2] << 16 |
                     static_cast<uint32_t>(c.dram[kStage + 3]) << 24;
    // After loading, the game has made the offset an address.
    if (first - 0x88000000u < 0x800000u) first -= 0x88000000u + kStage;
    if (first >= 0x100000u) return;
    const uint32_t frames0 = kStage + first;
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < 320 * 2 * 8; ++i) key = (key ^ c.dram[frames0 + i]) * 1099511628211ull;
    auto it = r.prerendered_.find(key);
    if (std::getenv("KI_HD_DEBUG")) {
        static int shown = 0;
        if (shown++ < 3)
            std::fprintf(stderr, "pre-rendered stage: frames at %08x key %016llx%s\n", 0x88000000u + frames0,
                         static_cast<unsigned long long>(key), it == r.prerendered_.end() ? " (not in pack)" : "");
    }
    if (it == r.prerendered_.end()) return;
    Prerendered& st = it->second;
    if (frames0 + static_cast<uint64_t>(st.frames) * kFrame * 2 > c.dram.size()) return;
    const uint16_t* fb = reinterpret_cast<const uint16_t*>(&c.sram[bank]);
    auto src = [&](int f) { return reinterpret_cast<const uint16_t*>(&c.dram[frames0 + static_cast<uint32_t>(f) * kFrame * 2]); };
    // Error of block (bx, by) drawn from frame f shifted by s/8 pixel (the
    // renderer mixes two neighbouring columns in eighths).
    auto block_error = [&](int f, int bx, int by, int s8) {
        const uint16_t* sp = src(f);
        int xi = s8 >> 3, w = s8 & 7, err = 0;
        for (int y = by * 8; y < by * 8 + 8; ++y)
            for (int x = bx * 8; x < bx * 8 + 8; ++x) {
                int x0 = std::clamp(x + xi, 0, 319), x1 = std::clamp(x + xi + 1, 0, 319);
                uint16_t a = sp[y * 320 + x0], b = sp[y * 320 + x1], v = fb[y * 320 + x];
                for (int sh = 0; sh < 15; sh += 5) {
                    int pa = (a >> sh) & 31, pb = (b >> sh) & 31, pv = (v >> sh) & 31;
                    err += std::abs((pa * (8 - w) + pb * w) - pv * 8);
                }
            }
        return err;
    };
    // The frame: the best one over a few blocks (the renderer uses one per call).
    int frame = -1, best_total = 1 << 30;
    for (int f = 0; f < st.frames; ++f) {
        int total = 0;
        for (int i = 0; i < 12; ++i) {
            int bx = 4 + (i % 4) * 10, by = 3 + (i / 4) * 9, best = 1 << 30;
            for (int s8 = -40; s8 <= 40; s8 += 2) best = std::min(best, block_error(f, bx, by, s8));
            total += best;
        }
        if (total < best_total) best_total = total, frame = f;
    }
    if (frame < 0) return;
    if (!st.tried[frame]) {
        st.tried[frame] = true;
        Image im;
        im.file = st.prefix + std::to_string(frame) + ".ki4";
        if (r.load(im), !im.missing && im.iw == 320 * kScale && im.ih == 240 * kScale) st.hd[frame] = std::move(im.argb);
    }
    if (st.hd[frame].empty()) return;
    const std::vector<uint32_t>& hd = st.hd[frame];
    // First every block's shift; the HD frame is used only if nearly all blocks
    // match (between two camera angles the renderer does something this model
    // does not cover, and a mix of matched and unmatched blocks looks broken).
    int shifts[30][40];
    int matched = 0, last_shift = 0;
    for (int by = 0; by < 30; ++by)
        for (int bx = 0; bx < 40; ++bx) {
            int best = 1 << 30, best_s = 0;
            for (int s8 = -40; s8 <= 40; ++s8) {
                int e = block_error(frame, bx, by, s8);
                if (e < best || (e == best && std::abs(s8 - last_shift) < std::abs(best_s - last_shift))) best = e, best_s = s8;
                if (best == 0) break;
            }
            // Mostly exact; a few rounding differences per block are fine.
            static const int limit = std::getenv("KI_PRE_LIMIT") ? std::atoi(std::getenv("KI_PRE_LIMIT")) : 64 * 3 * 8 / 2;
            shifts[by][bx] = best <= limit ? best_s : kNoMatch;
            if (shifts[by][bx] != kNoMatch) ++matched, last_shift = best_s;
        }
    if (std::getenv("KI_HD_DEBUG")) {
        static int calls = 0;
        if (++calls % 50 == 0) std::fprintf(stderr, "pre-rendered: frame %d, %d/1200 blocks match\n", frame, matched);
    }
    if (matched < 1200 * 9 / 10) {
        r.pre_blocks_kept_ += 1200;
        return;
    }
    Bank& b = r.bank_of(bank);
    for (int by = 0; by < 30; ++by)
        for (int bx = 0; bx < 40; ++bx) {
            int best_s = shifts[by][bx];
            if (best_s == kNoMatch) {
                // Inside a matched frame: the neighbour's shift (same depth, usually a flat area).
                best_s = bx > 0 && shifts[by][bx - 1] != kNoMatch ? shifts[by][bx - 1] : 0;
            }
            ++r.pre_blocks_hd_;
            // In HD the shift is 4x finer: s/8 original pixel = s/2 HD pixel.
            for (int y = 0; y < 8 * kScale; ++y) {
                int hy = by * 8 * kScale + y;
                const uint32_t* row = &hd[static_cast<size_t>(hy) * 320 * kScale];
                uint32_t* out = &b.hd[static_cast<size_t>(hy) * kW + bx * 8 * kScale];
                for (int x = 0; x < 8 * kScale; ++x) {
                    int pos2 = (bx * 8 * kScale + x) * 2 + best_s;  // in half HD pixels
                    int h0 = std::clamp(pos2 >> 1, 0, 320 * kScale - 1), h1 = std::clamp((pos2 >> 1) + 1, 0, 320 * kScale - 1);
                    uint32_t p0 = row[h0];
                    if (pos2 & 1) {
                        uint32_t p1 = row[h1];
                        p0 = 0xff000000u | (((p0 >> 16 & 255) + (p1 >> 16 & 255)) / 2) << 16 |
                             (((p0 >> 8 & 255) + (p1 >> 8 & 255)) / 2) << 8 | ((p0 & 255) + (p1 & 255)) / 2;
                    }
                    out[x] = p0;
                }
            }
            for (int y = by * 8; y < by * 8 + 8; ++y)
                std::memcpy(&b.shadow[y * 320 + bx * 8], fb + y * 320 + bx * 8, 16);
        }
}

const std::vector<uint32_t>& HdRenderer::frame(const Context& c, uint32_t bank) {
    sync(c, bank);
    return bank_of(bank).hd;
}

}  // namespace ki
