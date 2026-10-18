#include "ki/kinst_board.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace ki {

namespace {
constexpr uint8_t kDrdy = 0x40, kDsc = 0x10, kDrq = 0x08, kErr = 0x01;
constexpr int kCylinders = 419, kHeads = 13, kSectors = 47;  // ST9150AG
}  // namespace

KinstBoard::KinstBoard(std::vector<uint8_t> disk, std::vector<uint8_t> boot_rom)
    : disk_(std::move(disk)), rom_(std::move(boot_rom)) {
    build_identify();
    ide_reset();
}

// ---- timing / interrupt lines ------------------------------------------------

uint32_t KinstBoard::interrupt_lines(Context& c) {
    uint32_t lines = 0;
    if (c.cycles % kCyclesPerFrame >= kVblankStart) lines |= 0x400;  // Int0: VBLANK (screen_vblank)
    if (irq_) lines |= 0x800;                                        // Int1: IDE
    return lines;
}

uint64_t KinstBoard::next_event(Context&) { return next_vblank_; }

void KinstBoard::update(Context& c) {
    while (c.cycles >= next_vblank_) {
        ++frame_;
        next_vblank_ += kCyclesPerFrame;
        if (on_vblank) on_vblank(c, frame_);
    }
}

std::vector<uint8_t> KinstBoard::framebuffer_rgb(const Context& c) const {
    std::vector<uint8_t> rgb(320 * 240 * 3);
    uint32_t base = (vram_control_ & 4) ? 0x58000 : 0x30000;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) {
            uint32_t a = base + (y * 320 + x) * 2;
            uint16_t v = static_cast<uint16_t>(c.sram[a] | (c.sram[a + 1] << 8));
            auto expand = [](unsigned v5) { return static_cast<uint8_t>((v5 << 3) | (v5 >> 2)); };
            uint8_t* p = &rgb[(y * 320 + x) * 3];
            p[0] = expand(v & 0x1f);          // BGR555: red in the low bits
            p[1] = expand((v >> 5) & 0x1f);
            p[2] = expand((v >> 10) & 0x1f);
        }
    return rgb;
}

// ---- I/O space ---------------------------------------------------------------
// MAME's handlers are 32-bit; narrower accesses select byte lanes (little-endian).

uint64_t KinstBoard::read(Context& c, uint32_t phys, int size) {
    if (size == 8) {
        uint64_t lo = read(c, phys, 4), hi = read(c, phys + 4, 4);
        return lo | (hi << 32);
    }
    uint32_t shift = 8 * (phys & 3);
    uint32_t word = reg32_read(c, phys & ~3u);
    uint32_t mask = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
    return (word >> shift) & mask;
}

void KinstBoard::write(Context& c, uint32_t phys, int size, uint64_t value) {
    if (size == 8) {
        write(c, phys, 4, value & 0xffffffffu);
        write(c, phys + 4, 4, value >> 32);
        return;
    }
    uint32_t shift = 8 * (phys & 3);
    uint32_t mask = (size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1) << shift;
    reg32_write(c, phys & ~3u, static_cast<uint32_t>(value << shift), mask);
}

uint32_t KinstBoard::reg32_read(Context&, uint32_t a) {
    switch (a) {
        case 0x10000080: return p1;
        case 0x10000088: return p2;
        case 0x10000090: return 0xffffffff;  // VOLUME: bit 1 = DCS input latch empty (always ready), others released
        case 0x10000098: return 0xffffffff;        // UNUSED
        case 0x100000a0: return dsw;
        case 0x10000170: return ide_read(8, false);  // CS1 reg 6: alternate status
        default: break;
    }
    if (a >= 0x10000100 && a < 0x10000140) return ide_read((a - 0x10000100) / 8, true);
    if (a >= 0x1fc00000 && a < 0x1fc00000 + rom_.size())
        return rom_[a - 0x1fc00000] | rom_[a - 0x1fc00000 + 1] << 8 | rom_[a - 0x1fc00000 + 2] << 16 |
               static_cast<uint32_t>(rom_[a - 0x1fc00000 + 3]) << 24;
    ++stats.unknown_io;
    return 0xffffffff;  // unmap_value_high
}

