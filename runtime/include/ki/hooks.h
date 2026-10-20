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

}  // namespace ki::hooks
