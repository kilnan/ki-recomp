#include "ki/dcs.h"

#include <algorithm>
#include <cstdio>

namespace ki {

Dcs::Dcs(const std::vector<std::vector<uint8_t>>& roms)
    : owned_dsp_(std::make_unique<adsp::Adsp2105>(*this)), dsp_(*owned_dsp_) {
    // ROM_REGION16_LE(0x1000000, "dcs", ROMREGION_ERASEFF) with ROM_LOAD16_BYTE of
    // each 512 KiB ROM at k * 0x200000: the ROM byte is the low byte of a word.
    rom_.assign(0x800000, 0xffff);
    for (size_t k = 0; k < roms.size() && k < 8; ++k)
        for (size_t j = 0; j < roms[k].size() && j < 0x100000; ++j)
            rom_[k * 0x100000 + j] = static_cast<uint16_t>(0xff00 | roms[k][j]);
    rom_banks_ = static_cast<uint32_t>(rom_.size() / 0x1000);
    board_reset();
    dsp_.reset();
}

void Dcs::copy_state_from(const Dcs& o) {
    internal_pram_ = o.internal_pram_;
    external_pram_ = o.external_pram_;
    internal_dram_ = o.internal_dram_;
    control_regs_ = o.control_regs_;
    bank_ = o.bank_;
    latch_control_ = o.latch_control_;
    input_data_ = o.input_data_;
    output_data_ = o.output_data_;
    progflags_ = o.progflags_;
    dac_on_ = o.dac_on_;
    ireg_ = o.ireg_;
    size_ = o.size_;
    ireg_base_ = o.ireg_base_;
    incs_ = o.incs_;
    sample_rate_ = o.sample_rate_;
    half_period_ = o.half_period_;
    next_transfer_ = o.next_transfer_;
    dac_off_since_ = o.dac_off_since_;
    irq1_pending_clear_ = o.irq1_pending_clear_;
    timer_enable_ = o.timer_enable_;
    timer_ignore_ = o.timer_ignore_;
    timer_armed_ = o.timer_armed_;
    timer_scale_ = o.timer_scale_;
    timer_period_ = o.timer_period_;
    timer_start_count_ = o.timer_start_count_;
    timer_start_cycles_ = o.timer_start_cycles_;
    timers_fired_ = o.timers_fired_;
    next_timer_ = o.next_timer_;
    samples_.clear();
    dsp_.copy_state_from(o.dsp_);
}

// ---- host interface ----------------------------------------------------------

void Dcs::reset_w(bool state) {
    if (!state) {
        board_reset();
        dsp_.set_reset(true);
    } else {
        dsp_.set_reset(false);
    }
}

void Dcs::data_w(uint16_t data) {
    // MAME dcs_delayed_data_w: IRQ2 on, input latch full.
    dsp_.set_input(adsp::kIrq2, true);
    latch_control_ &= ~kInputEmpty;
    input_data_ = data;
}

void Dcs::board_reset() {
    // MAME dcs_reset (REV_DCS1)
    bank_ = 0;
    size_ = 0;
    incs_ = 0;
    ireg_ = 0;
    control_regs_.fill(0);
    dsp_.set_input(adsp::kIrq0, false);
    dsp_.set_input(adsp::kIrq1, false);
    dsp_.set_input(adsp::kIrq2, false);
    latch_control_ |= kInputEmpty | kOutputEmpty;
    boot();
    timer_ignore_ = false;
    timer_enable_ = false;
    timer_scale_ = 1;
    timer_armed_ = false;
    stop_dac();
}

void Dcs::boot() {
    // Boot from the selected data bank: the low byte of each word is the 8-bit boot ROM.
    uint8_t buffer[0x1000];
    const uint16_t* base = &rom_[(static_cast<size_t>(bank_) * 0x1000) % rom_.size()];
    for (int i = 0; i < 0x1000; ++i) buffer[i] = static_cast<uint8_t>(base[i]);
    uint32_t page[0x800];
    int words = (buffer[3] + 1) * 8;
    adsp::Adsp2105::load_boot_data(buffer, page);
    for (int i = 0; i < words && i < 0x400; ++i) internal_pram_[i] = page[i];
}

// ---- scheduling --------------------------------------------------------------

void Dcs::run_until(uint64_t target) {
    while (dsp_.total_cycles() < target) {
        uint64_t now = dsp_.total_cycles();
        uint64_t stop = target;
        if (dac_on_) stop = std::min(stop, next_transfer_);
        if (timer_armed_) stop = std::min(stop, next_timer_);
        if (stop <= now) stop = now + 1;
        // IRQ1 is a pulse (MAME pulse_input_line for the minimum quantum): the DSP
        // samples it when the next slice starts, so that slice is one instruction long.
        if (irq1_pending_clear_) stop = now + 1;
        dsp_.run(static_cast<int>(std::min<uint64_t>(stop - now, 1u << 16)));
        if (irq1_pending_clear_) {
            irq1_pending_clear_ = false;
            dsp_.set_input(adsp::kIrq1, false);
        }
        now = dsp_.total_cycles();
        if (dac_on_ && now >= next_transfer_) {
            next_transfer_ += half_period_;
            transfer_half_buffer();
        }
        if (timer_armed_ && now >= next_timer_) timer_fire();
    }
}

// ---- DSP memory map (dcs_2k_program_map / dcs_2k_data_map) -----------------------

uint32_t Dcs::program_read(uint32_t addr) {
    addr &= 0x3fff;
    if (addr < 0x400) return internal_pram_[addr];
    if (addr >= 0x800 && addr < 0x2000) return external_pram_[addr & 0x7ff];
    return 0;
}

void Dcs::program_write(uint32_t addr, uint32_t data) {
    addr &= 0x3fff;
    if (addr < 0x400) internal_pram_[addr] = data & 0xffffff;
    else if (addr >= 0x800 && addr < 0x2000) external_pram_[addr & 0x7ff] = data & 0xffffff;
}

uint16_t Dcs::data_read(uint32_t addr) {
    addr &= 0x3fff;
    if (addr < 0x2000) return static_cast<uint16_t>(external_pram_[addr & 0x7ff] >> 8);  // mirror 0x1800
    if (addr < 0x3000) return rom_[(bank_ % rom_banks_) * 0x1000 + (addr & 0xfff)];
    if (addr >= 0x3400 && addr < 0x3800) {
        // input_latch_r with auto-ack (non-RAM DCS boards)
        latch_control_ |= kInputEmpty;
        dsp_.set_input(adsp::kIrq2, false);
        return input_data_;
    }
    if (addr >= 0x3800 && addr < 0x3a00) return internal_dram_[addr - 0x3800];
    if (addr >= 0x3fe0) return control_r(addr - 0x3fe0);
    return 0;
}

void Dcs::data_write(uint32_t addr, uint16_t data) {
    addr &= 0x3fff;
    if (addr < 0x2000) {
        uint32_t& w = external_pram_[addr & 0x7ff];
        w = (static_cast<uint32_t>(data) << 8) | (w & 0xff);
    } else if (addr >= 0x3000 && addr < 0x3400) {
        bank_ = data & 0x7ff;  // dcs_data_bank_select_w
    } else if (addr >= 0x3400 && addr < 0x3800) {
        output_data_ = data;  // output latch: KI does not read it back
        latch_control_ &= ~kOutputEmpty;
    } else if (addr >= 0x3800 && addr < 0x3a00) {
        internal_dram_[addr - 0x3800] = data;
    } else if (addr >= 0x3fe0) {
        control_w(addr - 0x3fe0, data);
    }
}

// ---- ADSP memory-mapped control registers (adsp_control_r/w) --------------------

uint16_t Dcs::control_r(uint32_t offset) {
    switch (offset) {
        case kProgFlagData: {
            uint16_t r = (control_regs_[kProgFlagControl] & control_regs_[kProgFlagData]) |
                         (progflags_ & ~control_regs_[kProgFlagControl]);
            progflags_ ^= 0x6;
            return r;
        }
        case kTimerCount:
            timer_update_count();
            return control_regs_[offset];
        default:
            return control_regs_[offset];
    }
}

void Dcs::control_w(uint32_t offset, uint16_t data) {
    control_regs_[offset] = data;
    switch (offset) {
        case kSysControl:
            if (data & 0x0200) {  // bit 9: reboot
                dsp_.set_reset(true);
                dsp_.set_reset(false);
                boot();
                control_regs_[kSysControl] = 0;
            }
            if (!(data & 0x0800)) stop_dac();  // SPORT1 disabled
            break;
        case kS1Autobuf:
            if (!(data & 0x0002)) stop_dac();
            break;
        case kTimerScale: {
            uint32_t scale = (data & 0xff) + 1u;
            if (scale != timer_scale_) {
                timer_update_count();
                timer_scale_ = scale;
                timer_reset();
            }
            break;
        }
        case kTimerCount:
            timer_start_count_ = data;
            timer_reset();
            break;
        case kTimerPeriod:
            if (data != timer_period_) {
                timer_update_count();
                timer_period_ = data;
                timer_reset();
            }
            break;
        default:
            break;
    }
}

// ---- SPORT1 autobuffer to the DAC (sound_tx_callback / dcs_irq) ---------------------

void Dcs::sport_tx(int port, uint32_t) {
    if (port != 1) return;
    if ((control_regs_[kSysControl] & 0x0800) && (control_regs_[kS1Autobuf] & 0x0002)) {
        ireg_ = (control_regs_[kS1Autobuf] >> 9) & 7;
        int mreg = ((control_regs_[kS1Autobuf] >> 7) & 3) | (ireg_ & 4);
        incs_ = dsp_.mreg(mreg);
        size_ = dsp_.lreg(ireg_);
        uint32_t source = dsp_.ireg(ireg_) & ~0xfu;
        dsp_.set_ireg(ireg_, source);
        ireg_base_ = source;
        // recompute_sample_rate (one channel)
        uint64_t sample_cycles = (control_regs_[kS1Control] & 0x4000)
                                     ? 2ull * (control_regs_[kS1Sclkdiv] + 1u) * 16u
                                     : kClock / 31250;
        sample_rate_ = static_cast<double>(kClock) / static_cast<double>(sample_cycles);
        if (incs_) {
            if (!dac_on_) pad_silence(sample_cycles);
            half_period_ = sample_cycles * size_ / (2u * static_cast<uint32_t>(incs_));
            if (!half_period_) half_period_ = 1;
            next_transfer_ = dsp_.total_cycles() + half_period_;
            dac_on_ = true;
            dsp_.abort_slice();
        }
        return;
    }
    stop_dac();
}

void Dcs::stop_dac() {
    if (dac_on_) dac_off_since_ = dsp_.total_cycles();
    dac_on_ = false;
}

void Dcs::pad_silence(uint64_t sample_cycles) {
    // While the DAC is off MAME's stream outputs silence; keep the timeline continuous.
    uint64_t now = dsp_.total_cycles();
    if (now > dac_off_since_) samples_.insert(samples_.end(), (now - dac_off_since_) / sample_cycles, 0);
    dac_off_since_ = now;
}

void Dcs::transfer_half_buffer() {
    uint32_t reg = dsp_.ireg(ireg_);
    int count = static_cast<int>(size_ / (2u * static_cast<uint32_t>(incs_ ? incs_ : 1)));
    for (int i = 0; i < count; ++i) {
        samples_.push_back(static_cast<int16_t>(data_read(reg)));
        reg += static_cast<uint32_t>(incs_);
    }
    ireg_base_ = dsp_.get_ibase(ireg_);
    if (reg >= ireg_base_ + size_) {
        reg = ireg_base_;
        dsp_.set_input(adsp::kIrq1, true);
        irq1_pending_clear_ = true;
    }
    dsp_.set_ireg(ireg_, reg);
}

// ---- internal timer ------------------------------------------------------------

void Dcs::timer_fired(bool enabled) {
    // timer_enable_callback: MSTAT timer bit changed
    if (enabled) {
        timer_enable_ = true;
        timer_reset();
    } else {
        timer_update_count();
        timer_enable_ = false;
        timer_armed_ = false;
    }
}

void Dcs::timer_update_count() {
    if (!timer_enable_) return;
    uint64_t elapsed_clocks = (dsp_.total_cycles() - timer_start_cycles_) / timer_scale_;
    if (elapsed_clocks < timer_start_count_ + 1ull) {
        timer_start_count_ -= static_cast<uint32_t>(elapsed_clocks);
    } else {
        elapsed_clocks -= timer_start_count_ + 1ull;
        uint64_t periods = elapsed_clocks / (timer_period_ + 1ull);
        elapsed_clocks -= periods * (timer_period_ + 1ull);
        timer_start_count_ = static_cast<uint32_t>(timer_period_ - elapsed_clocks);
    }
    control_regs_[kTimerCount] = static_cast<uint16_t>(timer_start_count_);
}

void Dcs::timer_reset() {
    if (!timer_enable_) return;
    timer_start_cycles_ = dsp_.total_cycles();
    timers_fired_ = 0;
    // If the timer handler only refreshes DRAM, MAME does not fire it at all.
    if (!timer_ignore_ && program_read(0x18) == 0x0c0030 && program_read(0x19) == 0x804828 &&
        program_read(0x1a) == 0x904828 && program_read(0x1b) == 0x0c0020 && program_read(0x1c) == 0x0a001f)
        timer_ignore_ = true;
    timer_armed_ = !timer_ignore_;
    if (timer_armed_) next_timer_ = timer_start_cycles_ + timer_scale_ * (timer_start_count_ + 1ull);
    dsp_.abort_slice();
}

void Dcs::timer_fire() {
    ++timers_fired_;
    uint64_t target = timer_start_cycles_ +
                      timer_scale_ * (timer_start_count_ + 1ull + timers_fired_ * (timer_period_ + 1ull));
    if (!timer_ignore_ && (timer_period_ > 10 || timer_scale_ > 1)) next_timer_ = target;
    else timer_armed_ = false;
    // edge triggered
    dsp_.set_input(adsp::kTimer, true);
    dsp_.set_input(adsp::kTimer, false);
}

}  // namespace ki
