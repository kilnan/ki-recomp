// Shared setup for the whole-program executables (ki_run, ki_play).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ki/runtime.h"

namespace ki {

std::vector<uint8_t> read_file_or_die(const std::string& path);
// The eight DCS sample ROMs (u10-l1 ... u36-l1) from a directory, in board order.
std::vector<std::vector<uint8_t>> read_sound_roms_or_die(const std::string& dir);
// Writes 16-bit mono PCM as a WAV file.
void write_wav(const std::string& path, const std::vector<int16_t>& samples, uint32_t rate);
// Loads a boot-state snapshot (tools/mame_capture.py --entry-only at
// 0x88000000): registers, COP0, FPU, SRAM and DRAM. Returns the entry address.
uint32_t load_boot_state(Context& c, const std::string& dir);
// Power-on state of the CPU (MAME mips3 reset: Status = BEV | ERL, everything
// else zero) with empty RAM, and the boot ROM's code installed: the program
// then starts at the reset vector 0xbfc00000 like the real board. Returns it.
uint32_t cold_reset(Context& c);
// Calls the recompiled entry. Returns false (after printing the state) on a guest fault.
bool run_entry(Context& c, uint32_t entry);

}  // namespace ki
