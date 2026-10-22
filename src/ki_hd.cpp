#include "ki_hd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
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
    std::ifstream sc(dir + "/screens.txt");
    while (std::getline(sc, line)) {
        std::istringstream ls(line);
        std::string key;
        if (ls >> key) screens_[std::stoull(key, nullptr, 16)] = dir + "/screen_" + key + ".ki4";
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
    return !pack_.empty() || !panoramas_.empty() || !prerendered_.empty() || !screens_.empty();
}

void HdRenderer::attach() {
    g_hd = this;
    hooks::sprite = on_before;
    hooks::sprite_after = on_after;
    hooks::panorama = on_panorama;
    hooks::panorama_after = on_panorama_after;
    hooks::prerendered_setup = on_prerendered_setup_before;
    hooks::prerendered_setup_after = on_prerendered_setup;
    hooks::screen_save_after = on_screen_save;
    hooks::screen_restore_after = on_screen_restore;
    // Pre-rendered stages (off with KI_HD_PRERENDERED=0); see TECHNICAL_NOTE.
    const char* pre = std::getenv("KI_HD_PRERENDERED");
    if (!pre || std::strcmp(pre, "0") != 0) {
        hooks::prerendered = on_prerendered;
        hooks::prerendered_after = on_prerendered_after;
    }
}

void HdRenderer::detach() {
    hooks::sprite = nullptr;
    hooks::sprite_after = nullptr;
    hooks::panorama = nullptr;
    hooks::panorama_after = nullptr;
    hooks::prerendered_setup = nullptr;
    hooks::prerendered_setup_after = nullptr;
    hooks::screen_save_after = nullptr;
    hooks::screen_restore_after = nullptr;
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
    // Per HD pixel: of the (up to 4) original pixels around it, the entries
    // whose original colour is closest to the HD pixel's colour decide the
    // correction (weights by colour distance). The correction then follows the
    // HD detail instead of the original pixel grid, which an interpolated
    // correction map would show again when every entry changes (a tinted stage).
    double bc[128][3], dv[128][3];
    for (int n = 0; n < 128; ++n)
        for (int ch = 0; ch < 3; ++ch) {
            bc[n][ch] = rgb(base.palette.data(), n, ch);
            dv[n][ch] = rgb(pal, n, ch) - bc[n][ch];
        }
    (void)delta;
    auto entry = [&](int x, int y) {
        x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, ht - 1);
        uint8_t e = base.entries[static_cast<size_t>(y) * w + x];
        return e == 0xff ? -1 : e & 0x7f;
    };
    for (int y = 0; y < v.ih; ++y) {
        float fy = (y + 0.5f) * ht / v.ih - 0.5f;
        int y0 = static_cast<int>(std::floor(fy));
        for (int x = 0; x < v.iw; ++x) {
            uint32_t& px = v.argb[static_cast<size_t>(y) * v.iw + x];
            if (!(px >> 24)) continue;
            float fx = (x + 0.5f) * w / v.iw - 0.5f;
            int x0 = static_cast<int>(std::floor(fx));
            double c0[3] = {double((px >> 16) & 255), double((px >> 8) & 255), double(px & 255)};
            double d[3] = {0, 0, 0}, wsum = 0;
            const int es[4] = {entry(x0, y0), entry(x0 + 1, y0), entry(x0, y0 + 1), entry(x0 + 1, y0 + 1)};
            for (int e : es) {
                if (e < 0) continue;
                double dist = 0;
                for (int ch = 0; ch < 3; ++ch) dist += (c0[ch] - bc[e][ch]) * (c0[ch] - bc[e][ch]);
                double wt = std::exp(-dist / (2 * 20.0 * 20.0)) + 1e-6;
                for (int ch = 0; ch < 3; ++ch) d[ch] += wt * dv[e][ch];
                wsum += wt;
            }
            if (wsum <= 0) continue;
            uint32_t out = px & 0xff000000u;
            for (int ch = 0; ch < 3; ++ch) {
                int val = static_cast<int>(std::lround(c0[ch] + d[ch] / wsum));
                out |= static_cast<uint32_t>(std::clamp(val, 0, 255)) << (16 - 8 * ch);
            }
            px = out;
        }
    }
    return &v;
}

