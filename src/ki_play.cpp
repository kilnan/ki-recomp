// Plays the recompiled Killer Instinct program in a window (SDL2).
//
// usage: ki_play [--data <dir>] [--scale N] [--mute] [--opponent NAMES] [--stage N,...]
//        ki_play --boot <snapshot dir|rom> --disk <kinst.img> --rom <ki-l15d.u98>
//                [--sound-roms <dir>] [--scale N] [--mute]      (development)
//
// Normally the program starts like the arcade board, at the reset vector,
// from the player's MAME `kinst` set: a folder with kinst.zip (or its files)
// and kinst.chd, given with --data, dropped onto the window, or remembered
// from the last run. Every file is checked against MAME's SHA1. Sectors the
// game writes (settings, high scores) are kept in the user's preferences
// folder and put back on the next start.
//
// The guest runs on the main thread; at every VBLANK the board callback
// presents the framebuffer, reads input, queues the DCS samples produced
// during the frame and paces emulation to the arcade's refresh rate
// (6.25 MHz / (406 x 261) = 58.98 Hz).
//
// Keys (player 1): arrows move, A S D = high quick/medium/fierce,
// Z X C = low quick/medium/fierce, 1 = start, 5 = coin.
// Player 2: I J K L move, Q W E / T Y U attacks... see kKeys. F2 = service
// (test switch), Esc quits, Tab toggles unthrottled speed (no sound while on).
// Gamepads work too, see kPadButtons. F5 saves the state, F9 loads it.
// P pauses, R resets. --opponent NAME[,NAME...] and --stage N[,N...] choose the coming
// fights (in turn, one per fight); F6 / F7 cycle the opponent / stage of the
// next fight (shown in the title bar).
#include <SDL.h>
#ifdef _WIN32
#include <process.h>
#define execv _execv
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ki/kinst_board.h"
#include "ki/runtime.h"
#include "ki_boot.h"
#include "ki_fight.h"
#ifdef KI_HAVE_FILES
#include "ki_files.h"
#endif
#ifdef KI_HAVE_HD
#include "ki_hd.h"
#endif

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

// Gamepads (SDL GameController, Xbox layout): the first pad is player 1, the
// second player 2. Top row X Y RB = high quick/medium/fierce, bottom row
// A B RT = low quick/medium/fierce; d-pad or left stick moves; Start = start,
// Back = coin.
struct PadBit {
    SDL_GameControllerButton button;
    uint32_t bit;
};
const PadBit kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_X, 0x001},          {SDL_CONTROLLER_BUTTON_Y, 0x002},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x004}, {SDL_CONTROLLER_BUTTON_A, 0x008},
    {SDL_CONTROLLER_BUTTON_B, 0x010},          {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x040},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x080},  {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x100},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x200}, {SDL_CONTROLLER_BUTTON_START, 0x400},
    {SDL_CONTROLLER_BUTTON_BACK, 0x800},
};

uint32_t pad_bits(SDL_GameController* pad) {
    uint32_t bits = 0;
    for (const PadBit& b : kPadButtons)
        if (SDL_GameControllerGetButton(pad, b.button)) bits |= b.bit;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) bits |= 0x020;
    const int dead = 16000;
    int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (y < -dead) bits |= 0x040;
    if (y > dead) bits |= 0x080;
    if (x < -dead) bits |= 0x100;
    if (x > dead) bits |= 0x200;
    return bits;
}

constexpr double kFrameSeconds = 1.0 / (6.25e6 / (406.0 * 261.0));
constexpr const char* kTitle = "Killer Instinct (recompiled)";

std::string pref_path() {
    char* p = SDL_GetPrefPath("kilnan", "KIRecompiled");
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}

std::string read_text(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text << "\n";
}

// Disk sectors written by the game: [u32 lba][512 bytes] records.
void apply_saved_sectors(const std::string& path, std::vector<uint8_t>& disk) {
    std::ifstream f(path, std::ios::binary);
    uint8_t rec[4 + 512];
    while (f.read(reinterpret_cast<char*>(rec), sizeof rec)) {
        uint32_t lba = rec[0] | rec[1] << 8 | rec[2] << 16 | static_cast<uint32_t>(rec[3]) << 24;
        if ((static_cast<uint64_t>(lba) + 1) * 512 <= disk.size()) std::memcpy(&disk[lba * 512ull], rec + 4, 512);
    }
}

