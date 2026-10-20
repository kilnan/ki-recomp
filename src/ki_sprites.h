// Sprite extraction for HD replacement work: every sprite the game draws with
// the blitter 0x880107a8 is redrawn alone by the recompiled blitter itself, on
// two different backgrounds, so its pixels can be read back without knowing
// the encoding. The game's state is restored afterwards, so the run continues
// exactly as without extraction.
#pragma once

#include <cstdint>
#include <string>

#include "ki/runtime.h"

namespace ki {

// Installs the sprite hook; unique sprites (by header and data content) are
// written to `dir` as PPM (opaque pixels; others in key colour 0x00ff00).
void start_sprite_extraction(const std::string& dir);
void stop_sprite_extraction();
// Number of distinct sprites written so far.
unsigned extracted_sprites();

}  // namespace ki