void HdRenderer::upscale_pixel(Bank& b, const uint16_t* lo, int x, int y) {
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
            int out[3];
            for (int ch = 0; ch < 3; ++ch)
                out[ch] = (nb[1][1][ch] * (8 - wx) * (8 - wy) + nb[1][1 + ox][ch] * wx * (8 - wy) +
                           nb[1 + oy][1][ch] * (8 - wx) * wy + nb[1 + oy][1 + ox][ch] * wx * wy + 32) >> 6;
            row[u4] = 0xff000000u | static_cast<uint32_t>(out[0]) << 16 | static_cast<uint32_t>(out[1]) << 8 |
                      static_cast<uint32_t>(out[2]);
        }
    }
    b.owned[y * 320 + x] = 0;
}

void HdRenderer::sync(const Context& c, uint32_t bank) {
    Bank& b = bank_of(bank);
    const uint16_t* lo = reinterpret_cast<const uint16_t*>(&c.sram[bank]);  // little-endian host
    // A changed pixel's own block, and the blocks of its neighbours, which
    // interpolate towards it (left stale they draw thin outlines where
    // something moved); not neighbours an HD image owns.
    std::vector<uint8_t>& todo = scratch_;
    todo.assign(320 * 240, 0);
    bool any = false;
    for (int y = 0; y < 240; ++y) {
        if (!std::memcmp(lo + y * 320, &b.shadow[y * 320], 640)) continue;  // most rows are unchanged
        for (int x = 0; x < 320; ++x) {
            uint16_t v = lo[y * 320 + x];
            uint16_t& s = b.shadow[y * 320 + x];
            if (v == s) continue;
            s = v;
            any = true;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= 320 || ny >= 240) continue;
                    if ((dx || dy) && b.owned[ny * 320 + nx]) continue;
                    todo[ny * 320 + nx] = 1;
                }
        }
    }
    if (!any) return;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
            if (todo[y * 320 + x]) upscale_pixel(b, lo, x, y);
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
    // Maps the HD image onto original pixels x0..x1, y0..y1 (inclusive),
    // bilinearly: the game often draws fighters enlarged (the camera zooms in).
    int dx0 = x0 * kScale, dy0 = y0 * kScale, dw = (x1 - x0 + 1) * kScale, dh = (y1 - y0 + 1) * kScale;
    const int iw = im.iw, ih = im.ih;
    auto at = [&](int x, int y) { return im.argb[static_cast<size_t>(y) * iw + x]; };
    for (int y = 0; y < dh; ++y) {
        int ty = dy0 + y;
        if (ty < 0 || ty >= kH) continue;
        // Source position in 1/256 pixel.
        int fy = std::max(0, ((2 * y + 1) * ih * 128) / dh - 128);
        int sy0 = std::min(ih - 1, fy >> 8), sy1 = std::min(ih - 1, sy0 + 1), wy = fy & 255;
        for (int x = 0; x < dw; ++x) {
            int tx = dx0 + x;
            if (tx < 0 || tx >= kW) continue;
            int fx = std::max(0, ((2 * x + 1) * iw * 128) / dw - 128);
            int sx0 = std::min(iw - 1, fx >> 8), sx1 = std::min(iw - 1, sx0 + 1), wx = fx & 255;
            if (mirror) sx0 = iw - 1 - sx0, sx1 = iw - 1 - sx1;
            uint32_t p00 = at(sx0, sy0), p01 = at(sx1, sy0), p10 = at(sx0, sy1), p11 = at(sx1, sy1);
            // Premultiplied by alpha, so transparent neighbours do not darken the edge.
            uint32_t acc[4] = {0, 0, 0, 0};
            const uint32_t px[4] = {p00, p01, p10, p11};
            const int wt[4] = {(256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy};
            for (int k = 0; k < 4; ++k) {
                uint32_t a = px[k] >> 24;
                acc[3] += a * wt[k];
                for (int ch = 0; ch < 3; ++ch) acc[ch] += ((px[k] >> (16 - 8 * ch)) & 255) * a * wt[k];
            }
            unsigned a = acc[3] >> 16;
            if (!a) continue;
            uint32_t& d = b.hd[static_cast<size_t>(ty) * kW + tx];
            uint32_t out = 0xff000000u;
            for (int ch = 0; ch < 3; ++ch) {
                unsigned sc = static_cast<unsigned>(acc[ch] / acc[3]);  // un-premultiplied colour
                unsigned dc = (d >> (16 - 8 * ch)) & 255;
                out |= ((sc * a + dc * (255 - a)) / 255) << (16 - 8 * ch);
            }
            d = out;
        }
    }
}

