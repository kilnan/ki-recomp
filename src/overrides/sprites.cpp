// Overrides around sprite and stage drawing (built with --override, see CMakeLists.txt).
#include "ki/hooks.h"
#include "recompiled.h"

namespace ki::gen {

// The scaled sprite blitter: lets the host observe each sprite drawn, then
// draws it with the recompiled original.
void func_880107a8(ki::Context& c) {
    if (ki::hooks::sprite) ki::hooks::sprite(c);
    func_880107a8_original(c);
    if (ki::hooks::sprite_after) ki::hooks::sprite_after(c);
}

// The stage panorama copy (sky and buildings of some stages): lets the host
// replace what was copied with its HD panorama.
void func_88018c98(ki::Context& c) {
    if (ki::hooks::panorama) ki::hooks::panorama(c);
    func_88018c98_original(c);
    if (ki::hooks::panorama_after) ki::hooks::panorama_after(c);
}

// The pre-rendered stage renderer (whole screen from a stage frame).
void func_88030b9c(ki::Context& c) {
    if (ki::hooks::prerendered) ki::hooks::prerendered(c);
    func_88030b9c_original(c);
    if (ki::hooks::prerendered_after) ki::hooks::prerendered_after(c);
}

}  // namespace ki::gen
