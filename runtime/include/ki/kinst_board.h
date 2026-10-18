// Killer Instinct 1 board model for the recompiled CPU: I/O registers, the
// IDE drive (PIO, raw disk image), VBLANK timing and the framebuffer.
// Register map and behaviour follow MAME src/mame/rare/kinst.cpp (kinst_map).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
    uint64_t next_event(Context& c) override;
    void update(Context& c) override;

    // Called at the start of every VBLANK with the frame number (from 1).
    std::function<void(Context&, uint64_t frame)> on_vblank;
    // Displayed framebuffer as RGB888, 320 x 240 (MAME screen_update).
    std::vector<uint8_t> framebuffer_rgb(const Context& c) const;

    // Inputs (active low, all released = 0xffffffff), see kinst.cpp ports.
    uint32_t p1 = 0xffffffff, p2 = 0xffffffff, dsw = 0xffffffff;

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
    void ide_finish_write_sector();
    void build_identify();

    std::vector<uint8_t> disk_, rom_;
    uint64_t frame_ = 0, next_vblank_ = kVblankStart;
    uint32_t vram_control_ = 0, sound_control_ = 0, sound_data_ = 0, sound_reset_ = 0;

    uint8_t err_ = 0, features_ = 0, nsect_ = 0, sector_ = 0, lcyl_ = 0, hcyl_ = 0, select_ = 0;
    uint8_t status_ = 0, devctrl_ = 0;
    // Logical geometry for CHS addressing, set by INITIALIZE DEVICE PARAMETERS
    // (0x91) like MAME's set_geometry(); the game sets 14 heads x 40 sectors.
    uint32_t logical_heads_ = 13, logical_spt_ = 47;
    bool irq_ = false;
    enum class Xfer { None, Read, Write } xfer_ = Xfer::None;
    uint32_t lba_ = 0, sectors_left_ = 0;
    uint16_t buffer_[256] = {};
    int buffer_pos_ = 0;
    uint16_t identify_[256] = {};
};

}  // namespace ki
