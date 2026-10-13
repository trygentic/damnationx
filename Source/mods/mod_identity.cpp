#include "mods/mod_identity.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <regex>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <utility>

#include <picosha2.h>

#include "utils/file_util.h"
#include "utils/ini.hpp"
#include "utils/log.hpp"

namespace devilution {

std::vector<ModIdentifier> ActiveModIdentifiers;

namespace {

/**
 * @brief Hardcoded list of approved third-party mod hashes.
 */
constexpr std::array<std::array<uint8_t, 32>, 0> ApprovedModHashes = {};

} // namespace

std::string NormalizeModLogicalId(std::string_view id)
{
	std::string result;
	bool separator = false;
	for (const unsigned char character : id) {
		if (std::isspace(character)) continue;
		const char c = static_cast<char>(std::tolower(character));
		if (c == '/' || c == '\\' || c == '.') { separator = !result.empty(); continue; }
		if (separator) { if (!result.empty()) result.push_back('.'); separator = false; }
		result.push_back(c);
	}
	while (!result.empty() && (result.front() == '.' || result.back() == '.')) {
		if (result.front() == '.') result.erase(result.begin());
		if (!result.empty() && result.back() == '.') result.pop_back();
	}
	return result;
}

std::expected<VersionedModManifest, ModValidationError> ParseVersionedModManifest(std::string_view manifestIni)
{
	const std::expected<Ini, std::string> parsed = Ini::parse(manifestIni);
	if (!parsed.has_value()) return std::unexpected(ModValidationError::InvalidSchema);
	if (parsed->getString("schema", "version") != "1") return std::unexpected(ModValidationError::InvalidSchema);
	VersionedModManifest result;
	result.schemaVersion = 1;
	result.canonicalId = NormalizeModLogicalId(parsed->getString("package", "id"));
	result.version = std::string(parsed->getString("package", "version"));
	if (result.canonicalId.empty() || result.version.empty()) return std::unexpected(ModValidationError::InvalidId);
	if (!std::regex_match(result.version, std::regex(R"((0|[1-9][0-9]*)\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?)"))) return std::unexpected(ModValidationError::InvalidVersion);
	// Lists are comma-separated; repeated keys are accepted as well.
	const auto forEachListItem = [&](std::string_view section, std::string_view key, auto &&fn) {
		for (const Ini::Value &value : parsed->get(section, key)) {
			std::string_view remaining = value.value;
			while (!remaining.empty()) {
				const size_t comma = remaining.find(',');
				std::string_view item = remaining.substr(0, comma);
				while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.remove_prefix(1);
				while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.remove_suffix(1);
				if (!item.empty()) fn(item);
				if (comma == std::string_view::npos) break;
				remaining.remove_prefix(comma + 1);
			}
		}
	};
	forEachListItem("package", "aliases", [&](std::string_view item) {
		const std::string alias = NormalizeModLogicalId(item);
		if (!alias.empty() && std::find(result.aliases.begin(), result.aliases.end(), alias) == result.aliases.end())
			result.aliases.push_back(alias);
	});
	std::sort(result.aliases.begin(), result.aliases.end());
	forEachListItem("capabilities", "provides", [&](std::string_view item) {
		const std::string capability(item);
		if (std::find(result.capabilities.begin(), result.capabilities.end(), capability) == result.capabilities.end())
			result.capabilities.push_back(capability);
	});
	return result;
}

std::string VersionedModIdentityInput(const VersionedModManifest &manifest, std::span<const std::string> dependencyIdentities)
{
	std::string input = std::to_string(manifest.schemaVersion) + "\n" + manifest.canonicalId + "\n" + manifest.version + "\n";
	for (const std::string &alias : manifest.aliases) input += alias + "\n";
	std::vector<std::string> capabilities = manifest.capabilities;
	std::sort(capabilities.begin(), capabilities.end());
	for (const std::string &capability : capabilities) input += capability + "\n";
	for (const std::string &dependency : dependencyIdentities) input += dependency + "\n";
	return input;
}

void ClearModIdentifiers()
{
	ActiveModIdentifiers.clear();
}

bool ComputeFileSha256(const char *path, std::array<uint8_t, 32> &hashOut)
{
	FILE *file = OpenFile(path, "rb");
	if (file == nullptr)
		return false;

	picosha2::hash256_one_by_one hasher;
	std::vector<uint8_t> buffer(32768);
	size_t bytesRead;
	while ((bytesRead = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
		hasher.process(buffer.begin(), buffer.begin() + bytesRead);
	}
	const bool readError = std::ferror(file) != 0;
	std::fclose(file);
	if (readError)
		return false;

	hasher.finish();
	hasher.get_hash_bytes(hashOut.begin(), hashOut.end());
	return true;
}

ModIdentifier &RegisterPackedModIdentifier(std::string_view name, const char *mpqPath)
{
	ModIdentifier identifier;
	identifier.name = std::string(name);
	identifier.hash = {};
	if (!ComputeFileSha256(mpqPath, identifier.hash)) {
		LogError("Failed to hash mod archive: {}", mpqPath);
	}
	identifier.whitelisted = IsHashWhitelisted(identifier.hash);
	ActiveModIdentifiers.push_back(std::move(identifier));
	return ActiveModIdentifiers.back();
}

bool HexToModHash(std::string_view hex, std::array<uint8_t, 32> &out)
{
	if (hex.size() != 2 * out.size())
		return false;
	const auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < out.size(); ++i) {
		const int hi = nibble(hex[i * 2]);
		const int lo = nibble(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out[i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return true;
}

ModManifest ParseModManifest(std::string_view manifestIni)
{
	ModManifest manifest;
	const bool declaresNewSchema = manifestIni.find("[schema]") != std::string_view::npos;
	if (declaresNewSchema) {
		const auto versioned = ParseVersionedModManifest(manifestIni);
		if (!versioned.has_value()) {
			manifest.activation = ModActivation::Rejected;
			manifest.validationError = versioned.error();
			return manifest;
		}
		manifest.versioned = *versioned;
		manifest.activation = ModActivation::NewSchema;
	}
	std::expected<Ini, std::string> ini = Ini::parse(manifestIni);
	if (!ini.has_value()) {
		LogError("Failed to parse mod manifest: {}", ini.error());
		return manifest;
	}

	constexpr std::string_view Section = "mod";
	manifest.name = std::string(ini->getString(Section, "name"));
	manifest.description = std::string(ini->getString(Section, "description"));
	manifest.version = std::string(ini->getString(Section, "version"));
	manifest.author = std::string(ini->getString(Section, "author"));
	manifest.homepage = std::string(ini->getString(Section, "homepage"));
	manifest.license = std::string(ini->getString(Section, "license"));
	manifest.saveExtension = std::string(ini->getString(Section, "saveExtension"));
	manifest.programId = std::string(ini->getString(Section, "programId"));

	for (const Ini::Value &value : ini->get(Section, "requires")) {
		if (!value.value.empty())
			manifest.requiredMods.push_back(value.value);
	}
	for (const Ini::Value &value : ini->get(Section, "compatible")) {
		std::array<uint8_t, 32> hash;
		if (HexToModHash(value.value, hash)) {
			manifest.compatibleWith.push_back(hash);
		} else {
			LogError("Ignoring invalid compatible identifier in mod manifest: {}", value.value);
		}
	}
	return manifest;
}

namespace {

// Length-prefixed so that field boundaries cannot be forged by embedded separators.
void AppendField(std::string &out, std::string_view value)
{
	out += std::to_string(value.size());
	out.push_back(':');
	out += value;
	out.push_back('\n');
}

void AppendFields(std::string &out, std::span<const std::string> values)
{
	AppendField(out, std::to_string(values.size()));
	for (const std::string &value : values) AppendField(out, value);
}

void AppendPairs(std::string &out, std::span<const std::pair<std::string, std::string>> values)
{
	AppendField(out, std::to_string(values.size()));
	for (const auto &[key, value] : values) {
		AppendField(out, key);
		AppendField(out, value);
	}
}

std::array<uint8_t, 32> Sha256(std::string_view input)
{
	std::array<uint8_t, 32> hash {};
	picosha2::hash256(input.begin(), input.end(), hash.begin(), hash.end());
	return hash;
}

bool AnyEmpty(std::span<const std::string> values)
{
	return std::any_of(values.begin(), values.end(), [](const std::string &value) { return value.empty(); });
}

} // namespace

std::expected<CanonicalIdentity, ModValidationError> BuildCanonicalModIdentity(
    const CanonicalModIdentityInput &gameplay,
    const VerifiedCosmeticIdentity &cosmetic)
{
	if (gameplay.profile.empty() || AnyEmpty(gameplay.wasmIdentities) || AnyEmpty(gameplay.luaIdentities)
	    || AnyEmpty(gameplay.behaviorIdentities) || AnyEmpty(gameplay.features)
	    || AnyEmpty(cosmetic.assetIdentities) || AnyEmpty(cosmetic.features))
		return std::unexpected(ModValidationError::InvalidCanonicalData);

	std::string gameplayInput;
	AppendField(gameplayInput, "dxmod-gameplay-identity-1");
	AppendFields(gameplayInput, gameplay.wasmIdentities);
	AppendFields(gameplayInput, gameplay.luaIdentities);
	AppendFields(gameplayInput, gameplay.behaviorIdentities);
	AppendField(gameplayInput, gameplay.profile);
	AppendField(gameplayInput, std::to_string(gameplay.abiVersion));
	AppendFields(gameplayInput, gameplay.features);
	AppendField(gameplayInput, std::to_string(gameplay.gasLimit));
	AppendField(gameplayInput, std::to_string(gameplay.callDepthLimit));
	AppendField(gameplayInput, std::to_string(gameplay.configurationVersion));
	AppendPairs(gameplayInput, gameplay.configuration);
	AppendPairs(gameplayInput, gameplay.modState);

	std::string cosmeticInput;
	AppendField(cosmeticInput, "dxmod-cosmetic-identity-1");
	AppendFields(cosmeticInput, cosmetic.assetIdentities);
	AppendFields(cosmeticInput, cosmetic.features);

	CanonicalIdentity identity;
	identity.gameplay = Sha256(gameplayInput);
	identity.cosmetic = Sha256(cosmeticInput);
	return identity;
}

std::string CanonicalModSaveNamespace(const ModIdentifier &mod)
{
	if (!mod.manifest.versioned.has_value())
		return {};
	return ModHashToHex(Sha256(VersionedModIdentityInput(*mod.manifest.versioned, {})));
}

bool IsCompatibleWithSavedMod(const ModIdentifier &mod, std::span<const uint8_t, 32> savedHash)
{
	if (!mod.hashValid || !mod.manifest.versioned.has_value())
		return false;
	return SatisfiesRequiredIdentifier(mod, savedHash);
}

bool SatisfiesRequiredIdentifier(const ModIdentifier &mod, std::span<const uint8_t, 32> required)
{
	// A hash that failed to compute is never an identity match, even when both are zero.
	if (mod.hashValid && std::equal(mod.hash.begin(), mod.hash.end(), required.begin()))
		return true;
	for (const std::array<uint8_t, 32> &compatible : mod.manifest.compatibleWith) {
		if (std::equal(compatible.begin(), compatible.end(), required.begin()))
			return true;
	}
	return false;
}

void RegisterBuiltinModIdentifier(std::string_view name)
{
	ModIdentifier identifier;
	identifier.name = std::string(name);
	identifier.hash = {};
	identifier.hashValid = true; // the zero hash is the built-in's real identity, not a failure
	identifier.whitelisted = true; // provenance: resolves purely from core archives
	ActiveModIdentifiers.push_back(std::move(identifier));
}

std::string ModHashToHex(std::span<const uint8_t, 32> hash)
{
	return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
}

bool IsHashWhitelisted(std::span<const uint8_t, 32> hash)
{
	for (const std::array<uint8_t, 32> &approved : ApprovedModHashes) {
		if (std::equal(approved.begin(), approved.end(), hash.begin()))
			return true;
	}
	return false;
}

std::string_view GetActiveModSaveExtension()
{
	std::string_view result;
	for (const ModIdentifier &mod : ActiveModIdentifiers) {
		if (!mod.whitelisted) {
			result = "msv";
			break;
		}
	}
	for (const ModIdentifier &mod : ActiveModIdentifiers) {
		std::string_view ext = mod.manifest.saveExtension;
		if (ext.empty())
			continue;
		if (ext.front() == '.')
			ext.remove_prefix(1);
		if (ext == "sv")
			continue;
		if (!ext.empty())
			result = ext; // last active mod that declares one wins
	}
	return result;
}

std::vector<std::string> OrderModsByDependencies(const std::vector<ModDependency> &mods)
{
	std::vector<std::string> ordered;
	ordered.reserve(mods.size());

	// A mod is emitted once all of its (present) required mods have been emitted. We repeat
	// passes over the not-yet-emitted mods until a full pass emits nothing new: the remainder
	// is then unsatisfiable (a cycle, or a required mod missing from the active set).
	std::vector<bool> emitted(mods.size(), false);
	std::unordered_set<std::string_view> emittedNames;
	std::unordered_set<std::string_view> presentNames;
	for (const ModDependency &mod : mods)
		presentNames.insert(mod.name);

	bool progress = true;
	while (progress) {
		progress = false;
		for (size_t i = 0; i < mods.size(); ++i) {
			if (emitted[i])
				continue;
			bool ready = true;
			for (const std::string &required : mods[i].requiredMods) {
				if (emittedNames.find(required) != emittedNames.end())
					continue;
				// A required mod that is not part of the active set can never be satisfied;
				// don't block on it here (it is reported once the loop stalls below).
				if (presentNames.find(required) != presentNames.end()) {
					ready = false;
					break;
				}
			}
			if (!ready)
				continue;
			ordered.push_back(mods[i].name);
			emittedNames.insert(mods[i].name);
			emitted[i] = true;
			progress = true;
		}
	}

	// Anything still unemitted is part of a cycle. Report it and append in original order so
	// the mod is still loaded (best-effort) rather than silently dropped.
	for (size_t i = 0; i < mods.size(); ++i) {
		if (emitted[i])
			continue;
		LogError("Mod '{}' has an unsatisfiable required-mod dependency (cycle); loading anyway.", mods[i].name);
		ordered.push_back(mods[i].name);
	}

	// Report any required mods that are missing from the active set.
	for (const ModDependency &mod : mods) {
		for (const std::string &required : mod.requiredMods) {
			if (presentNames.find(required) == presentNames.end())
				LogError("Mod '{}' requires '{}', which is not enabled.", mod.name, required);
		}
	}

	return ordered;
}

} // namespace devilution
