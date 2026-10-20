#include "ki_fight.h"

#include <cstdio>
#include <sstream>
#include <strings.h>

#include "ki/hooks.h"
#include "ki/runtime.h"

namespace ki {

const char* const kCharacterNames[11] = {"jago",   "combo",     "spinal", "thunder", "glacius", "fulgore",
                                         "cinder", "sabrewulf", "orchid", "riptor",  "eyedol"};

namespace {

FightChoice* g_choice = nullptr;

uint32_t word(const Context& c, uint32_t off) {
    return c.dram[off] | c.dram[off + 1] << 8 | c.dram[off + 2] << 16 | static_cast<uint32_t>(c.dram[off + 3]) << 24;
}

void on_fight_setup(Context& c) {
    // 0x8808f5f0 / 0x8808f5f4: player 1 / player 2 fighter. Low byte: character;
    // bits 8-11: costume; 0x10000000: controlled by the game.
    constexpr uint32_t kSides[2] = {0x8f5f0, 0x8f5f4};
    constexpr uint32_t kCpu = 0x10000000u;
    bool cpu[2] = {(word(c, kSides[0]) & kCpu) != 0, (word(c, kSides[1]) & kCpu) != 0};
    if (cpu[0] && cpu[1]) return;  // attract mode
    FightChoice& f = *g_choice;
    unsigned n = f.fights++;
    if (!f.opponents.empty() && cpu[0] != cpu[1]) {
        int opponent = f.opponents[n % f.opponents.size()];
        c.dram[kSides[cpu[0] ? 0 : 1]] = static_cast<uint8_t>(opponent);
        std::fprintf(stderr, "fight %u: opponent %s\n", n + 1, kCharacterNames[opponent]);
    }
    if (!f.stages.empty()) {
        c.dram[0x86238] = static_cast<uint8_t>(f.stages[n % f.stages.size()]);
        std::fprintf(stderr, "fight %u: stage %d\n", n + 1, c.dram[0x86238]);
    }
}

}  // namespace

bool parse_numbers(const std::string& list, int limit, std::vector<int>& out) {
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        char* end = nullptr;
        long v = std::strtol(item.c_str(), &end, 10);
        if (item.empty() || *end || v < 0 || v >= limit) return false;
        out.push_back(static_cast<int>(v));
    }
    return !out.empty();
}

bool parse_characters(const std::string& list, std::vector<int>& out) {
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        int found = -1;
        for (int i = 0; i < 11; ++i)
            if (!strcasecmp(item.c_str(), kCharacterNames[i])) found = i;
        if (found < 0) {
            std::vector<int> number;
            if (!parse_numbers(item, 11, number)) return false;
            found = number[0];
        }
        out.push_back(found);
    }
    return !out.empty();
}

void install_fight_choice(FightChoice* choice) {
    g_choice = choice;
    hooks::fight_setup = choice ? on_fight_setup : nullptr;
}

}  // namespace ki