void HdRenderer::on_before(Context& c) {
    HdRenderer& r = *g_hd;
    r.bank_ = (c.dram[0x86200] & 1) ? 0x58000 : 0x30000;  // the blitter's target (0x88010900)
    // The first sprite of a new frame in this bank (also when the game keeps
    // drawing into the same one, as on the title screen): what HD images
    // covered before is upscaled again from the game's pixels.
    if (r.bank_ != r.last_bank_ || r.epoch_ != r.last_epoch_) {
        r.invalidate(c, r.bank_);
        r.last_bank_ = r.bank_;
        r.last_epoch_ = r.epoch_;
    }
    r.sync(c, r.bank_);
    // The other one too: the footprint is then looked for in both, as on some
    // stages the call does not draw where 0x88086200 says at this point.
    r.sync(c, r.bank_ == 0x30000 ? 0x58000 : 0x30000);
    r.key_ = sprite_key(c);
    r.shape_ = sprite_shape_key(c);
    r.header_ = lo32(c.r[3]);
    r.s7_ = static_cast<int32_t>(lo32(c.r[23]));
    r.t8_ = static_cast<int32_t>(lo32(c.r[24]));
    r.t1_ = static_cast<int32_t>(lo32(c.r[9]));
    r.t3_ = static_cast<int32_t>(lo32(c.r[11]));
    r.t4_ = static_cast<int32_t>(lo32(c.r[12]));
    r.mode_ = static_cast<int32_t>(lo32(c.r[22]));
    r.a0_ = static_cast<int32_t>(lo32(c.r[4]));
}