void save_sectors(const std::string& path, const ki::KinstBoard& board, const std::vector<uint32_t>& earlier) {
    std::vector<uint32_t> all = earlier;
    all.insert(all.end(), board.written_sectors().begin(), board.written_sectors().end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        for (uint32_t lba : all) {
            uint8_t head[4] = {static_cast<uint8_t>(lba), static_cast<uint8_t>(lba >> 8),
                               static_cast<uint8_t>(lba >> 16), static_cast<uint8_t>(lba >> 24)};
            f.write(reinterpret_cast<const char*>(head), 4);
            f.write(reinterpret_cast<const char*>(&board.disk()[lba * 512ull]), 512);
        }
    }
    std::rename(tmp.c_str(), path.c_str());
}

std::vector<uint32_t> saved_lbas(const std::string& path) {
    std::vector<uint32_t> out;
    std::ifstream f(path, std::ios::binary);
    uint8_t rec[4 + 512];
    while (f.read(reinterpret_cast<char*>(rec), sizeof rec))
        out.push_back(rec[0] | rec[1] << 8 | rec[2] << 16 | static_cast<uint32_t>(rec[3]) << 24);
    return out;
}

}  // namespace

char** g_argv = nullptr;  // for the reset key

int main(int argc, char** argv) {
    g_argv = argv;
    std::string boot, disk, rom, sound_roms, data, hd_pack;
    static ki::FightChoice fight;
    int scale = 3;
    bool mute = false;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--mute") { mute = true; continue; }
        if (i + 1 >= argc) break;
        std::string v = argv[++i];
        if (k == "--boot") boot = v;
        else if (k == "--disk") disk = v;
        else if (k == "--rom") rom = v;
        else if (k == "--scale") scale = std::stoi(v);
        else if (k == "--sound-roms") sound_roms = v;
        else if (k == "--data") data = v;
        else if (k == "--hd-pack") hd_pack = v;
        else if (k == "--opponent" && !ki::parse_characters(v, fight.opponents)) {
            std::fprintf(stderr, "--opponent: names or numbers 0-10 (jago combo spinal thunder glacius fulgore cinder sabrewulf orchid riptor eyedol)\n");
            return 1;
        } else if (k == "--stage" && !ki::parse_numbers(v, ki::kStageCount, fight.stages)) {
            std::fprintf(stderr, "--stage: numbers 0-15\n");
            return 1;
        }
    }
    ki::install_fight_choice(&fight);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(kTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale,
                                          240 * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, 320, 240);
    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    SDL_Texture* hd_texture = nullptr;  // 4x picture when an HD pack is loaded (H toggles)

    // ---- the game's files ----
    std::vector<uint8_t> disk_image, boot_rom;
    std::vector<std::vector<uint8_t>> sound;
    const bool dev = !boot.empty();
    std::string saves;  // file with the sectors the game wrote (player mode only)
    std::vector<uint32_t> earlier_saves;
    if (dev) {
        if (disk.empty() || rom.empty()) {
            std::fprintf(stderr, "usage: %s --boot <dir|rom> --disk <img> --rom <u98> [--sound-roms <dir>]\n", argv[0]);
            return 2;
        }
        disk_image = ki::read_file_or_die(disk);
        boot_rom = ki::read_file_or_die(rom);
        if (sound_roms.empty()) {
            size_t slash = rom.find_last_of('/');
            sound_roms = slash == std::string::npos ? "." : rom.substr(0, slash);
        }
        if (FILE* f = std::fopen((sound_roms + "/u10-l1").c_str(), "rb")) {
            std::fclose(f);
            sound = ki::read_sound_roms_or_die(sound_roms);
        }
    } else {
#ifdef KI_HAVE_FILES
        const std::string prefs = pref_path();
        if (data.empty()) data = read_text(prefs + "data_dir.txt");
        ki::GameFiles files;
        std::string error;
        bool ok = !data.empty() && ki::load_game_files(data, files, error, prefs);
        if (!ok) {
            std::string msg =
                (data.empty() ? std::string("Drag the folder with your MAME 'kinst' set onto this window.")
                              : "The game files in " + data + " cannot be used:\n" + error +
                                    "\n\nDrag another folder onto this window.") +
                "\n\nThe folder must contain kinst.zip (BIOS v1.5d) and kinst.chd (or a .zip with it).";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, kTitle, msg.c_str(), window);
            SDL_SetWindowTitle(window, "Killer Instinct (recompiled): drop the kinst folder here");
            SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
            while (!ok) {
                SDL_RenderClear(renderer);
                SDL_RenderPresent(renderer);
                SDL_Event ev;
                if (!SDL_WaitEvent(&ev)) continue;
                if (ev.type == SDL_QUIT) return 0;
                if (ev.type != SDL_DROPFILE) continue;
                std::string dropped = ev.drop.file;
                SDL_free(ev.drop.file);
                // A dropped file (kinst.zip, kinst.chd) stands for its folder.
                if (!ki::load_game_files(dropped, files, error, prefs)) {
                    size_t slash = dropped.find_last_of('/');
                    std::string parent = slash == std::string::npos ? dropped : dropped.substr(0, slash);
                    std::string e2;
                    if (ki::load_game_files(parent, files, e2, prefs)) dropped = parent;
                    else {
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, kTitle, error.c_str(), window);
                        continue;
                    }
                }
                data = dropped;
                ok = true;
            }
            SDL_SetWindowTitle(window, kTitle);
        }
        std::error_code ec;
        std::string absolute = std::filesystem::absolute(data, ec).lexically_normal().string();
        write_text(prefs + "data_dir.txt", ec ? data : absolute);
        disk_image = std::move(files.disk);
        boot_rom = std::move(files.boot_rom);
        sound = std::move(files.sound_roms);
        saves = prefs + "disk_writes.bin";
        earlier_saves = saved_lbas(saves);
        apply_saved_sectors(saves, disk_image);
