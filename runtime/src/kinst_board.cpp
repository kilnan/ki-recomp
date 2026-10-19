#include "ki/kinst_board.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ki {

namespace {
constexpr uint8_t kBsy = 0x80, kDrdy = 0x40, kDsc = 0x10, kDrq = 0x08, kErr = 0x01;
constexpr int kCylinders = 419, kHeads = 13, kSectors = 47;  // ST9150AG
constexpr uint64_t kUsec = 100;  // CPU cycles per microsecond (100 MHz)
}  // namespace

KinstBoard::KinstBoard(std::vector<uint8_t> disk, std::vector<uint8_t> boot_rom)
    : disk_(std::move(disk)), rom_(std::move(boot_rom)) {
    build_identify();
    ide_reset();
}

// ---- timing / interrupt lines ------------------------------------------------

uint32_t KinstBoard::interrupt_lines(Context& c) {
    uint32_t lines = 0;
    if ((c.cycles + phase_) % kCyclesPerFrame >= kVblankStart) lines |= 0x400;  // Int0: VBLANK (screen_vblank)
    if (irq_) lines |= 0x800;                                        // Int1: IDE
    return lines;
}

void KinstBoard::set_cycles_to_vblank(uint64_t cycles) {
    cycles %= kCyclesPerFrame;
    phase_ = (kVblankStart + kCyclesPerFrame - cycles) % kCyclesPerFrame;  // frame position at cycle 0
    next_vblank_ = cycles ? cycles : kCyclesPerFrame;
}

uint64_t KinstBoard::next_event(Context&) {
    return pending_ != Pending::None && busy_until_ < next_vblank_ ? busy_until_ : next_vblank_;
}

void KinstBoard::start_busy(uint64_t cycles, Pending what) {
    status_ = kBsy | kDsc;
    pending_ = what;
    busy_until_ = now_ + cycles;
}

void KinstBoard::complete_pending() {
    Pending what = pending_;
    pending_ = Pending::None;
    switch (what) {
        case Pending::Command:
            status_ = (status_ & ~kBsy) | kDrdy;
            if (xfer_ == Xfer::Read) status_ |= kDrq;
            irq_ = true;
            break;
        case Pending::ReadSector: ide_load_sector(); break;
        case Pending::WriteDone:
            irq_ = true;
            status_ = xfer_ == Xfer::Write ? (kDrdy | kDsc | kDrq) : (kDrdy | kDsc);
            break;
        case Pending::None: break;
    }
}

