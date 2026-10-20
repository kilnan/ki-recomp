// Hooks the host program can install, called by hand-written overrides of
// recompiled functions (src/overrides/). Each hook sees the guest registers
// at the entry of the overridden function; all are optional.
#pragma once

namespace ki {
struct Context;
}

namespace ki::hooks {

using Fn = void (*)(Context&);

// Entry of the scaled sprite blitter 0x880107a8 (hand-written MIPS, custom
// register arguments; see docs/TECHNICAL_NOTE.md "Sprite blitter").
extern Fn sprite;
// After the blitter has drawn (same call; guest registers as at the entry are
// not preserved, so a host that needs the arguments keeps them from `sprite`).
extern Fn sprite_after;
// Fight setup, just before the stage (0x88086238) and then the two fighters
// (0x8808f5f0 / 0x8808f5f4) are loaded: entry of 0x8800e188, whose only
// caller is the fight setup at 0x88000680. A host may change them here.
extern Fn fight_setup;
// Stage panorama copy 0x88018c98 (v0 framebuffer, v1 panorama row 0, a0 first
// column, a1 first row, a2 width, a3 rows; each row is 4 pixels of margin then
// the a2 columns that wrap around):
// entry and after the copy.
extern Fn panorama;
extern Fn panorama_after;
// Pre-rendered stage renderer 0x88030b9c: draws the whole screen from one of a
// stage's 320x240 frames, each 8x8 block shifted horizontally by a fraction of
// a pixel that grows with the block's depth (parallax). Entry and after.
extern Fn prerendered;
extern Fn prerendered_after;
// End-of-fight screens: 0x88023008 saves the shown frame darkened (half
// brightness) at 0x884a0000 and copies it into the target framebuffer
// (pointer at 0x88040dc8); 0x8802309c copies the saved frame there again.
// Called after each.
// 0x8803108c, called by the renderer before each row of 8x8 blocks: builds the
// row's 320x8 pixels at 0x8002ce00 from 160 chunk indices (18 bits each, read
// from f0/f1, relative to f2), or, if f4 < 0, points gp into a frame directly.
extern Fn prerendered_setup;
extern Fn prerendered_setup_after;
extern Fn screen_save_after;
extern Fn screen_restore_after;

}  // namespace ki::hooks