void HdRenderer::on_after(Context& c) {
    HdRenderer& r = *g_hd;
    if (r.mode_ == 48) return;  // shadows are drawn normally; picked up by the next sync
    auto it = r.pack_.find(r.key_);
    Image* found = nullptr;
    if (it != r.pack_.end()) {
        found = &it->second;
        ++r.hits_exact_;
    } else if (r.mode_ == 0) {  // another palette (costume, flash, lighting): recolour the shape's image
        auto sh = r.shapes_.find(r.shape_);
        if (sh != r.shapes_.end()) found = r.recoloured(c, sh->second);
        if (found) ++r.hits_shape_;
    }
    if (!found) {
        ++r.misses_[static_cast<uint64_t>(r.header_) << 32 | static_cast<uint32_t>(r.mode_)];
        RecentMiss m{r.header_, r.mode_, r.key_, r.shape_, {}};
        std::memcpy(m.palette, &c.dram[0x90000], 0x100);
        if (r.recent_.size() >= 200) r.recent_.erase(r.recent_.begin());
        r.recent_.push_back(m);
        return;
    }
    Image& im = *found;
    if (!im.loaded) r.load(im);
    static const bool trace = std::getenv("KI_HD_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "hd draw: header %08x mode %d scale %d/%d %s image %s %dx%d (original %dx%d)\n", r.header_, r.mode_,
                     r.t1_, r.t3_, it != r.pack_.end() ? "exact" : "shape", im.file.c_str(), im.iw, im.ih, im.w, im.h);
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
    static const int kSearch = std::getenv("KI_HD_SEARCH") ? std::atoi(std::getenv("KI_HD_SEARCH")) : 4;
    int fx0 = 320, fy0 = 240, fx1 = -1, fy1 = -1;
    for (uint32_t bank : {r.bank_, r.bank_ == 0x30000 ? 0x58000u : 0x30000u}) {
        const Bank& bb = r.bank_of(bank);
        const uint16_t* fb = reinterpret_cast<const uint16_t*>(&c.sram[bank]);
        for (int y = std::max(0, y0 - kSearch); y <= std::min(239, y1 + kSearch); ++y)
            for (int x = std::max(0, x0 - kSearch); x <= std::min(319, x1 + kSearch); ++x)
                if (fb[y * 320 + x] != bb.shadow[y * 320 + x]) {
                    fx0 = std::min(fx0, x), fy0 = std::min(fy0, y), fx1 = std::max(fx1, x), fy1 = std::max(fy1, y);
                }
        if (fx1 >= 0) {
            r.bank_ = bank;
            break;
        }
    }
    Bank& b = r.bank_of(r.bank_);
    const uint16_t* lo = reinterpret_cast<const uint16_t*>(&c.sram[r.bank_]);
    if (trace && fx1 >= 0)
        std::fprintf(stderr, "  footprint %d,%d-%d,%d predicted %d,%d-%d,%d a0 %d\n", fx0, fy0, fx1, fy1, x0, y0, x1, y1, r.a0_);
    if (fx1 < 0) {
        if (trace)
            std::fprintf(stderr, "  no footprint: header %08x predicted %d,%d-%d,%d bank %05x (blitter target bit %d)\n", r.header_,
                         x0, y0, x1, y1, r.bank_, c.dram[0x86200] & 1);
        return;
    }
    // Unclipped: the footprint itself is exact.
    bool clipped = fx0 == 0 || fy0 == 0 || fx1 == 319 || fy1 == 239;
    if (!clipped && std::abs((fx1 - fx0) - (x1 - x0)) <= 2 && std::abs((fy1 - fy0) - (y1 - y0)) <= 2)
        x0 = fx0, y0 = fy0, x1 = fx1, y1 = fy1;
    // The pixels the call drew are taken as synced without being upscaled:
    // under the HD image, and around its soft edge, stays what was there
    // before the sprite, not the original sprite enlarged.
    for (int y = fy0; y <= fy1; ++y) {
        std::memcpy(&b.shadow[y * 320 + fx0], lo + y * 320 + fx0, (fx1 - fx0 + 1) * 2);
        std::memset(&b.owned[y * 320 + fx0], 1, fx1 - fx0 + 1);
    }
    r.draw(b, im, x0, y0, x1, y1, mirror);
    ++r.hd_drawn_;
    if (const char* snap = std::getenv("KI_HD_SNAP")) {  // debug: the HD copy right after this header's draw
        static int n = 0;
        if (std::strtoul(snap, nullptr, 16) == r.header_ && n < 40) {
            char name[64];
            std::snprintf(name, sizeof name, "hd_snap_%02d.ppm", n++);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
                for (uint32_t px : b.hd) {
                    uint8_t rgb[3] = {uint8_t(px >> 16), uint8_t(px >> 8), uint8_t(px)};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
            std::fprintf(stderr, "snap %d: header %08x rect %d,%d-%d,%d footprint %d,%d-%d,%d bank %05x\n", n - 1, r.header_, x0, y0,
                         x1, y1, fx0, fy0, fx1, fy1, r.bank_);
        }
    }
}

void HdRenderer::dump_recent_misses(FILE* f) {
    std::fprintf(f, "pack: %zu keys, %zu shapes\n", pack_.size(), shapes_.size());
    for (const RecentMiss& m : recent_) {
        std::fprintf(f, "header %08x mode %d key %016llx shape %016llx%s palette ", m.header, m.mode,
                     static_cast<unsigned long long>(m.key), static_cast<unsigned long long>(m.shape),
                     shapes_.count(m.shape) ? " (shape known)" : "");
        for (uint8_t b : m.palette) std::fprintf(f, "%02x", b);
        std::fprintf(f, "\n");
    }
    recent_.clear();
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
    {
        auto& copies = r.pano_copies_[bank == 0x58000 ? 1 : 0];
        if (r.pano_epoch_[bank == 0x58000 ? 1 : 0] != r.epoch_) copies.clear(), r.pano_epoch_[bank == 0x58000 ? 1 : 0] = r.epoch_;
        copies.push_back({&p, row0, rows, ((col0 % w) + w) % w});
    }
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
            b.owned[y * 320 + x] = 1;
            for (int sy = 0; sy < kScale; ++sy)
                std::memcpy(&b.hd[static_cast<size_t>(y * kScale + sy) * kW + x * kScale],
                            &p.hd[static_cast<size_t>(row * kScale + sy) * p.w * kScale + col * kScale], kScale * 4);
        }
    }
}