#else
        std::fprintf(stderr, "built without KI_WITH_CHD: use --boot/--disk/--rom\n");
        return 2;
#endif
    }

    static ki::Context c;
    // --boot rom (and player mode): start at the reset vector, like the real board.
    const bool cold = !dev || boot == "rom";
    uint32_t entry = cold ? ki::cold_reset(c) : ki::load_boot_state(c, boot);
    static ki::KinstBoard board(std::move(disk_image), std::move(boot_rom));
    c.io = &board;
    if (cold) board.set_cycles_to_vblank(0);
    static std::unique_ptr<ki::Dcs> dcs;
    SDL_AudioDeviceID audio = 0;
    if (!sound.empty()) {
        dcs = std::make_unique<ki::Dcs>(sound);
        board.dcs = dcs.get();
        if (!mute) {
            // KI's DCS program drives the DAC at 31250 Hz; SDL converts to the device rate.
            SDL_AudioSpec want{};
            want.freq = 31250;
            want.format = AUDIO_S16SYS;
            want.channels = 1;
            want.samples = 1024;
            audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
            if (!audio) std::fprintf(stderr, "no audio: %s\n", SDL_GetError());
            else SDL_PauseAudioDevice(audio, 0);
        }
    }

#ifdef KI_HAVE_HD
    static ki::HdRenderer hd;
    bool hd_on = false;
    if (!hd_pack.empty()) {
        if (hd.load_pack(hd_pack)) {
            hd.attach();
            hd_on = true;
            hd_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                           ki::HdRenderer::kW, ki::HdRenderer::kH);
            std::fprintf(stderr, "HD pack: %zu sprites, %zu shapes (H switches HD on and off)\n", hd.pack_size(), hd.shape_count());
        } else {
            std::fprintf(stderr, "no HD pack in %s\n", hd_pack.c_str());
        }
    }
#endif
    static ki::SaveState state;  // F5 / F9
    state.bind(board, dcs.get(), sound);

    bool turbo = false;
    std::vector<SDL_GameController*> pads;  // opened as SDL reports them (SDL_CONTROLLERDEVICEADDED)
    uint64_t saved_writes = 0;
    uint64_t title_reset_frame = 0;
    auto save = [&]() {
        if (!saves.empty() && board.disk_writes() != saved_writes) {
            save_sectors(saves, board, earlier_saves);
            saved_writes = board.disk_writes();
        }
    };
    auto next_frame = std::chrono::steady_clock::now();
    bool paused = false;
    auto present = [&](ki::Context& ctx) {
        SDL_RenderClear(renderer);
#ifdef KI_HAVE_HD
        // The HD copy is kept up to date even when not shown, so H switches instantly.
        if (hd_texture) {
            const std::vector<uint32_t>& px = hd.frame(ctx, board.displayed_bank());
            if (hd_on) {
                SDL_UpdateTexture(hd_texture, nullptr, px.data(), ki::HdRenderer::kW * 4);
                SDL_RenderCopy(renderer, hd_texture, nullptr, nullptr);
            }
        }
        if (!hd_on)
#endif
        {
            auto rgb = board.framebuffer_rgb(ctx);
            SDL_UpdateTexture(texture, nullptr, rgb.data(), 320 * 3);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        }
        SDL_RenderPresent(renderer);
    };
    board.on_vblank = [&](ki::Context& ctx, uint64_t frame) {
        SDL_Event ev;
        for (bool first = true; first || paused; first = false) {
            if (!first) {
                // Paused: the frame stays on screen (H still switches HD, to compare), the game does not run.
                present(ctx);
                SDL_Delay(16);
                next_frame = std::chrono::steady_clock::now();
            }
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
                    save();
                    SDL_Quit();
                    std::exit(0);
                }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_TAB && !ev.key.repeat) turbo = !turbo;
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                    (ev.key.keysym.scancode == SDL_SCANCODE_F6 || ev.key.keysym.scancode == SDL_SCANCODE_F7)) {
                    // Next fight: off -> 0 -> 1 ... -> off. Replaces any --opponent / --stage list.
                    bool opp = ev.key.keysym.scancode == SDL_SCANCODE_F6;
                    std::vector<int>& list = opp ? fight.opponents : fight.stages;
                    int limit = opp ? 11 : ki::kStageCount;
                    int next = list.size() == 1 ? list[0] + 1 : 0;
                    list.clear();
                    if (next < limit) list.push_back(next);
                    fight.fights = 0;
                    std::string t = std::string(kTitle) + ": next fight: opponent " +
                                    (fight.opponents.empty() ? "(game)" : ki::kCharacterNames[fight.opponents[0]]) +
                                    ", stage " + (fight.stages.empty() ? "(game)" : std::to_string(fight.stages[0]));
                    SDL_SetWindowTitle(window, t.c_str());
                }
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_R) {
                    // Reset: like switching the board off and on (same arguments).
                    save();
                    if (audio) SDL_CloseAudioDevice(audio);
                    SDL_Quit();
                    execv(g_argv[0], g_argv);
                    std::perror("reset");
                    std::exit(1);
                }
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_P) {
                    paused = !paused;
                    if (audio) SDL_PauseAudioDevice(audio, paused ? 1 : 0);
                    SDL_SetWindowTitle(window, paused ? "Killer Instinct (recompiled): paused (P resumes)" : kTitle);
                }
