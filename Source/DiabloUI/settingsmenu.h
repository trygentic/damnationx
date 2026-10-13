#pragma once

#include <functional>

#include "DiabloUI/diabloui.h"

namespace devilution {

void UiSettingsMenu();

/**
 * @brief Runs `action` once the settings menu has torn down its UI, so that the action can show
 * its own dialogs. The menu is rebuilt afterwards. Runs `action` right away outside the settings menu.
 */
void DeferSettingsMenuAction(std::function<void()> action);

} // namespace devilution