constexpr int kNoMatch = 1 << 20;

void HdRenderer::on_prerendered_setup_before(Context& c) {
    HdRenderer& r = *g_hd;
    r.pre_f0_ = lo32(c.f[0]);
    r.pre_f1_ = lo32(c.f[1]);
    r.pre_f2_ = lo32(c.f[2]);
    r.pre_f4_ = static_cast<int32_t>(lo32(c.f[4]));
}

void HdRenderer::on_prerendered_setup(Context& c) {
    HdRenderer& r = *g_hd;
    if (r.pre_rows_.empty()) r.pre_s7_ = lo32(c.r[23]);  // the depth bytes of block 0
    PreRow row;
    row.gp = lo32(c.r[28]);
    row.staged = r.pre_f4_ >= 0;
    row.f2 = r.pre_f2_;
    if (row.staged) {
        // As 0x8803108c: 2 x 80 chunks, each 4 pixels x 4 rows, at f2 + 2 * index.
        uint32_t a0 = r.pre_f0_ - 0x88000000u, a1 = r.pre_f1_;
        auto word = [&](uint32_t off) {
            return off + 4 <= c.dram.size() ? static_cast<uint32_t>(c.dram[off] | c.dram[off + 1] << 8 | c.dram[off + 2] << 16 |
                                                                    static_cast<uint32_t>(c.dram[off + 3]) << 24)
                                            : 0u;
        };
        for (int n = 0; n < 160; ++n) {
            uint32_t t1 = (word(a0) << a1) >> 14;
            if (a1 >= 15) t1 |= word(a0 + 4) >> (46 - a1);
            row.chunk[n] = static_cast<int>(t1 & 0x3ffff);  // pixels from f2
            a1 += 18;
            if (a1 >= 32) a1 &= 31, a0 += 4;
        }
        std::memcpy(row.original, &c.sram[0x2ce00], sizeof row.original);
    }
    r.pre_rows_.push_back(row);
}

