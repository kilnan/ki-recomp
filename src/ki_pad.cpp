#include "ki_pad.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ki::pad {

namespace {

constexpr int kAxisThreshold = 16000;

Input button(SDL_GameControllerButton b) { return {Input::kButton, b, 1}; }
Input axis(SDL_GameControllerAxis a, int dir) { return {Input::kAxis, a, dir}; }

std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

}  // namespace

std::vector<Action> default_bindings() {
    // Xbox layout names (SDL maps other pads, Nimbus included, onto it): top row
    // X Y RB = high quick/medium/fierce, bottom row A B RT = low; d-pad or left
    // stick moves; Start (Nimbus: Menu) = start; Back or LB = coin (the Nimbus
    // has no Back button).
    return {
        {"UP", 0x040, {button(SDL_CONTROLLER_BUTTON_DPAD_UP), axis(SDL_CONTROLLER_AXIS_LEFTY, -1)}},
        {"DOWN", 0x080, {button(SDL_CONTROLLER_BUTTON_DPAD_DOWN), axis(SDL_CONTROLLER_AXIS_LEFTY, 1)}},
        {"LEFT", 0x100, {button(SDL_CONTROLLER_BUTTON_DPAD_LEFT), axis(SDL_CONTROLLER_AXIS_LEFTX, -1)}},
        {"RIGHT", 0x200, {button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), axis(SDL_CONTROLLER_AXIS_LEFTX, 1)}},
        {"HIGH QUICK", 0x001, {button(SDL_CONTROLLER_BUTTON_X)}},
        {"HIGH MEDIUM", 0x002, {button(SDL_CONTROLLER_BUTTON_Y)}},
        {"HIGH FIERCE", 0x004, {button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)}},
        {"LOW QUICK", 0x008, {button(SDL_CONTROLLER_BUTTON_A)}},
        {"LOW MEDIUM", 0x010, {button(SDL_CONTROLLER_BUTTON_B)}},
        {"LOW FIERCE", 0x020, {axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)}},
        {"START", 0x400, {button(SDL_CONTROLLER_BUTTON_START)}},
        {"COIN", 0x800, {button(SDL_CONTROLLER_BUTTON_BACK), button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)}},
    };
}

std::vector<Action> load_bindings(const std::string& path) {
    std::vector<Action> actions = default_bindings();
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string bit_hex, word;
        if (!(ls >> bit_hex)) continue;
        uint32_t bit = static_cast<uint32_t>(std::stoul(bit_hex, nullptr, 16));
        for (Action& a : actions) {
            if (a.bit != bit) continue;
            a.inputs.clear();
            while (ls >> word) {
                if (word.rfind("button:", 0) == 0) {
                    SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(word.c_str() + 7);
                    if (b != SDL_CONTROLLER_BUTTON_INVALID) a.inputs.push_back(button(b));
                } else if (word.rfind("axis:", 0) == 0 && word.size() > 6) {
                    SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(word.c_str() + 6);
                    if (ax != SDL_CONTROLLER_AXIS_INVALID) a.inputs.push_back(axis(ax, word[5] == '-' ? -1 : 1));
                }
            }
        }
    }
    return actions;
}

void save_bindings(const std::string& path, const std::vector<Action>& actions) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        for (const Action& a : actions) {
            char bit[16];
            std::snprintf(bit, sizeof bit, "%03x", a.bit);
            f << bit;
            for (const Input& in : a.inputs) {
                if (in.kind == Input::kButton)
                    f << " button:" << SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(in.index));
                else
                    f << " axis:" << (in.dir < 0 ? '-' : '+')
                      << SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(in.index));
            }
            f << "  # " << a.name << "\n";
        }
    }
    std::rename(tmp.c_str(), path.c_str());
}

bool active(SDL_GameController* pad, const Input& in) {
    if (in.kind == Input::kButton) return SDL_GameControllerGetButton(pad, static_cast<SDL_GameControllerButton>(in.index));
    int v = SDL_GameControllerGetAxis(pad, static_cast<SDL_GameControllerAxis>(in.index));
    return in.dir > 0 ? v > kAxisThreshold : v < -kAxisThreshold;
}

uint32_t pressed(SDL_GameController* pad, const std::vector<Action>& actions) {
    uint32_t bits = 0;
    for (const Action& a : actions)
        for (const Input& in : a.inputs)
            if (active(pad, in)) bits |= a.bit;
    return bits;
}

std::string describe(const Input& in) {
    if (in.kind == Input::kButton) {
        switch (in.index) {
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-PAD UP";
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-PAD DOWN";
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-PAD LEFT";
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-PAD RIGHT";
            case SDL_CONTROLLER_BUTTON_GUIDE: return "HOME";
            default: {
                const char* s = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(in.index));
                return s ? upper(s) : "?";
            }
        }
    }
    switch (in.index) {
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "LT";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "RT";
        case SDL_CONTROLLER_AXIS_LEFTX: return in.dir < 0 ? "L-STICK LEFT" : "L-STICK RIGHT";
        case SDL_CONTROLLER_AXIS_LEFTY: return in.dir < 0 ? "L-STICK UP" : "L-STICK DOWN";
        case SDL_CONTROLLER_AXIS_RIGHTX: return in.dir < 0 ? "R-STICK LEFT" : "R-STICK RIGHT";
        case SDL_CONTROLLER_AXIS_RIGHTY: return in.dir < 0 ? "R-STICK UP" : "R-STICK DOWN";
        default: return "?";
    }
}

}  // namespace ki::pad
