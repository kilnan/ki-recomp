// Sprite extraction for HD replacement work: every sprite the game draws with
// the blitter 0x880107a8 is redrawn alone by the recompiled blitter itself, on
// two different backgrounds, so its pixels can be read back without knowing
// the encoding. The game's state is restored afterwards, so the run continues
// exactly as without extraction.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ki/runtime.h"

namespace ki {

// Installs the sprite hook; unique sprites (by header and data content) are
// written to `dir` as PPM (opaque pixels; others in key colour 0x00ff00).
void start_sprite_extraction(const std::string& dir);
void stop_sprite_extraction();
// Also extract every sprite of the character data at DRAM [base, base+size),
// at the first normal draw of one of them (its mode), with each of `palettes`
// (palette numbers, see 0x88001ffc) or, if empty, with that call's palette.
void extract_character(uint32_t base, uint32_t size, const std::vector<int>& palettes = {});
// Number of distinct sprites written so far.
unsigned extracted_sprites();

// Identity of the sprite of the current blitter call: its header and data
// (first 512 bytes) and the palette (zero page, first 256 bytes). HD packs are keyed by it.
uint64_t sprite_key(const Context& c);

}  // namespace ki
