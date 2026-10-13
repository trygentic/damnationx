#pragma once

#include <expected>
#include <string_view>

#include <function_ref.hpp>
#include <sol/forward.hpp>

namespace devilution {

void LuaInitialize();
// Requests a reload; the implementation applies it only at a safe frame boundary.
void LuaReloadActiveMods();
// Called by the application pump after simulation work reaches a frame boundary.
void LuaApplyPendingModReload();
void LuaShutdown();

sol::state &GetLuaState();
sol::environment CreateLuaSandbox();
sol::object SafeCallResult(sol::protected_function_result result, bool optional);
sol::table *GetLuaEvents();

/** Adds a handler to be called when mods status changes after the initial startup. */
void AddModsChangedHandler(tl::function_ref<void()> callback);

} // namespace devilution
