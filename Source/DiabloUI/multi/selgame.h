#pragma once

#include "levels/gendung.h"

namespace devilution {

extern _difficulty nDifficulty;

void selgame_GameSelection_Init();
void selgame_GameSelection_Focus(size_t value);
void selgame_GameSelection_Select(size_t value);
void selgame_GameSelection_Esc();
void selgame_Diff_Focus(size_t value);
void selgame_Diff_Select(size_t value);
void selgame_Diff_Esc();
void selgame_GameSpeedSelection();
void selgame_Speed_Focus(size_t value);
void selgame_Speed_Select(size_t value);
void selgame_Speed_Esc();
#ifdef DEVILUTIONX_LOBBY
void selgame_LobbyName_Init();
void selgame_LobbyName_Select(size_t value);
void selgame_LobbyName_Esc();
#ifdef __EMSCRIPTEN__
void selgame_Transport_Toggle();
#endif
#endif
void selgame_Password_Init(size_t value);
void selgame_Password_Select(size_t value);
void selgame_Password_Esc();

} // namespace devilution
