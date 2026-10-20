// Override around the fight setup (built with --override, see CMakeLists.txt).
#include "ki/hooks.h"
#include "recompiled.h"

namespace ki::gen {

// Loads the stage of the coming fight; the host may first choose the stage or
// the opponent (ki_play --opponent / --stage).
void func_8800e188(ki::Context& c) {
    if (ki::hooks::fight_setup) ki::hooks::fight_setup(c);
    func_8800e188_original(c);
}

}  // namespace ki::gen