void KinstBoard::reg32_write(Context&, uint32_t a, uint32_t data, uint32_t mask) {
    auto combine = [&](uint32_t& reg) { reg = (reg & ~mask) | (data & mask); };
    switch (a) {
        case 0x10000080: combine(vram_control_); return;
        case 0x10000088: combine(sound_reset_); ++stats.sound_writes; return;
        case 0x10000090: combine(sound_control_); ++stats.sound_writes; return;  // DCS latch strobe: ignored
        case 0x10000098: combine(sound_data_); ++stats.sound_writes; return;
        case 0x100000a0: case 0x100000b0: return;                                // DSW (nopw), coin counters
        case 0x10000170: ide_write(8, static_cast<uint16_t>(data)); return;       // device control
        default: break;
    }
    if (a >= 0x10000100 && a < 0x10000140) {
        ide_write((a - 0x10000100) / 8, static_cast<uint16_t>(data));
        return;
    }
    ++stats.unknown_io;
}

// ---- IDE -----------------------------------------------------------------------

void KinstBoard::build_identify() {
    uint16_t* w = identify_;
    w[0] = 0x0040;  // fixed disk
    w[1] = kCylinders;
    w[3] = kHeads;
    w[6] = kSectors;
    // MAME machine_reset: serial number all '0', model field spaces with the
    // drive model the game checks ("ST9150AG") at word 27 (m_hdd_serial_offset).
    for (int i = 10; i < 20; ++i) w[i] = ('0' << 8) | '0';
    for (int i = 27; i < 47; ++i) w[i] = (' ' << 8) | ' ';
    const char model[] = "ST9150AG";
    for (int i = 0; i < 4; ++i) w[27 + i] = static_cast<uint16_t>(model[i * 2] << 8 | model[i * 2 + 1]);
    w[47] = 0x8001;
    w[49] = 0x0200;  // LBA supported
    w[53] = 0x0001;
    w[54] = kCylinders;
    w[55] = kHeads;
    w[56] = kSectors;
    uint32_t total = kCylinders * kHeads * kSectors;
    w[57] = w[60] = static_cast<uint16_t>(total);
    w[58] = w[61] = static_cast<uint16_t>(total >> 16);
}

void KinstBoard::ide_reset() {
    status_ = kDrdy | kDsc;
    err_ = 1;  // diagnostic code: no error
    nsect_ = sector_ = 1;
    lcyl_ = hcyl_ = 0;
    xfer_ = Xfer::None;
    irq_ = false;
}

uint32_t KinstBoard::ide_lba() const {
    if (select_ & 0x40) return (select_ & 0xf) << 24 | hcyl_ << 16 | lcyl_ << 8 | sector_;
    uint32_t cyl = hcyl_ << 8 | lcyl_;
    return (cyl * logical_heads_ + (select_ & 0xf)) * logical_spt_ + sector_ - 1;
}

void KinstBoard::ide_load_sector() {
    uint64_t off = static_cast<uint64_t>(lba_) * 512;
    for (int i = 0; i < 256; ++i)
        buffer_[i] = off + 2 * i + 1 < disk_.size() ? static_cast<uint16_t>(disk_[off + 2 * i] | disk_[off + 2 * i + 1] << 8)
                                                    : 0;
    buffer_pos_ = 0;
    status_ = kDrdy | kDsc | kDrq;
    irq_ = true;
    ++stats.sectors_read;
}

void KinstBoard::ide_finish_write_sector() {
    uint64_t off = static_cast<uint64_t>(lba_) * 512;
    for (int i = 0; i < 256 && off + 2 * i + 1 < disk_.size(); ++i) {
        disk_[off + 2 * i] = static_cast<uint8_t>(buffer_[i]);
        disk_[off + 2 * i + 1] = static_cast<uint8_t>(buffer_[i] >> 8);
    }
    ++stats.sectors_written;
    ++lba_;
    buffer_pos_ = 0;
    irq_ = true;
    if (--sectors_left_ == 0) {
        xfer_ = Xfer::None;
        status_ = kDrdy | kDsc;
    }
}