void HdRenderer::on_prerendered(Context& c) {
    // The renderer's inputs (0x88030b9c): k0 framebuffer, gp source pixels
    // (in a stage frame), s7 one depth byte per 8x8 block, k1 the camera
    // parameter (0..8), v0 & 7 rows the picture is moved down by.
    HdRenderer& r = *g_hd;
    r.pre_dest_ = lo32(c.r[26]);
    r.pre_gp_ = lo32(c.r[28]);
    r.pre_s7_ = lo32(c.r[23]);
    r.pre_k_ = std::clamp(static_cast<int32_t>(lo32(c.r[27])), 0, 8);
    r.pre_rows_.clear();
    r.pre_v0_ = static_cast<int>(lo32(c.r[2]) & 7);
}

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
    // Block i (row br, column bc) shows its row's source from column bc*8,
    // moved right by s = (depth * k) >> 3 half pixels (an odd s averages two
    // neighbouring columns), at screen row v0 + 8*br: the parallax between the
    // stage's angles.
    if (std::getenv("KI_HD_DEBUG")) {
        static int shown = 0;
        if (shown++ < 4)
            std::fprintf(stderr, "pre-rendered call: k0 %08x s7 %08x k %d v0 %d rows %zu (%s first)\n", r.pre_dest_, r.pre_s7_, r.pre_k_,
                         r.pre_v0_, r.pre_rows_.size(), !r.pre_rows_.empty() && r.pre_rows_[0].staged ? "staged" : "direct");
    }
    // The frames are one tall picture (320 x 240*frames). Each row of blocks
    // reads either a frame directly (still camera) or the 320x8 row the setup
    // step assembled from 4x4 chunks of the frames (moving camera); see hooks.h.
    const uint16_t* sp = reinterpret_cast<const uint16_t*>(&c.dram[frames0]);
    Bank& b = r.bank_of(bank);
    const int k = r.pre_k_, v0 = r.pre_v0_;
    const int rows_total = st.frames * 240;
    auto avg = [](uint16_t a, uint16_t bb) {  // as the renderer (0x88030db4): ((a + 0x421) & 0x7bde) + (b & 0x7bde), halved
        return static_cast<uint16_t>(((((a + 0x0421) & 0x7bde) + (bb & 0x7bde)) >> 1) & 0x7fff);
    };
    auto hd_frame = [&](int f) -> const std::vector<uint32_t>* {
        if (f < 0 || f >= st.frames) return nullptr;
        if (!st.tried[f]) {
            st.tried[f] = true;
            Image im;
            im.file = st.prefix + std::to_string(f) + ".ki4";
            if (r.load(im), !im.missing && im.iw == 320 * kScale && im.ih == 240 * kScale) st.hd[f] = std::move(im.argb);
        }
        return st.hd[f].empty() ? nullptr : &st.hd[f];
    };
    // HD colour at (hx, hy) of the tall picture, in HD pixels.
    auto hd_at = [&](int hx, int hy) -> uint32_t {
        const std::vector<uint32_t>* hf = hd_frame(hy / (240 * kScale));
        if (!hf) return 0;
        return (*hf)[static_cast<size_t>(hy % (240 * kScale)) * 320 * kScale + std::clamp(hx, 0, 320 * kScale - 1)];
    };
    const uint32_t depth = r.pre_s7_ - 0x88000000u;
    if (depth + 1200 > c.dram.size()) return;
    std::vector<uint32_t> hdrow(static_cast<size_t>(320 * kScale + 16) * 8 * kScale);
    for (int br = 0; br < static_cast<int>(r.pre_rows_.size()) && br < 30; ++br) {
        const PreRow& row = r.pre_rows_[br];
        int y0 = v0 + br * 8;
        if (y0 + 8 > 240) break;
        // The row's 8 source lines: originals (to check the model) and HD.
        uint16_t orig[8][321];
        bool usable = true;
        if (row.staged) {
            int base = static_cast<int>(static_cast<int64_t>(row.f2) - 0x88000000 - frames0) / 2;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 320; ++x) orig[y][x] = row.original[y * 320 + x];
                orig[y][320] = orig[y][319];
            }
            for (int n = 0; n < 160 && usable; ++n) {
                int g = n / 80, j = n % 80, p = base + row.chunk[n];
                // Memory is linear: a chunk near the right edge continues on the next line.
                for (int y = 0; y < 4 && usable; ++y)
                    for (int x = 0; x < 4; ++x) {
                        int q = p + y * 320 + x, sy = q / 320, sx = q % 320;
                        if (q < 0 || sy >= rows_total) { usable = false; break; }
                        for (int y4 = 0; y4 < kScale; ++y4)
                            for (int x4 = 0; x4 < kScale; ++x4)
                                hdrow[static_cast<size_t>((g * 4 + y) * kScale + y4) * (320 * kScale + 16) + (j * 4 + x) * kScale + x4] =
                                    hd_at(sx * kScale + x4, sy * kScale + y4);
                    }
            }
        } else {
            int p0 = static_cast<int>(static_cast<int64_t>(row.gp) - 0x88000000 - frames0) / 2;
            int sy = p0 / 320, sx = p0 % 320;
            if (p0 < 0 || sy + 8 > rows_total) usable = false;
            for (int y = 0; y < 8 && usable; ++y) {
                for (int x = 0; x < 321; ++x) orig[y][x] = sp[(sy + y) * 320 + std::min(sx + x, 319)];
                for (int y4 = 0; y4 < kScale; ++y4)
                    for (int x = 0; x < 320 * kScale + 16; ++x)
                        hdrow[static_cast<size_t>(y * kScale + y4) * (320 * kScale + 16) + x] =
                            hd_at(sx * kScale + std::min(x, 320 * kScale - 1), (sy + y) * kScale + y4);
            }
        }
        if (!usable) {
            r.pre_blocks_kept_ += 40;
            continue;
        }
        for (int bc = 0; bc < 40; ++bc) {
            int i = br * 40 + bc;
            int d = static_cast<int8_t>(c.dram[depth + i]);
            int sh = (d * k) >> 3;  // half pixels
            int sx = bc * 8 + (sh >> 1);
            if (sx < 0 || sx + 9 > 321) {
                ++r.pre_blocks_kept_;
                continue;
            }
            // The model must give exactly the pixels drawn (not, e.g., the animated fire).
            bool same = true;
            for (int y = 0; y < 8 && same; ++y)
                for (int x = 0; x < 8; ++x) {
                    uint16_t a = orig[y][sx + x];
                    uint16_t v = (sh & 1) ? avg(a, orig[y][sx + x + 1]) : a;
                    if (v != fb[(y0 + y) * 320 + bc * 8 + x]) {
                        same = false;
                        break;
                    }
                }
            if (!same) {
                ++r.pre_blocks_kept_;
                continue;
            }
            ++r.pre_blocks_hd_;
            // Half an original pixel is exactly 2 HD pixels: sampled there, not averaged.
            const int hx0 = sx * kScale + (sh & 1) * kScale / 2;
            for (int y = 0; y < 8 * kScale; ++y) {
                const uint32_t* src = &hdrow[static_cast<size_t>(y) * (320 * kScale + 16)];
                uint32_t* out = &b.hd[static_cast<size_t>(y0 * kScale + y) * kW + bc * 8 * kScale];
                for (int x = 0; x < 8 * kScale; ++x) out[x] = src[std::min(hx0 + x, 320 * kScale + 15)];
            }
            for (int y = y0; y < y0 + 8; ++y) {
                std::memcpy(&b.shadow[y * 320 + bc * 8], fb + y * 320 + bc * 8, 16);
                std::memset(&b.owned[y * 320 + bc * 8], 1, 8);
            }
        }
    }
}