#ifdef KI_HAVE_HD
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_H && hd_texture)
                    hd_on = !hd_on;
#endif
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_F5)
                    state.request(ctx);  // taken at the next dispatcher entry
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_F9 && state.valid())
                    throw ki::LoadStateRequest{};
                if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                    if (SDL_GameController* pad = SDL_GameControllerOpen(ev.cdevice.which)) pads.push_back(pad);
                }
                if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    for (SDL_GameController*& pad : pads)
                        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == ev.cdevice.which) {
                            SDL_GameControllerClose(pad);
                            pad = nullptr;
                        }
                    pads.erase(std::remove(pads.begin(), pads.end(), nullptr), pads.end());
                }
            }
        }
        if (frame % 600 == 0) save();  // every ~10 s, if the game wrote anything
        if (state.just_saved()) {
            SDL_SetWindowTitle(window, "Killer Instinct (recompiled): state saved (F9 loads it)");
            title_reset_frame = frame + 120;
        }
        if (title_reset_frame && frame >= title_reset_frame) {
            SDL_SetWindowTitle(window, kTitle);
            title_reset_frame = 0;
        }
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        uint32_t p1 = 0xffffffff, p2 = 0xffffffff;
        for (const KeyBit& k : kKeys)
            if (keys[k.key]) (k.player == 1 ? p1 : p2) &= ~k.bit;
        int player = 0;
        for (SDL_GameController* pad : pads)
            if (pad && SDL_GameControllerGetAttached(pad) && player < 2) (player++ == 0 ? p1 : p2) &= ~pad_bits(pad);
        board.p1 = p1;
        board.p2 = p2;

        if (dcs) {
            std::vector<int16_t>& pcm = dcs->samples();
            // Drop audio in turbo mode, or if the queue backs up past 200 ms.
            if (audio && !turbo && SDL_GetQueuedAudioSize(audio) < 31250 * 2 / 5)
                SDL_QueueAudio(audio, pcm.data(), static_cast<Uint32>(pcm.size() * sizeof(int16_t)));
            pcm.clear();
        }
        if (!turbo || frame % 8 == 0) present(ctx);
        if (!turbo) {
            next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kFrameSeconds));
            // Follow the audio clock: when more than ~60 ms are queued, slow down slightly.
            if (audio && SDL_GetQueuedAudioSize(audio) > 31250 * 2 * 6 / 100)
                next_frame += std::chrono::microseconds(500);
            auto now = std::chrono::steady_clock::now();
            if (next_frame > now) std::this_thread::sleep_until(next_frame);
            else if (now - next_frame > std::chrono::milliseconds(100)) next_frame = now;  // fell behind: resync
        }
    };
    ki::schedule(c);
    bool ok = false;
    for (;;) {
        try {
            ok = ki::run_entry(c, entry);
            break;
        } catch (const ki::LoadStateRequest&) {
            entry = state.restore(c);
            if (audio) SDL_ClearQueuedAudio(audio);
            next_frame = std::chrono::steady_clock::now();
            SDL_SetWindowTitle(window, "Killer Instinct (recompiled): state loaded");
        }
    }
    save();
    if (!ok)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle,
                                 "The recompiled program stopped on an unexpected guest fault "
                                 "(details on the terminal).",
                                 window);
    SDL_Quit();
    return ok ? 0 : 1;
}
