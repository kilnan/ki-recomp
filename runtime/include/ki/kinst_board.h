// Killer Instinct 1 board model for the recompiled CPU: I/O registers, the
// IDE drive (PIO, raw disk image), VBLANK timing and the framebuffer.
// Register map and behaviour follow MAME src/mame/rare/kinst.cpp (kinst_map).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "ki/dcs.h"
#include "ki/runtime.h"

namespace ki {

class KinstBoard : public IoHandler {
public:
    // CPU clock 100 MHz, pixel clock 6.25 MHz (16 CPU cycles per pixel),
    // 406 x 261 total, 320 x 240 visible; VBLANK = lines 240..260.
    static constexpr uint64_t kCyclesPerPixel = 16;
    static constexpr uint64_t kCyclesPerLine = 406 * kCyclesPerPixel;
    static constexpr uint64_t kCyclesPerFrame = 261 * kCyclesPerLine;
    static constexpr uint64_t kVblankStart = 240 * kCyclesPerLine;

    KinstBoard(std::vector<uint8_t> disk, std::vector<uint8_t> boot_rom);

    uint64_t read(Context& c, uint32_t phys, int size) override;
    void write(Context& c, uint32_t phys, int size, uint64_t value) override;
    uint32_t interrupt_lines(Context& c) override;
    // For starts from a mid-game snapshot: cycles from cycle 0 to the next VBLANK start.
    void set_cycles_to_vblank(uint64_t cycles);
    // For starts from a mid-game snapshot: the logical CHS geometry the game set at
    // boot with INITIALIZE DEVICE PARAMETERS (KI 1: 14 heads x 40 sectors).
    void set_logical_geometry(uint32_t heads, uint32_t spt) { logical_heads_ = heads; logical_spt_ = spt; }
    uint64_t next_event(Context& c) override;
    void update(Context& c) override;

    // Called at the start of every VBLANK with the frame number (from 1).
    std::function<void(Context&, uint64_t frame)> on_vblank;
    // Displayed framebuffer as RGB888, 320 x 240 (MAME screen_update).
    std::vector<uint8_t> framebuffer_rgb(const Context& c) const;
    // SRAM offset of the framebuffer being displayed (0x30000 or 0x58000).
    uint32_t displayed_bank() const { return (vram_control_ & 4) ? 0x58000 : 0x30000; }

    // Sound board (optional; without it the DCS reads as always ready and is silent).
    // The DSP runs at 10 MHz, one tenth of the CPU clock.
    static constexpr uint64_t kCyclesPerDspCycle = 10;
    Dcs* dcs = nullptr;

    // Inputs (active low, all released = 0xffffffff), see kinst.cpp ports.
    uint32_t p1 = 0xffffffff, p2 = 0xffffffff, dsw = 0xffffffff;

    // The disk as the game has written it, and which sectors it wrote (settings,
    // high scores), so a player can keep them between sessions.
    const std::vector<uint8_t>& disk() const { return disk_; }
    const std::vector<uint32_t>& written_sectors() const { return written_; }
    uint64_t disk_writes() const { return stats.sectors_written; }

    // Optional I/O access histogram (address -> count), enabled by io_histogram_on.
    bool io_histogram_on = false;
    std::vector<std::pair<uint32_t, uint64_t>> io_histogram() const;
    struct Stats {
        uint64_t ide_commands = 0, sectors_read = 0, sectors_written = 0, sound_writes = 0;
        uint32_t last_ide_command = 0, unknown_io = 0;
    } stats;

private:
    uint32_t reg32_read(Context& c, uint32_t addr);
    void reg32_write(Context& c, uint32_t addr, uint32_t data, uint32_t mask);

    // IDE (ATA, master only)
    uint16_t ide_read(int reg, bool status_clears_irq);
    void ide_write(int reg, uint16_t value);
    void ide_command(uint8_t cmd);
    void ide_reset();
    uint32_t ide_lba() const;
    void ide_load_sector();
    void track_head_position();
    void ide_finish_write_sector();
    void build_identify();

    std::vector<uint8_t> disk_, rom_;
    std::vector<uint32_t> written_;  // sorted, unique
    std::vector<uint64_t> io_reads_ = std::vector<uint64_t>(0x200), io_writes_ = std::vector<uint64_t>(0x200);
    uint64_t frame_ = 0, next_vblank_ = kVblankStart, phase_ = 0;
    bool trace_vram_ = std::getenv("KI_TRACE_VRAM") != nullptr;
    uint32_t vram_control_ = 0, sound_control_ = 0, sound_data_ = 0, sound_reset_ = 0;

    uint8_t err_ = 0, features_ = 0, nsect_ = 0, sector_ = 0, lcyl_ = 0, hcyl_ = 0, select_ = 0;
    uint8_t status_ = 0, devctrl_ = 0;
    // Logical geometry for CHS addressing, set by INITIALIZE DEVICE PARAMETERS
    // (0x91) like MAME's set_geometry(); the game sets 14 heads x 40 sectors.
    uint32_t logical_heads_ = 13, logical_spt_ = 47;
    // Command timing (MAME atastorage.cpp / atahle.cpp): the drive is busy
    // until busy_until_, then completes `pending_`. Seek = 13 ms full stroke
    // scaled by cylinder distance + 1.3 ms rotational latency; next sector of
    // a multi-sector read 400 ns; sector write 100 us; other commands 10 us.
    enum class Pending { None, Command, ReadSector, WriteDone } pending_ = Pending::None;
    uint64_t busy_until_ = 0;
    uint32_t cur_cylinder_ = 0;
    uint64_t now_ = 0;  // ctx.cycles at the last access
    void start_busy(uint64_t cycles, Pending what);
    void complete_pending();
    bool irq_ = false;
    enum class Xfer { None, Read, Write } xfer_ = Xfer::None;
    uint32_t lba_ = 0, sectors_left_ = 0;
    uint16_t buffer_[256] = {};
    int buffer_pos_ = 0;
    uint16_t identify_[256] = {};
};

}  // namespace ki