void KinstBoard::ide_command(uint8_t cmd) {
    static const bool trace = std::getenv("KI_TRACE_IDE") != nullptr;
    if (trace)
        std::fprintf(stderr, "[ide] cmd %02x sel %02x nsect %u sector %u cyl %u -> lba %u\n", cmd, select_, nsect_,
                     sector_, hcyl_ << 8 | lcyl_, ide_lba());
    ++stats.ide_commands;
    stats.last_ide_command = cmd;
    err_ = 0;
    if (select_ & 0x10) {  // slave: not present
        status_ = 0;
        return;
    }
    switch (cmd) {
        case 0xec:  // IDENTIFY DEVICE
            for (int i = 0; i < 256; ++i) buffer_[i] = identify_[i];
            buffer_pos_ = 0;
            xfer_ = Xfer::Read;
            sectors_left_ = 1;
            status_ = kDrdy | kDsc | kDrq;
            irq_ = true;
            return;
        case 0x20: case 0x21: case 0xc4:  // READ SECTORS / READ MULTIPLE
            lba_ = ide_lba();
            sectors_left_ = nsect_ ? nsect_ : 256;
            xfer_ = Xfer::Read;
            ide_load_sector();
            return;
        case 0x30: case 0x31: case 0xc5:  // WRITE SECTORS / WRITE MULTIPLE
            lba_ = ide_lba();
            sectors_left_ = nsect_ ? nsect_ : 256;
            xfer_ = Xfer::Write;
            buffer_pos_ = 0;
            status_ = kDrdy | kDsc | kDrq;  // no interrupt before the first sector (PIO out)
            return;
        case 0x91:  // INITIALIZE DEVICE PARAMETERS: logical geometry for CHS
            logical_spt_ = nsect_ ? nsect_ : 256;
            logical_heads_ = (select_ & 0xf) + 1u;
            status_ = kDrdy | kDsc;
            irq_ = true;
            return;
        case 0x10: case 0xef: case 0xc6: case 0xe0: case 0xe1: case 0xe5: case 0x70:
            status_ = kDrdy | kDsc;  // recalibrate, init params, set features, power, seek: complete now
            irq_ = true;
            return;
        default:
            std::fprintf(stderr, "[ide] unsupported command %02x\n", cmd);
            err_ = 0x04;  // ABRT
            status_ = kDrdy | kDsc | kErr;
            irq_ = true;
            return;
    }
}

uint16_t KinstBoard::ide_read(int reg, bool status_clears_irq) {
    switch (reg) {
        case 0: {  // data
            if (xfer_ != Xfer::Read) return 0xffff;
            uint16_t v = buffer_[buffer_pos_++];
            if (buffer_pos_ == 256) {
                if (--sectors_left_ == 0) {
                    xfer_ = Xfer::None;
                    status_ = kDrdy | kDsc;
                } else {
                    ++lba_;
                    ide_load_sector();
                }
            }
            return v;
        }
        case 1: return err_;
        case 2: return nsect_;
        case 3: return sector_;
        case 4: return lcyl_;
        case 5: return hcyl_;
        case 6: return select_ | 0xa0;
        case 7:
            if (status_clears_irq) irq_ = false;
            return status_;
        case 8: return status_;  // alternate status: does not clear the interrupt
        default: return 0xff;
    }
}

void KinstBoard::ide_write(int reg, uint16_t value) {
    switch (reg) {
        case 0:  // data
            if (xfer_ != Xfer::Write) return;
            buffer_[buffer_pos_++] = value;
            if (buffer_pos_ == 256) ide_finish_write_sector();
            return;
        case 1: features_ = static_cast<uint8_t>(value); return;
        case 2: nsect_ = static_cast<uint8_t>(value); return;
        case 3: sector_ = static_cast<uint8_t>(value); return;
        case 4: lcyl_ = static_cast<uint8_t>(value); return;
        case 5: hcyl_ = static_cast<uint8_t>(value); return;
        case 6: select_ = static_cast<uint8_t>(value); return;
        case 7: ide_command(static_cast<uint8_t>(value)); return;
        case 8:  // device control: SRST, nIEN
            if ((value & 4) && !(devctrl_ & 4)) ide_reset();
            devctrl_ = static_cast<uint8_t>(value);
            return;
        default: return;
    }
}

}  // namespace ki
