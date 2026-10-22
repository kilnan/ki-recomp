#include "ki_pad_ui.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ki::pad {

namespace {

// A 5x7 font for the characters the screen uses (upper case; '#' = lit).
struct Glyph {
    char ch;
    const char* rows[7];
};
const Glyph kFont[] = {
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"####.", "....#", "....#", ".###.", "....#", "....#", "####."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {':', {".....", "..#..", "..#..", ".....", "..#..", "..#..", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
    {'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
    {'*', {".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'\'', {"..#..", "..#..", ".#...", ".....", ".....", ".....", "....."}},
};

// Draws text with its top-left at (x, y) in a 320x240 layout scaled by s.
void text(SDL_Renderer* r, int x, int y, const std::string& str, float s, SDL_Color col) {
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
    for (char raw : str) {
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        for (const Glyph& g : kFont) {
            if (g.ch != ch) continue;
            for (int gy = 0; gy < 7; ++gy)
                for (int gx = 0; gx < 5; ++gx)
                    if (g.rows[gy][gx] == '#') {
                        SDL_FRect px{(x + gx) * s, (y + gy) * s, s, s};
                        SDL_RenderFillRectF(r, &px);
                    }
            break;
        }
        x += 6;
    }
}

void box(SDL_Renderer* r, int x, int y, int w, int h, float s, SDL_Color col) {
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
    SDL_FRect rect{x * s, y * s, w * s, h * s};
    SDL_RenderFillRectF(r, &rect);
}

const SDL_Color kWhite{235, 235, 235, 255}, kGrey{140, 140, 150, 255}, kYellow{250, 210, 60, 255},
    kGreen{90, 230, 110, 255}, kRed{240, 90, 80, 255}, kPanel{10, 10, 20, 245}, kBar{60, 50, 120, 255};

}  // namespace

void SetupScreen::bind(const Input& in) {
    Action& a = actions_[selected_];
    // An input drives one cabinet input only: taken away from any other.
    for (Action& other : actions_)
        other.inputs.erase(std::remove(other.inputs.begin(), other.inputs.end(), in), other.inputs.end());
    a.inputs.push_back(in);
    save_bindings(save_path_, actions_);
    message_ = describe(in) + " -> " + a.name + " (SAVED)";
    capturing_ = false;
}

bool SetupScreen::handle(const SDL_Event& ev) {
    if (!open_) return false;
    const int n = static_cast<int>(actions_.size());
    if (capturing_) {
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            capturing_ = false;
            message_ = "CANCELLED";
            return true;
        }
        if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
            bind({Input::kButton, ev.cbutton.button, 1});
            last_nav_ = SDL_GetTicks();
            return true;
        }
        if (ev.type == SDL_CONTROLLERAXISMOTION && std::abs(ev.caxis.value) > 24000) {
            bind({Input::kAxis, ev.caxis.axis, ev.caxis.value < 0 ? -1 : 1});
            last_nav_ = SDL_GetTicks();
            return true;
        }
        return ev.type == SDL_KEYDOWN || ev.type == SDL_CONTROLLERBUTTONUP || ev.type == SDL_CONTROLLERAXISMOTION;
    }
    if (ev.type == SDL_KEYDOWN) {
        switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_UP: selected_ = (selected_ + n - 1) % n; return true;
            case SDL_SCANCODE_DOWN: selected_ = (selected_ + 1) % n; return true;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_SPACE: capturing_ = true, message_.clear(); return true;
            case SDL_SCANCODE_BACKSPACE:
            case SDL_SCANCODE_DELETE:
                actions_[selected_].inputs.clear();
                save_bindings(save_path_, actions_);
                message_ = std::string(actions_[selected_].name) + " CLEARED (SAVED)";
                return true;
            case SDL_SCANCODE_D:
                actions_ = default_bindings();
                save_bindings(save_path_, actions_);
                message_ = "DEFAULTS RESTORED (SAVED)";
                return true;
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_G: toggle(); return true;
            default: return true;  // the game does not see keys while the screen is open
        }
    }
    // With a controller: d-pad moves, A changes, B closes (after a short pause,
    // so the button that just bound something does not also act).
    if (ev.type == SDL_CONTROLLERBUTTONDOWN && SDL_GetTicks() - last_nav_ > 300) {
        switch (ev.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: selected_ = (selected_ + n - 1) % n; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: selected_ = (selected_ + 1) % n; break;
            case SDL_CONTROLLER_BUTTON_A: capturing_ = true, message_.clear(); break;
            case SDL_CONTROLLER_BUTTON_B: toggle(); break;
            default: break;
        }
        return true;
    }
    return ev.type == SDL_KEYUP || ev.type == SDL_CONTROLLERBUTTONUP || ev.type == SDL_CONTROLLERAXISMOTION;
}

void SetupScreen::draw(SDL_Renderer* r, const std::vector<SDL_GameController*>& pads) {
    if (!open_) return;
    // ki_play's renderer has a 320x240 logical size: the layout is in those units.
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    float s = 1.0f;
    if (lw == 0) {
        int ww = 0, wh = 0;
        SDL_GetRendererOutputSize(r, &ww, &wh);
        s = std::min(ww / 320.0f, wh / 240.0f);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    box(r, 0, 0, 320, 240, s, kPanel);
    text(r, 8, 6, "CONTROLLER SETUP", s, kYellow);
    text(r, 214, 6, "G/ESC: CLOSE", s, kGrey);

    // Connected controllers.
    std::vector<SDL_GameController*> live;
    for (SDL_GameController* p : pads)
        if (p && SDL_GameControllerGetAttached(p)) live.push_back(p);
    for (int i = 0; i < 2; ++i) {
        std::string line = "PLAYER " + std::to_string(i + 1) + ": ";
        if (i < static_cast<int>(live.size())) {
            const char* name = SDL_GameControllerName(live[i]);
            line += name ? name : "CONTROLLER";
        } else {
            line += i == 0 ? "NO CONTROLLER (KEYBOARD)" : "-";
        }
        if (line.size() > 52) line.resize(52);
        text(r, 8, 20 + i * 10, line, s, i < static_cast<int>(live.size()) ? kGreen : kGrey);
    }

    // Bindings, with the live state of every controller.
    text(r, 8, 44, "INPUT", s, kGrey);
    text(r, 92, 44, "CONTROLLER BUTTONS", s, kGrey);
    text(r, 272, 44, "P1 P2", s, kGrey);
    for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
        const Action& a = actions_[i];
        int y = 55 + i * 12;
        if (i == selected_) box(r, 4, y - 2, 312, 11, s, kBar);
        text(r, 8, y, a.name, s, i == selected_ ? kYellow : kWhite);
        std::string binds;
        if (i == selected_ && capturing_) {
            binds = "PRESS A BUTTON... (ESC CANCELS)";
        } else {
            for (const Input& in : a.inputs) binds += (binds.empty() ? "" : ", ") + describe(in);
            if (binds.empty()) binds = "(NONE)";
        }
        if (binds.size() > 29) binds = binds.substr(0, 28) + "+";
        text(r, 92, y, binds, s, i == selected_ && capturing_ ? kYellow : (a.inputs.empty() ? kRed : kWhite));
        for (int p = 0; p < 2 && p < static_cast<int>(live.size()); ++p) {
            bool on = false;
            for (const Input& in : a.inputs) on = on || active(live[p], in);
            box(r, 274 + p * 18, y, 7, 7, s, on ? kGreen : SDL_Color{50, 50, 60, 255});
        }
    }
    text(r, 8, 202, message_, s, kGreen);
    text(r, 8, 216, "ARROWS/D-PAD: CHOOSE   ENTER/A: CHANGE", s, kGrey);
    text(r, 8, 227, "DEL: CLEAR   D: DEFAULTS   B: CLOSE", s, kGrey);
}

}  // namespace ki::pad
