// In-game controller setup screen for ki_play (G opens and closes it; the
// game is paused while it is open): lists the connected controllers, shows
// live which cabinet input each one is pressing, and lets each cabinet input
// be bound to other controller buttons. Bindings are saved to pad_map.txt.
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "ki_pad.h"

namespace ki::pad {

class SetupScreen {
public:
    SetupScreen(std::vector<Action>& actions, std::string save_path) : actions_(actions), save_path_(std::move(save_path)) {}
    bool open() const { return open_; }
    void toggle() { open_ = !open_, capturing_ = false; }
    // Keyboard and controller events while open; false if the event is not for the screen.
    bool handle(const SDL_Event& ev);
    // Draws over whatever the renderer shows (call before SDL_RenderPresent).
    void draw(SDL_Renderer* r, const std::vector<SDL_GameController*>& pads);

private:
    void bind(const Input& in);
    std::vector<Action>& actions_;
    std::string save_path_;
    bool open_ = false, capturing_ = false;
    int selected_ = 0;
    std::string message_;
    // A rising edge for the menu's own navigation with a controller.
    Uint32 last_nav_ = 0;
};

}  // namespace ki::pad
