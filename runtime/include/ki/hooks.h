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

}  // namespace ki::hooks
