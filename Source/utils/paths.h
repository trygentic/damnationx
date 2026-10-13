#pragma once

#include <string>

namespace devilution {

namespace paths {

const std::string &BasePath();
const std::string &PrefPath();
const std::string &ConfigPath();
const std::string &AssetsPath();
/** SDL's base path (the executable's folder, or `Contents/Resources/` of a macOS bundle), even after `--data-dir`. */
const std::string &ExecutableDirectory();

void SetBasePath(const std::string &path);
void SetPrefPath(const std::string &path);
void SetConfigPath(const std::string &path);
void SetAssetsPath(const std::string &path);

} // namespace paths

} // namespace devilution