void KinstBoard::update(Context& c) {
    now_ = c.cycles;
    if (dcs && c.cycles / kCyclesPerDspCycle >= dcs->cycles() + 1000) dcs->run_until(c.cycles / kCyclesPerDspCycle);
    if (pending_ != Pending::None && c.cycles >= busy_until_) complete_pending();
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

std::vector<std::pair<uint32_t, uint64_t>> KinstBoard::io_histogram() const {
    std::vector<std::pair<uint32_t, uint64_t>> out;
    for (uint32_t i = 0; i < io_reads_.size(); ++i) {
        if (io_reads_[i]) out.emplace_back(0x10000000u + i, io_reads_[i]);
        if (io_writes_[i]) out.emplace_back(0x90000000u + i, io_writes_[i]);  // bit 31: write
    }
    return out;
}

uint32_t KinstBoard::reg32_read(Context& c, uint32_t a) {
    now_ = c.cycles;
    if (pending_ != Pending::None && now_ >= busy_until_) complete_pending();
    if (io_histogram_on && a - 0x10000000u < io_reads_.size()) ++io_reads_[a - 0x10000000u];
    switch (a) {
        case 0x10000080: return p1;
        case 0x10000088: return p2;
        case 0x10000090:  // VOLUME: bit 1 = DCS input latch empty (active high), others released
            if (!dcs) return 0xffffffff;
            dcs->run_until(c.cycles / kCyclesPerDspCycle);
            return dcs->input_empty() ? 0xffffffff : 0xfffffffd;
        case 0x10000098: return 0xffffffff;        // UNUSED
        case 0x100000a0: return dsw;
        case 0x10000170: return ide_read(8, false);  // CS1 reg 6: alternate status
        default: break;
    }
    if (a >= 0x10000100 && a < 0x10000140) return ide_read((a - 0x10000100) / 8, true);
    if (a >= 0x1fc00000 && a < 0x1fc00000 + rom_.size()) {
        c.cycles += 128;  // RdRdy wait states on every EPROM read (kinst.cpp rom_r)
        return rom_[a - 0x1fc00000] | rom_[a - 0x1fc00000 + 1] << 8 | rom_[a - 0x1fc00000 + 2] << 16 |
               static_cast<uint32_t>(rom_[a - 0x1fc00000 + 3]) << 24;
    }
    ++stats.unknown_io;
    return 0xffffffff;  // unmap_value_high
}

void KinstBoard::reg32_write(Context& c, uint32_t a, uint32_t data, uint32_t mask) {
    now_ = c.cycles;
    if (pending_ != Pending::None && now_ >= busy_until_) complete_pending();
    if (io_histogram_on && a - 0x10000000u < io_writes_.size()) ++io_writes_[a - 0x10000000u];
    auto combine = [&](uint32_t& reg) { reg = (reg & ~mask) | (data & mask); };
    switch (a) {
        case 0x10000080:
            combine(vram_control_);
            if (trace_vram_)  // KI_TRACE_VRAM: buffer flips, to compare frame timing with MAME
                std::fprintf(stderr, "[vram] %llu %x\n", static_cast<unsigned long long>(c.cycles * 10), data);
            return;
        case 0x10000088:  // sound_reset_w: bit 0 low holds the DCS in reset
            combine(sound_reset_);
            ++stats.sound_writes;
            if (dcs) {
                dcs->run_until(c.cycles / kCyclesPerDspCycle);
                dcs->reset_w(sound_reset_ & 1);
            }
            return;
        case 0x10000090: {  // sound_control_w: a rising edge of bit 1 sends sound_data to the DCS
            uint32_t prev = sound_control_;
            combine(sound_control_);
            ++stats.sound_writes;
            if (dcs && !(prev & 2) && (sound_control_ & 2)) {
                dcs->run_until(c.cycles / kCyclesPerDspCycle);
                dcs->data_w(static_cast<uint16_t>(sound_data_));
            }
            return;
        }
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
    // Same words as MAME: device_ata_mass_storage_device_interface::
    // ide_build_identify_device() (atastorage.cpp) for this geometry, then the
    // patches of kinst_state::machine_reset(). The game checks this data while
    // loading a fight and hangs on the VS screen if it does not match.
    uint16_t* w = identify_;
    auto ident = [&](int word, int count, const char* text) {  // device_ata_hle_interface::identify()
        size_t len = std::strlen(text), p = 0;
        for (int i = 0; i < count; ++i) {
            uint16_t hi = p < len ? static_cast<uint8_t>(text[p++]) : ' ';
            uint16_t lo = p < len ? static_cast<uint8_t>(text[p++]) : ' ';
            w[word + i] = static_cast<uint16_t>(hi << 8 | lo);
        }
    };
    uint32_t total = kCylinders * kHeads * kSectors;
    w[0] = 0x045a;
    w[1] = kCylinders;
    w[3] = kHeads;
    w[6] = kSectors;
    ident(10, 10, "00000000000000000000");
    w[22] = 4;
    ident(23, 4, "1.0");
    ident(27, 20, "MAME    Virtual Hard Disk");
    w[47] = 0x8010;
    w[49] = 0x0f03;
    w[51] = 2;
    w[52] = 2;
    w[53] = 3;
    w[54] = kCylinders;
    w[55] = kHeads;
    w[56] = kSectors;
    w[57] = static_cast<uint16_t>(total);
    w[58] = static_cast<uint16_t>(total >> 16);
    w[60] = static_cast<uint16_t>(total);
    w[61] = static_cast<uint16_t>(total >> 16);
    w[62] = 0x0007;
    w[63] = 0x0407;
    w[64] = 0x0003;
    w[65] = 0x78;
    w[66] = 0x78;
    w[67] = 0x014d;
    w[68] = 0x78;
    // kinst_state::machine_reset(): serial all '0', model field blank with the
    // drive model at word 27 (m_hdd_serial_offset for KI 1).
    for (int i = 10; i < 20; ++i) w[i] = ('0' << 8) | '0';
    for (int i = 27; i < 47; ++i) w[i] = (' ' << 8) | ' ';
    const char model[] = "ST9150AG";
    for (int i = 0; i < 4; ++i) w[27 + i] = static_cast<uint16_t>(model[i * 2] << 8 | model[i * 2 + 1]);
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

// MAME next_sector(): the head follows each sector of a multi-sector transfer,
// so a read that continues where the previous one stopped needs no seek.
void KinstBoard::track_head_position() {
    uint32_t per_cyl = logical_heads_ * logical_spt_;
    cur_cylinder_ = per_cyl ? lba_ / per_cyl : 0;
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
    auto at = std::lower_bound(written_.begin(), written_.end(), lba_);
    if (at == written_.end() || *at != lba_) written_.insert(at, lba_);
    ++stats.sectors_written;
    ++lba_;
    if (sectors_left_ > 1) track_head_position();
    buffer_pos_ = 0;
    if (--sectors_left_ == 0) xfer_ = Xfer::None;
    start_busy(kUsec * 100, Pending::WriteDone);
}

void KinstBoard::ide_command(uint8_t cmd) {
    static const bool trace = std::getenv("KI_TRACE_IDE") != nullptr;
    if (trace)
        std::fprintf(stderr, "[ide] cmd %02x sel %02x nsect %u sector %u cyl %u -> lba %u  frame %llu  cycle %llu\n", cmd,
                     select_, nsect_, sector_, hcyl_ << 8 | lcyl_, ide_lba(), static_cast<unsigned long long>(frame_),
                     static_cast<unsigned long long>(now_));
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
            start_busy(kUsec * 10, Pending::Command);
            return;
        case 0x20: case 0x21: case 0xc4: {  // READ SECTORS / READ MULTIPLE
            lba_ = ide_lba();
            sectors_left_ = nsect_ ? nsect_ : 256;
            xfer_ = Xfer::Read;
            uint32_t per_cyl = logical_heads_ * logical_spt_;
            uint32_t cyl = per_cyl ? lba_ / per_cyl : 0;
            uint32_t diff = cyl > cur_cylinder_ ? cyl - cur_cylinder_ : cur_cylinder_ - cyl;
            cur_cylinder_ = cyl;
            uint64_t seek = diff == 0 ? kUsec * 4 / 10
                                      : kUsec * 13000 * diff / kCylinders + kUsec * 1300;
            start_busy(seek, Pending::ReadSector);
            return;
        }
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
            xfer_ = Xfer::None;
            start_busy(kUsec * 10, Pending::Command);
            return;
        case 0x10: case 0xef: case 0xc6: case 0xe0: case 0xe1: case 0xe5: case 0x70:
            xfer_ = Xfer::None;  // recalibrate, set features, power, seek
            start_busy(kUsec * 10, Pending::Command);
            return;
        case 0xf9:  // SET MAX ADDRESS: accepted, only an interrupt (MAME atastorage.cpp)
            xfer_ = Xfer::None;
            status_ = kDrdy | kDsc;
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
                    track_head_position();
                    irq_ = false;
                    start_busy(kUsec * 4 / 10, Pending::ReadSector);
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
