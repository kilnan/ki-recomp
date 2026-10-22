// Gamepad bindings for ki_play: which controller input drives which cabinet
// input, editable in the in-game setup screen (src/ki_pad_ui.cpp) and kept in
// the preferences folder (pad_map.txt). The same bindings apply to every pad;
// the first pad is player 1, the second player 2.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ki::pad {

// A controller input: a button, or an axis pushed past half way in one direction.
struct Input {
    enum Kind { kButton, kAxis } kind = kButton;
    int index = 0;  // SDL_GameControllerButton or SDL_GameControllerAxis
    int dir = 1;    // axes: +1 or -1
    bool operator==(const Input& o) const { return kind == o.kind && index == o.index && dir == o.dir; }
};

struct Action {
    const char* name;  // shown in the setup screen
    uint32_t bit;      // cabinet input bit (active low in the board ports)
    std::vector<Input> inputs;
};

// The 12 cabinet inputs, in the order the setup screen lists them.
std::vector<Action> default_bindings();

// pad_map.txt: one line per action, "<bit hex> <input> <input> ...", inputs
// written as button:<name> or axis:+<name> / axis:-<name> (SDL names).
std::vector<Action> load_bindings(const std::string& path);
void save_bindings(const std::string& path, const std::vector<Action>& actions);

// Cabinet bits currently pressed on this pad.
uint32_t pressed(SDL_GameController* pad, const std::vector<Action>& actions);
bool active(SDL_GameController* pad, const Input& in);

std::string describe(const Input& in);  // e.g. "A", "RB", "LEFT STICK UP", "RT"

}  // namespace ki::pad
