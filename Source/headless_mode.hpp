#pragma once

#include "utils/attributes.h"

namespace devilution {

/**
 * @brief Don't load UI or show Messageboxes or other user-interaction. Needed for unit tests.
 */
extern DVL_API_FOR_TEST bool HeadlessMode;

/**
 * @brief A headless multiplayer companion driven by the s1 debug bridge (`--headless`).
 *
 * Tests also set HeadlessMode but must keep the normal game flow, so companion-only behaviour checks this instead.
 */
inline bool CompanionHeadless()
{
#ifdef DAMNATION_S1_BRIDGE
	return HeadlessMode;
#else
	return false;
#endif
}

} // namespace devilution
