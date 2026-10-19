// The player's own copy of the game: finding, reading and verifying it.
//
// A folder is searched for the MAME `kinst` set as MAME keeps it: `kinst.zip`
// (or its nine files loose) and `kinst.chd`, also inside a `kinst/`
// subfolder. Every file is checked against the SHA1 from MAME's kinst.cpp
// (revision L1.5D), because the recompiled code only matches that revision.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ki {

struct GameFiles {
    std::vector<uint8_t> boot_rom;                // ki-l15d.u98
    std::vector<std::vector<uint8_t>> sound_roms; // u10-l1 ... u36-l1, in board order
    std::vector<uint8_t> disk;                    // kinst.chd content (raw sectors)
};

// Loads and verifies the game from `dir`. On failure returns false and a
// message for the player in `error` (what is missing or wrong, and what to use).
// When the folder has no kinst.chd but a .zip that contains one (as CHDs are
// often downloaded), it is extracted once into `cache_dir` and used from there.
bool load_game_files(const std::string& dir, GameFiles& out, std::string& error,
                     const std::string& cache_dir = "");

// SHA1 of a buffer, lowercase hex.
std::string sha1_hex(const uint8_t* data, size_t size);

}  // namespace ki
