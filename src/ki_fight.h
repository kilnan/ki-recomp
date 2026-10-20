// Choosing the opponent and the stage of the coming fights (ki_play
// --opponent / --stage, F6 / F7). The game's own choice is replaced at the
// fight setup (hooks::fight_setup), before the stage and the fighters load.
#pragma once

#include <string>
#include <vector>

namespace ki {

struct FightChoice {
    // Used in turn, one per fight with a human player; empty: the game's choice.
    std::vector<int> opponents;  // character numbers, see kCharacterNames
    std::vector<int> stages;     // 0..15, index into the stage table at 0x8800e22c
    unsigned fights = 0;
};

// Character numbers as the game keeps them (low byte of 0x8808f5f0 / 0x8808f5f4).
extern const char* const kCharacterNames[11];
constexpr int kStageCount = 16;

// "orchid,combo" or "8,1"; false on an unknown name.
bool parse_characters(const std::string& list, std::vector<int>& out);
bool parse_numbers(const std::string& list, int limit, std::vector<int>& out);

// Installs the hook (one choice at a time; the object must outlive the run).
void install_fight_choice(FightChoice* choice);

}  // namespace ki