void HdRenderer::on_screen_save(Context& c) {
    // The source is the other framebuffer than the target (the frame shown).
    HdRenderer& r = *g_hd;
    uint32_t target = (c.dram[0x40dc8] | c.dram[0x40dc9] << 8 | c.dram[0x40dca] << 16 |
                       static_cast<uint32_t>(c.dram[0x40dcb]) << 24) & 0x1fffffffu;
    if (target != 0x30000 && target != 0x58000) return;
    uint32_t source = target == 0x30000 ? 0x58000 : 0x30000;
    r.sync(c, source);
    const std::vector<uint32_t>& hd = r.bank_of(source).hd;
    r.saved_screen_.resize(hd.size());
    for (size_t i = 0; i < hd.size(); ++i) r.saved_screen_[i] = 0xff000000u | ((hd[i] >> 1) & 0x7f7f7f);  // half, as 0x7bde mask + shift
    r.put_saved_screen(c);
}

void HdRenderer::on_screen_restore(Context& c) {
    // 0x884a0000 holds either the saved frame or a picture read from disk
    // (0x8802dc40 reads the drive into it): a picture in the pack wins.
    HdRenderer& r = *g_hd;
    const uint8_t* pic = &c.dram[0x4a0000];
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < 320 * 240 * 2; ++i) key = (key ^ pic[i]) * 1099511628211ull;
    if (!r.screen_dump_dir.empty()) {
        static std::set<uint64_t> seen;
        if (seen.insert(key).second) {
            char name[96];
            std::snprintf(name, sizeof name, "/screen_%016llx.raw", static_cast<unsigned long long>(key));
            if (FILE* f = std::fopen((r.screen_dump_dir + name).c_str(), "wb")) {
                std::fwrite(pic, 1, 320 * 240 * 2, f);
                std::fclose(f);
            }
        }
    }
    auto it = r.screens_.find(key);
    if (it != r.screens_.end()) {
        auto& hd = r.screen_cache_[key];
        if (hd.empty()) {
            Image im;
            im.file = it->second;
            if (r.load(im), !im.missing && im.iw == kW && im.ih == kH) hd = std::move(im.argb);
        }
        if (!hd.empty()) {
            r.saved_screen_ = hd;
            r.put_saved_screen(c);
            return;
        }
    }
    r.put_saved_screen(c);
}

