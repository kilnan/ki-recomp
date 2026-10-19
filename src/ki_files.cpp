#include "ki_files.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <libchdr/chd.h>
#include <miniz.h>

namespace ki {

namespace {

struct Expected {
    const char* name;
    const char* sha1;
};

// MAME src/mame/rare/kinst.cpp, ROM_START(kinst), BIOS v1.5d.
constexpr Expected kBootRom = {"ki-l15d.u98", "607394d4ba1713f38c2cb5159303cace9cde991e"};
constexpr Expected kSoundRoms[] = {
    {"u10-l1", "810d455df8f385d76143e9d7d048f2b555ff8bf0"}, {"u11-l1", "0595909cb667c38ac7c8c7bd0646b28899e27777"},
    {"u12-l1", "7a8ee405c118fd176b66353fa7bfab888cc63cd2"}, {"u13-l1", "ea4d1f153eb284f1bcfc5295fbce316bba6083f4"},
    {"u33-l1", "b22e365bc8d58a80eaac226be14b4bb8d9a04844"}, {"u34-l1", "ee439f5162a2b3379d3f802328017bb3c68547d2"},
    {"u35-l1", "48c4c954ac9db648f28ad64f9845e19ec432eec3"}, {"u36-l1", "cc78070cc41701e9a91fde5cfbdc7e1e83354854"},
};
// SHA1 of the disk's raw sectors ("Data SHA1" in chdman info).
constexpr const char* kDiskDataSha1 = "a37a2c5e52ea936a715210d237874dd573bb002f";

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return static_cast<bool>(f);
}

// A file of the set: loose in the folder, or inside kinst.zip.
bool read_rom(const std::vector<std::string>& dirs, const char* name, std::vector<uint8_t>& out) {
    for (const std::string& d : dirs)
        if (read_file(d + "/" + name, out)) return true;
    for (const std::string& d : dirs) {
        std::string zip = d + "/kinst.zip";
        if (!exists(zip)) continue;
        mz_zip_archive ar;
        std::memset(&ar, 0, sizeof ar);
        if (!mz_zip_reader_init_file(&ar, zip.c_str(), 0)) continue;
        int index = mz_zip_reader_locate_file(&ar, name, nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
        bool ok = false;
        if (index >= 0) {
            size_t size = 0;
            void* p = mz_zip_reader_extract_to_heap(&ar, static_cast<mz_uint>(index), &size, 0);
            if (p) {
                out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
                mz_free(p);
                ok = true;
            }
        }
        mz_zip_reader_end(&ar);
        if (ok) return true;
    }
    return false;
}

bool read_chd(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    chd_file* chd = nullptr;
    chd_error e = chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd);
    if (e != CHDERR_NONE) {
        error = path + ": " + chd_error_string(e);
        return false;
    }
    const chd_header* h = chd_get_header(chd);
    out.assign(static_cast<size_t>(h->totalhunks) * h->hunkbytes, 0);
    for (uint32_t i = 0; i < h->totalhunks; ++i) {
        e = chd_read(chd, i, out.data() + static_cast<size_t>(i) * h->hunkbytes);
        if (e != CHDERR_NONE) {
            error = path + ": hunk " + std::to_string(i) + ": " + chd_error_string(e);
            chd_close(chd);
            return false;
        }
    }
    out.resize(static_cast<size_t>(h->logicalbytes));
    chd_close(chd);
    return true;
}

}  // namespace

bool load_game_files(const std::string& dir, GameFiles& out, std::string& error) {
    const std::vector<std::string> dirs = {dir, dir + "/kinst"};
    auto verify = [&](const Expected& e, std::vector<uint8_t>& data) {
        if (!read_rom(dirs, e.name, data)) {
            error = std::string(e.name) + " not found in " + dir + " (as a file or inside kinst.zip).";
            return false;
        }
        std::string got = sha1_hex(data.data(), data.size());
        if (got != e.sha1) {
            error = std::string(e.name) + " has SHA1 " + got + ", expected " + e.sha1 +
                    ". This program needs the MAME 'kinst' set, BIOS v1.5d.";
            return false;
        }
        return true;
    };
    if (!verify(kBootRom, out.boot_rom)) return false;
    out.sound_roms.clear();
    for (const Expected& e : kSoundRoms) {
        out.sound_roms.emplace_back();
        if (!verify(e, out.sound_roms.back())) return false;
    }
    std::string chd;
    for (const std::string& d : dirs)
        if (exists(d + "/kinst.chd")) {
            chd = d + "/kinst.chd";
            break;
        }
    if (chd.empty()) {
        error = "kinst.chd (the hard disk image) not found in " + dir + ".";
        return false;
    }
    if (!read_chd(chd, out.disk, error)) return false;
    std::string got = sha1_hex(out.disk.data(), out.disk.size());
    if (got != kDiskDataSha1) {
        error = "kinst.chd contains data with SHA1 " + got + ", expected " + kDiskDataSha1 + ".";
        return false;
    }
    return true;
}

// ---- SHA1 (FIPS 180-4) -----------------------------------------------------------

std::string sha1_hex(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999u;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1u;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdcu;
            else f = b ^ c ^ d, k = 0xca62c1d6u;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    };
    size_t full = size / 64 * 64;
    for (size_t i = 0; i < full; i += 64) block(data + i);
    uint8_t tail[128] = {};
    size_t rest = size - full;
    std::memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    size_t len = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = static_cast<uint64_t>(size) * 8;
    for (int i = 0; i < 8; ++i) tail[len - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    block(tail);
    if (len == 128) block(tail + 64);
    char out[41];
    for (int i = 0; i < 5; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return out;
}

}  // namespace ki
