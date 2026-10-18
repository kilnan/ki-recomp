// Plays the recompiled Killer Instinct program in a window (SDL2).
//
// usage: ki_play --boot <dir> --disk <kinst.img> --rom <ki-l15d.u98> [--scale N]
//
// The guest runs on the main thread; at every VBLANK the board callback
// presents the framebuffer, reads input and paces emulation to the arcade's
// refresh rate (6.25 MHz / (406 x 261) = 58.98 Hz). No sound yet.
//
// Keys (player 1): arrows move, A S D = high quick/medium/fierce,
// Z X C = low quick/medium/fierce, 1 = start, 5 = coin.
// Player 2: I J K L move, Q W E / T Y U attacks... see kKeys. F2 = service
// (test switch), Esc quits, Tab toggles unthrottled speed.
#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "ki/kinst_board.h"
#include "ki/runtime.h"
#include "ki_boot.h"

namespace {

struct KeyBit {
    SDL_Scancode key;
    int player;  // 1 or 2
    uint32_t bit;
};

// Bits of the P1/P2 ports (active low), MAME kinst.cpp INPUT_PORTS_START(kinst).
const KeyBit kKeys[] = {
    {SDL_SCANCODE_A, 1, 0x001},     {SDL_SCANCODE_S, 1, 0x002},     {SDL_SCANCODE_D, 1, 0x004},
    {SDL_SCANCODE_Z, 1, 0x008},     {SDL_SCANCODE_X, 1, 0x010},     {SDL_SCANCODE_C, 1, 0x020},
    {SDL_SCANCODE_UP, 1, 0x040},    {SDL_SCANCODE_DOWN, 1, 0x080},  {SDL_SCANCODE_LEFT, 1, 0x100},
    {SDL_SCANCODE_RIGHT, 1, 0x200}, {SDL_SCANCODE_1, 1, 0x400},     {SDL_SCANCODE_5, 1, 0x800},
    {SDL_SCANCODE_F2, 1, 0x1000},   // service mode (test switch)
    {SDL_SCANCODE_Q, 2, 0x001},     {SDL_SCANCODE_W, 2, 0x002},     {SDL_SCANCODE_E, 2, 0x004},
    {SDL_SCANCODE_T, 2, 0x008},     {SDL_SCANCODE_Y, 2, 0x010},     {SDL_SCANCODE_U, 2, 0x020},
    {SDL_SCANCODE_I, 2, 0x040},     {SDL_SCANCODE_K, 2, 0x080},     {SDL_SCANCODE_J, 2, 0x100},
    {SDL_SCANCODE_L, 2, 0x200},     {SDL_SCANCODE_2, 2, 0x400},     {SDL_SCANCODE_6, 2, 0x800},
};

constexpr double kFrameSeconds = 1.0 / (6.25e6 / (406.0 * 261.0));

}  // namespace

int main(int argc, char** argv) {
    std::string boot, disk, rom;
    int scale = 3;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--boot") boot = v;
        else if (k == "--disk") disk = v;
        else if (k == "--rom") rom = v;
        else if (k == "--scale") scale = std::stoi(v);
    }
    if (boot.empty() || disk.empty() || rom.empty()) {
        std::fprintf(stderr, "usage: %s --boot <dir> --disk <img> --rom <u98> [--scale N]\n", argv[0]);
        return 2;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Killer Instinct (recompiled)", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 320 * scale, 240 * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, 320, 240);
    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 320, 240);

    static ki::Context c;
    uint32_t entry = ki::load_boot_state(c, boot);
    static ki::KinstBoard board(ki::read_file_or_die(disk), ki::read_file_or_die(rom));
    c.io = &board;

    bool turbo = false;
    auto next_frame = std::chrono::steady_clock::now();
    board.on_vblank = [&](ki::Context& ctx, uint64_t frame) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
                SDL_Quit();
                std::exit(0);
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_TAB && !ev.key.repeat) turbo = !turbo;
        }
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        uint32_t p1 = 0xffffffff, p2 = 0xffffffff;
        for (const KeyBit& k : kKeys)
            if (keys[k.key]) (k.player == 1 ? p1 : p2) &= ~k.bit;
        board.p1 = p1;
        board.p2 = p2;

        if (!turbo || frame % 8 == 0) {
            auto rgb = board.framebuffer_rgb(ctx);
            SDL_UpdateTexture(texture, nullptr, rgb.data(), 320 * 3);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }
        if (!turbo) {
            next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kFrameSeconds));
            auto now = std::chrono::steady_clock::now();
            if (next_frame > now) std::this_thread::sleep_until(next_frame);
            else if (now - next_frame > std::chrono::milliseconds(100)) next_frame = now;  // fell behind: resync
        }
    };
    ki::schedule(c);
    bool ok = ki::run_entry(c, entry);
    SDL_Quit();
    return ok ? 0 : 1;
}