void HdRenderer::put_saved_screen(const Context& c) {
    if (saved_screen_.empty()) return;
    uint32_t target = (c.dram[0x40dc8] | c.dram[0x40dc9] << 8 | c.dram[0x40dca] << 16 |
                       static_cast<uint32_t>(c.dram[0x40dcb]) << 24) & 0x1fffffffu;
    if (target != 0x30000 && target != 0x58000) return;
    Bank& b = bank_of(target);
    b.hd = saved_screen_;
    std::memcpy(b.shadow.data(), &c.sram[target], 320 * 240 * 2);
    std::fill(b.owned.begin(), b.owned.end(), 1);
    b.hd_rects.clear();
}

bool HdRenderer::sides(const Context& c, uint32_t bank, int side_px, std::vector<uint32_t>& left, std::vector<uint32_t>& right) {
    const uint16_t* fb = reinterpret_cast<const uint16_t*>(&c.sram[bank]);
    const int sw = side_px * kScale;
    left.assign(static_cast<size_t>(sw) * kH, 0);
    right.assign(static_cast<size_t>(sw) * kH, 0);
    const auto& copies = pano_copies_[bank == 0x58000 ? 1 : 0];
    bool any = false;
    for (const PanoCopy& pc : copies) {
        const Panorama& p = *pc.pano;
        if (p.hd.empty()) continue;
        const int pw = p.w * kScale;
        for (int row = 0; row < pc.rows && row < p.h; ++row) {
            int y = pc.row0 + row;
            if (y < 0 || y >= 240) continue;
            int shown = 0;
            for (int x = 0; x < 320; ++x)
                shown += fb[y * 320 + x] == p.original[static_cast<size_t>(row) * p.stride + 4 + (pc.col0 + x) % p.w];
            if (shown < 160) continue;
            for (int sy = 0; sy < kScale; ++sy) {
                const uint32_t* src = &p.hd[static_cast<size_t>(row * kScale + sy) * pw];
                uint32_t* l = &left[static_cast<size_t>(y * kScale + sy) * sw];
                uint32_t* rr = &right[static_cast<size_t>(y * kScale + sy) * sw];
                for (int x = 0; x < sw; ++x) {
                    // Columns left of screen column 0 and right of 319, wrapping as the copy does.
                    int lc = ((pc.col0 * kScale - sw + x) % pw + pw) % pw;
                    int rc = ((pc.col0 + 320) * kScale + x) % pw;
                    l[x] = src[lc] | 0xff000000u;
                    rr[x] = src[rc] | 0xff000000u;
                }
            }
            any = true;
        }
    }
    return any;
}

const std::vector<uint32_t>& HdRenderer::frame(const Context& c, uint32_t bank) {
    ++epoch_;
    sync(c, bank);
    return bank_of(bank).hd;
}

}  // namespace ki
