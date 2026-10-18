// Shared setup for the whole-program executables (ki_run, ki_play).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ki/runtime.h"

namespace ki {

std::vector<uint8_t> read_file_or_die(const std::string& path);
// Loads a boot-state snapshot (tools/mame_capture.py --entry-only at
// 0x88000000): registers, COP0, FPU, SRAM and DRAM. Returns the entry address.
uint32_t load_boot_state(Context& c, const std::string& dir);
// Calls the recompiled entry. Returns false (after printing the state) on a guest fault.
bool run_entry(Context& c, uint32_t entry);

}  // namespace ki
