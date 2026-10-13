#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Routes "open URL" Apple events (custom URL schemes from Info.plist) to the callback.
// SDL installs its own handler during SDL_Init, so call this before and again after it:
// AppKit delivers the URL that launched the app while SDL is still finishing its launch.
void MacInstallUrlHandler(void (*callback)(const char *url));

#ifdef __cplusplus
}
#endif
