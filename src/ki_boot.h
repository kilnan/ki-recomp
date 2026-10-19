// Shared setup for the whole-program executables (ki_run, ki_play).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <memory>
#include <optional>

#include "ki/dcs.h"
#include "ki/kinst_board.h"
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

// Save states, taken at the entry of a per-frame function: the program can be
// restarted there with run_entry, which then continues at the return site in
// the main loop (see SaveState::kDispatcher). To load, the caller unwinds the host stack
// (throw LoadStateRequest from a callback) and restarts with restore().
struct LoadStateRequest {};
class SaveState {
public:
    // Taken at the entry of either per-frame function whose return site in a
    // main loop is an entry point (KI_EXTRA_ENTRIES): the dispatcher during
    // games, 0x88003e08 in the attract demo.
    static constexpr uint32_t kDispatcher = 0x88002038, kFrame = 0x88003e08;
    // Arms the hook: the state is taken at the next dispatcher entry.
    void request(Context& c);
    bool valid() const { return valid_; }
    bool just_saved() { bool s = just_saved_; just_saved_ = false; return s; }
    void bind(KinstBoard& board, Dcs* dcs, const std::vector<std::vector<uint8_t>>& sound_roms);
    // Puts the saved state back and returns the entry to restart from (run_entry).
    uint32_t restore(Context& c);

private:
    static void on_entry(Context& c, uint32_t entry);
    bool valid_ = false, requested_ = false, just_saved_ = false;
    uint32_t entry_ = 0;
    Context ctx_;
    std::optional<KinstBoard> board_;
    std::unique_ptr<Dcs> dcs_;
    KinstBoard* live_board_ = nullptr;
    Dcs* live_dcs_ = nullptr;
};

}  // namespace ki
