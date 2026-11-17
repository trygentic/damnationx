#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace devilution::render3d {

/** Shared CPU / WGSL palette budget, including rigid equipment palette entries. */
constexpr size_t MaxSkinnedJoints = 256; // the Sorcerer's spring chains plus the bow rig pass 128
constexpr size_t SkinnedJointMaskWords = MaxSkinnedJoints / 32;

/** One visibility bit per mesh node (part), 128 parts; the shader reads it as four u32 words. Integers convert to the low bits. */
struct PartMask {
	uint64_t low = 0, high = 0;
	constexpr PartMask() = default;
	constexpr PartMask(uint64_t bits) // NOLINT(google-explicit-constructor): masks read like integers
	    : low(bits)
	{
	}
	constexpr PartMask(uint64_t lowBits, uint64_t highBits)
	    : low(lowBits)
	    , high(highBits)
	{
	}
	constexpr PartMask operator|(PartMask o) const { return { low | o.low, high | o.high }; }
	constexpr PartMask operator&(PartMask o) const { return { low & o.low, high & o.high }; }
	constexpr PartMask operator~() const { return { ~low, ~high }; }
	constexpr PartMask &operator|=(PartMask o) { return *this = *this | o; }
	constexpr PartMask &operator&=(PartMask o) { return *this = *this & o; }
	constexpr PartMask operator<<(int n) const
	{
		if (n <= 0) return *this;
		if (n >= 128) return {};
		if (n >= 64) return { 0, low << (n - 64) };
		return { low << n, (high << n) | (low >> (64 - n)) };
	}
	constexpr bool operator==(const PartMask &o) const { return low == o.low && high == o.high; }
	constexpr bool operator!=(const PartMask &o) const { return !(*this == o); }
	/** Word i (0-3) of the shader's part mask. */
	constexpr uint32_t word(size_t i) const { return static_cast<uint32_t>((i < 2 ? low : high) >> (32 * (i % 2))); }
};
constexpr size_t MaxSkinnedParts = 128;

enum class SkinnedModelKind : uint8_t { Warrior,
	Rogue,
	Sorcerer };
enum class SkinnedEquipmentKind : uint8_t { Unarmed,
	Bow,
	UnarmedShield,
	Sword,
	SwordShield,
	Mace,
	MaceShield,
	Axe,
	Staff,
	TwoHanded,
	Unsupported };

/** Per-item meshes that replace the Warrior's sword; the sword clips still drive them. */
enum class WarriorSwordVariant : uint8_t { Sword,
	Dagger,
	Falchion,
	Scimitar,
	ShortSword,
	Sabre,
	Blade,
	Claymore,
	LongSword,
	BroadSword,
	BastardSword };

/** Authored names: <name>_RIGHT in the hand with <name>_Sheath_Empty at the hip; <name>_Town_Stowed (sheathed) in town. */
constexpr std::array<std::string_view, 10> WarriorSwordVariantNames { "Dagger", "Falchion", "Scimitar", "Short_Sword", "Sabre", "Blade", "Claymore",
	"Long_Sword", "Broad_Sword", "Bastard_Sword" };

/** Per-item shields replacing Shield_LEFT on the same ShieldCarry bone, in town and dungeon. */
enum class WarriorShieldVariant : uint8_t { Shield,
	Buckler,
	Kite,
	Tower,
	Small,
	Large,
	Gothic };

/** Authored names: <name>_LEFT. */
constexpr std::array<std::string_view, 6> WarriorShieldVariantNames { "Buckler", "Kite_Shield", "Tower_Shield", "Small_Shield", "Large_Shield", "Gothic_Shield" };

/** Per-item meshes of the mace, staff and axe families, held (<name>_RIGHT; the Rogue's axes <name>_TWOHANDED) and
 * stowed (<name>_Town_Stowed). Look indices are 1-based; 0 is the family's default mesh. */
constexpr std::array<std::string_view, 4> MaceVariantNames { "Morning_Star", "Club", "Spiked_Club", "War_Hammer" };
constexpr std::array<std::string_view, 3> StaffVariantNames { "Long_Staff", "Composite_Staff", "War_Staff" }; // Quarter Staff shares the Short Staff's icon
constexpr std::array<std::string_view, 5> AxeVariantNames { "Battle_Axe", "Small_Axe", "Large_Axe", "Broad_Axe", "Great_Axe" };

/** Two-handed swords and the Maul share the legacy one-handed graphic, so only their own meshes and the two_handed clips
 * show them: <name>_RIGHT in the hands, <name>_Town_Stowed on the back, <name>_Sheath_Empty (swords) while drawn. */
constexpr std::array<std::string_view, 3> TwoHandedVariantNames { "Two_Handed_Sword", "Great_Sword", "Maul" };

/** Per-item bows on the shared bow joints: <name>_LEFT in the bow hand, <name>_Town_Stowed, and bowflex_<name>(_repeat)
 * overlays that re-bend the shared limb joints to the bow's own shape during the draw. The Long Bow shares the Hunter's
 * icon and so its model. */
constexpr std::array<std::string_view, 7> BowVariantNames { "Short_Bow", "Hunters_Bow", "Composite_Bow", "Short_Battle_Bow",
	"Long_Battle_Bow", "Short_War_Bow", "Long_War_Bow" };

struct HeldVariant {
	PartMask held = 0, stowed = 0;
};

/** Which per-item meshes the equipped items select; packs without a mesh keep the family's default. */
struct SkinnedItemLook {
	WarriorSwordVariant sword = WarriorSwordVariant::Sword;
	WarriorShieldVariant shield = WarriorShieldVariant::Shield;
	uint8_t mace = 0, staff = 0, axe = 0; // 1-based into MaceVariantNames / StaffVariantNames / AxeVariantNames
	uint8_t twoHanded = 0;                // 1-based into TwoHandedVariantNames; 0 = no own mesh (sprite fallback)
	uint8_t bow = 0;                      // 1-based into BowVariantNames; 0 = the default bow
};

/** Replaces a family's held and stowed meshes with the selected variant's, when the pack has both. */
template <size_t N>
constexpr void SwapHeldVariant(PartMask &held, PartMask &stowed, const std::array<HeldVariant, N> &variants, uint8_t index)
{
	if (index == 0 || index > N) return;
	const HeldVariant &own = variants[index - 1];
	if (own.held == 0 || own.stowed == 0) return;
	held = own.held;
	stowed = own.stowed;
}

template <size_t N>
constexpr PartMask HeldVariantMask(const std::array<HeldVariant, N> &variants, bool stowedOnly = false)
{
	PartMask mask = 0;
	for (const HeldVariant &variant : variants)
		mask |= (stowedOnly ? PartMask {} : variant.held) | variant.stowed;
	return mask;
}

enum class SkinnedAnimationKind : uint8_t { Stand,
	Walk,
	Attack,
	Hit,
	Cast,
	Death,
	Block };

struct SkinnedEquipmentClipSet {
	int stand = -1, walk = -1, townStand = -1, townWalk = -1;
	int attack = -1, attackRepeat = -1, block = -1;
	int cast = -1; // the family's own cast, when the generic one would drive its weapon into the body
	int hit = -1; // optional grip-preserving reaction for this equipment family
	int townCast = -1, townHit = -1;
};

/** A bow's flex overlays for the bow attack and its repeat. */
struct BowFlexClips {
	int attack = -1, repeat = -1;
};

struct SkinnedClipSet {
	int stand = -1, walk = -1, townStand = -1, townWalk = -1;
	int standUnarmed = -1, walkUnarmed = -1, townStandUnarmed = -1, townWalkUnarmed = -1;
	int attack = -1, attackRepeat = -1, block = -1, hit = -1, cast = -1, death = -1;
	int attackUnarmed = -1, attackRepeatUnarmed = -1;
	SkinnedEquipmentClipSet unarmedShield, sword, swordShield, mace, maceShield, axe, bow, staff, twoHanded;
	std::array<BowFlexClips, BowVariantNames.size()> bowFlex {};
	int townCast = -1, townHit = -1;
};

constexpr std::string_view SkinnedAssetPath(SkinnedModelKind kind, bool smallTexture)
{
	if (kind == SkinnedModelKind::Rogue)
		return smallTexture ? "hd/3d/rogue/rogue_1024.glb" : "hd/3d/rogue/rogue.glb";
	if (kind == SkinnedModelKind::Sorcerer)
		return smallTexture ? "hd/3d/sorcerer/sorcerer_1024.glb" : "hd/3d/sorcerer/sorcerer.glb";
	return smallTexture ? "hd/3d/warrior/warrior_1024.glb" : "hd/3d/warrior/warrior.glb";
}

constexpr bool IsMaceEquipment(SkinnedEquipmentKind equipment)
{
	return equipment == SkinnedEquipmentKind::Mace || equipment == SkinnedEquipmentKind::MaceShield;
}

/** Strict Rogue state/equipment coverage; retain the Warrior's legacy-asset fallback. */
constexpr int ChooseSkinnedClip(const SkinnedClipSet &clips, SkinnedModelKind kind,
    SkinnedAnimationKind action, bool town, SkinnedEquipmentKind equipment, bool repeat)
{
	// WIP Sorcerer: each weapon family plays its own clips once the pack has them (its stand is the marker); until then
	// his base clips play empty-handed. A state without a clip keeps the sprites.
	if (kind == SkinnedModelKind::Sorcerer) {
		if (action == SkinnedAnimationKind::Death) return clips.death;
		SkinnedEquipmentClipSet family;
		switch (equipment) {
		case SkinnedEquipmentKind::UnarmedShield: family = clips.unarmedShield; break;
		case SkinnedEquipmentKind::Sword: family = clips.sword; break;
		case SkinnedEquipmentKind::SwordShield: family = clips.swordShield; break;
		case SkinnedEquipmentKind::Mace: family = clips.mace; break;
		case SkinnedEquipmentKind::MaceShield: family = clips.maceShield; break;
		case SkinnedEquipmentKind::Axe: family = clips.axe; break;
		case SkinnedEquipmentKind::Bow: family = clips.bow; break;
		case SkinnedEquipmentKind::Staff: family = clips.staff; break;
		case SkinnedEquipmentKind::TwoHanded: family = clips.twoHanded; break;
		default: break;
		}
		if (family.stand < 0)
			family = { clips.stand, clips.walk, clips.townStand >= 0 ? clips.townStand : clips.stand, clips.townWalk >= 0 ? clips.townWalk : clips.walk,
				clips.attack, clips.attackRepeat >= 0 ? clips.attackRepeat : clips.attack, clips.block, -1, -1, -1, -1 };
		switch (action) {
		case SkinnedAnimationKind::Stand: return town ? family.townStand : family.stand;
		case SkinnedAnimationKind::Walk: return town ? family.townWalk : family.walk;
		case SkinnedAnimationKind::Attack: return town ? -1 : repeat && family.attackRepeat >= 0 ? family.attackRepeat : family.attack;
		case SkinnedAnimationKind::Block: return town ? -1 : family.block;
		case SkinnedAnimationKind::Cast:
			if (town && family.townCast >= 0) return family.townCast;
			if (family.cast >= 0) return family.cast;
			return town && clips.townCast >= 0 ? clips.townCast : clips.cast;
		case SkinnedAnimationKind::Hit:
			if (town && family.townHit >= 0) return family.townHit;
			if (family.hit >= 0) return family.hit;
			return town && clips.townHit >= 0 ? clips.townHit : clips.hit;
		default: return -1;
		}
	}
	if (kind == SkinnedModelKind::Rogue) {
		// Death renders only the body, so it does not require an equipped item's mesh.
		if (action == SkinnedAnimationKind::Death) return clips.death;
		SkinnedEquipmentClipSet family;
		switch (equipment) {
		case SkinnedEquipmentKind::Bow:
			family = { clips.stand, clips.walk, clips.townStand, clips.townWalk, clips.attack, clips.attackRepeat, -1, clips.bow.cast, clips.bow.hit, clips.bow.townCast, clips.bow.townHit };
			break;
		case SkinnedEquipmentKind::Unarmed:
			family = { clips.standUnarmed, clips.walkUnarmed, clips.townStandUnarmed, clips.townWalkUnarmed, clips.attackUnarmed, clips.attackRepeatUnarmed, -1, clips.cast, clips.hit, clips.townCast, clips.townHit };
			break;
		case SkinnedEquipmentKind::UnarmedShield: family = clips.unarmedShield; break;
		case SkinnedEquipmentKind::Sword: family = clips.sword; break;
		case SkinnedEquipmentKind::SwordShield: family = clips.swordShield; break;
		case SkinnedEquipmentKind::Mace: family = clips.mace; break;
		case SkinnedEquipmentKind::MaceShield: family = clips.maceShield; break;
		case SkinnedEquipmentKind::Axe: family = clips.axe; break;
		case SkinnedEquipmentKind::Staff: family = clips.staff; break;
		case SkinnedEquipmentKind::TwoHanded: family = clips.twoHanded; break;
		case SkinnedEquipmentKind::Unsupported: return -1;
		}
		switch (action) {
		case SkinnedAnimationKind::Stand: return town ? family.townStand : family.stand;
		case SkinnedAnimationKind::Walk: return town ? family.townWalk : family.walk;
		case SkinnedAnimationKind::Attack: return town ? -1 : repeat ? family.attackRepeat
			                                                         : family.attack;
		case SkinnedAnimationKind::Block: return town ? -1 : family.block;
		case SkinnedAnimationKind::Cast: return town && family.townCast >= 0 ? family.townCast : family.cast;
		case SkinnedAnimationKind::Hit:
			if (town && family.townHit >= 0) return family.townHit;
			return family.hit >= 0 ? family.hit : clips.hit;
		default: return -1;
		}
	}
	// A free hand and a shield need different town carries when a pack provides them.
	// Older Warrior packs retain their shared town walk and dungeon fallbacks.
	if (kind == SkinnedModelKind::Warrior && town && action == SkinnedAnimationKind::Walk) {
		int townWalk = -1;
		switch (equipment) {
		case SkinnedEquipmentKind::Unarmed: townWalk = clips.townWalkUnarmed; break;
		case SkinnedEquipmentKind::UnarmedShield: townWalk = clips.unarmedShield.townWalk; break;
		case SkinnedEquipmentKind::Sword: townWalk = clips.sword.townWalk; break;
		case SkinnedEquipmentKind::SwordShield: townWalk = clips.swordShield.townWalk; break;
		default: break;
		}
		if (townWalk >= 0) return townWalk;
	}
	if (equipment == SkinnedEquipmentKind::Bow) {
		switch (action) {
		case SkinnedAnimationKind::Stand: return town ? clips.bow.townStand : clips.bow.stand;
		case SkinnedAnimationKind::Walk: return town ? clips.bow.townWalk : clips.bow.walk;
		case SkinnedAnimationKind::Attack: return town ? -1 : repeat ? clips.bow.attackRepeat : clips.bow.attack;
		case SkinnedAnimationKind::Hit: return town ? clips.bow.townHit : clips.bow.hit;
		default: return -1; // Dynamic bow parts require an authored clip for the action.
		}
	}
	if (equipment == SkinnedEquipmentKind::Axe || equipment == SkinnedEquipmentKind::Staff || equipment == SkinnedEquipmentKind::TwoHanded) {
		const auto &family = equipment == SkinnedEquipmentKind::Staff ? clips.staff
		    : equipment == SkinnedEquipmentKind::TwoHanded            ? clips.twoHanded
		                                                              : clips.axe;
		switch (action) {
		case SkinnedAnimationKind::Stand: return town ? family.townStand : family.stand;
		case SkinnedAnimationKind::Walk: return town ? family.townWalk : family.walk;
		case SkinnedAnimationKind::Attack: return town ? -1 : repeat ? family.attackRepeat : family.attack;
		case SkinnedAnimationKind::Block: return -1;
		case SkinnedAnimationKind::Cast:
			if (family.cast >= 0) return family.cast;
			break;
		case SkinnedAnimationKind::Hit: // the family's own grip-preserving reaction; older packs keep the generic hit
			if (town ? family.townHit >= 0 : family.hit >= 0) return town ? family.townHit : family.hit;
			break;
		default: break;
		}
	}
	if (IsMaceEquipment(equipment)) {
		const auto &family = equipment == SkinnedEquipmentKind::MaceShield ? clips.maceShield : clips.mace;
		int selected = -1;
		switch (action) {
		case SkinnedAnimationKind::Stand:
			selected = town ? (family.townStand >= 0 ? family.townStand : clips.mace.townStand) : family.stand;
			break;
		case SkinnedAnimationKind::Walk:
			selected = town ? (family.townWalk >= 0 ? family.townWalk : clips.mace.townWalk) : family.walk;
			break;
		case SkinnedAnimationKind::Attack: selected = repeat && family.attackRepeat >= 0 ? family.attackRepeat : family.attack; break;
		case SkinnedAnimationKind::Block: selected = family.block; break;
		default: break;
		}
		if (selected >= 0) return selected;
	}
	int clip = -1;

	switch (action) {
	case SkinnedAnimationKind::Stand:
		clip = town ? clips.townStand : clips.stand;
		if (kind == SkinnedModelKind::Warrior && clip < 0) clip = clips.stand;
		break;
	case SkinnedAnimationKind::Walk:
		clip = town ? clips.townWalk : clips.walk;
		if (kind == SkinnedModelKind::Warrior && clip < 0) clip = clips.walk;
		break;
	case SkinnedAnimationKind::Attack:
		clip = kind == SkinnedModelKind::Rogue && repeat ? clips.attackRepeat : clips.attack;
		break;
	case SkinnedAnimationKind::Hit: clip = clips.hit; break;
	case SkinnedAnimationKind::Cast: clip = clips.cast; break;
	case SkinnedAnimationKind::Death: clip = clips.death; break;
	case SkinnedAnimationKind::Block: clip = clips.block; break;
	}
	if (kind == SkinnedModelKind::Rogue)
		return town && action == SkinnedAnimationKind::Attack ? -1 : clip;
	return clip >= 0 ? clip : clips.stand;
}

/** Warrior equipment is selected by item family, independently of body/glove mesh order. */
struct WarriorEquipmentParts {
	PartMask swordRight = 0, shield = 0, swordStowed = 0, scabbard = 0, frog = 0, chape = 0;
	PartMask maceRight = 0, maceStowed = 0, axeRight = 0, axeStowed = 0;
	PartMask bowDrawn = 0, bowStowed = 0, quiver = 0;
	PartMask staffRight = 0, staffStowed = 0;
	std::array<HeldVariant, MaceVariantNames.size()> maceVariants {};
	std::array<HeldVariant, StaffVariantNames.size()> staffVariants {};
	std::array<HeldVariant, AxeVariantNames.size()> axeVariants {};
	struct SwordVariant {
		PartMask right = 0, stowed = 0, sheath = 0;
	};
	std::array<SwordVariant, WarriorSwordVariantNames.size()> swordVariants {}; // indexed by WarriorSwordVariant - 1
	std::array<PartMask, WarriorShieldVariantNames.size()> shieldVariants {};   // indexed by WarriorShieldVariant - 1
	std::array<SwordVariant, TwoHandedVariantNames.size()> twoHandedVariants {}; // indexed by SkinnedItemLook::twoHanded - 1
	std::array<HeldVariant, BowVariantNames.size()> bowVariants {};               // indexed by SkinnedItemLook::bow - 1
	std::array<bool, BowVariantNames.size()> bowReady {};                         // both meshes and both flex overlays present
	PartMask bowArrow = 0;
	bool completeBow = false;
	constexpr PartMask all() const
	{
		PartMask mask = swordRight | shield | swordStowed | scabbard | frog | chape | maceRight | maceStowed | axeRight | axeStowed | bowDrawn | bowStowed | quiver | staffRight | staffStowed
		    | HeldVariantMask(maceVariants) | HeldVariantMask(staffVariants) | HeldVariantMask(axeVariants) | HeldVariantMask(bowVariants) | bowArrow;
		for (const SwordVariant &variant : swordVariants)
			mask |= variant.right | variant.stowed | variant.sheath;
		for (const SwordVariant &variant : twoHandedVariants)
			mask |= variant.right | variant.stowed | variant.sheath;
		for (const PartMask variant : shieldVariants)
			mask |= variant;
		return mask;
	}
};

/** The equipped two-hander's own meshes, or nothing when the item or the pack has none. */
template <typename Variant, size_t N>
constexpr const Variant *TwoHandedVariant(const std::array<Variant, N> &variants, uint8_t index)
{
	return index >= 1 && index <= N ? &variants[index - 1] : nullptr;
}

/** The equipped bow's own meshes, when the pack has them and both flex overlays; otherwise the default bow. */
template <typename Parts>
constexpr const HeldVariant *BowVariantOf(const Parts &parts, uint8_t bow)
{
	return bow >= 1 && bow <= BowVariantNames.size() && parts.bowReady[bow - 1] ? &parts.bowVariants[bow - 1] : nullptr;
}

/** The flex overlay re-bending the bow joints while the bow attack (or its repeat) plays, else -1. */
constexpr int BowFlexOverlay(const SkinnedClipSet &clips, SkinnedModelKind kind, int clip, uint8_t bow)
{
	if (bow < 1 || bow > BowVariantNames.size() || clip < 0) return -1;
	const SkinnedEquipmentClipSet bowClips = kind == SkinnedModelKind::Rogue ? SkinnedEquipmentClipSet { -1, -1, -1, -1, clips.attack, clips.attackRepeat, -1 } : clips.bow;
	const BowFlexClips &flex = clips.bowFlex[bow - 1];
	if (clip == bowClips.attack) return flex.attack;
	if (clip == bowClips.attackRepeat) return flex.repeat;
	return -1;
}

/** Older packs lack weapon meshes; keep their correct sprite instead of an empty hand. */
constexpr bool WarriorEquipmentAvailable(SkinnedEquipmentKind equipment, const WarriorEquipmentParts &parts, bool town, const SkinnedItemLook &item = {})
{
	if (equipment == SkinnedEquipmentKind::TwoHanded) {
		const auto *own = TwoHandedVariant(parts.twoHandedVariants, item.twoHanded);
		return own != nullptr && (town ? own->stowed : own->right) != 0;
	}
	if (equipment == SkinnedEquipmentKind::Unsupported) return false; // a two-hander without its own mesh keeps its sprite
	if (equipment == SkinnedEquipmentKind::Bow) return parts.completeBow && parts.quiver != 0 && (town ? parts.bowStowed : parts.bowDrawn) != 0;
	if (equipment == SkinnedEquipmentKind::Axe) return (town ? parts.axeStowed : parts.axeRight) != 0;
	if (equipment == SkinnedEquipmentKind::Staff) return (town ? parts.staffStowed : parts.staffRight) != 0;
	if (!IsMaceEquipment(equipment)) return true;
	return (town ? parts.maceStowed : parts.maceRight) != 0
	    && (equipment != SkinnedEquipmentKind::MaceShield || parts.shield != 0);
}

/** A variant's own meshes carry its sheath and strap, so the sword's scabbard and frog hide; an older pack without
 * the variant keeps the plain sword. */
constexpr PartMask WarriorSwordParts(const WarriorEquipmentParts &parts, bool town, WarriorSwordVariant variant)
{
	if (variant != WarriorSwordVariant::Sword) {
		const auto &own = parts.swordVariants[static_cast<size_t>(variant) - 1];
		const PartMask part = town ? own.stowed : own.right | own.sheath;
		if (part != 0) return part;
	}
	return (town ? parts.swordStowed : parts.swordRight) | parts.scabbard | parts.frog | parts.chape;
}

constexpr PartMask WarriorVisibleParts(SkinnedEquipmentKind equipment, const WarriorEquipmentParts &parts, bool town, bool dead, const SkinnedItemLook &item = {})
{
	const PartMask body = ~parts.all();
	if (dead) return body;
	const WarriorSwordVariant variant = item.sword;
	const WarriorShieldVariant shieldVariant = item.shield;
	PartMask mace = parts.maceRight, maceStowed = parts.maceStowed;
	PartMask staff = parts.staffRight, staffStowed = parts.staffStowed;
	PartMask axe = parts.axeRight, axeStowed = parts.axeStowed;
	SwapHeldVariant(mace, maceStowed, parts.maceVariants, item.mace);
	SwapHeldVariant(staff, staffStowed, parts.staffVariants, item.staff);
	SwapHeldVariant(axe, axeStowed, parts.axeVariants, item.axe);
	PartMask selected = 0;
	if (equipment == SkinnedEquipmentKind::Sword || equipment == SkinnedEquipmentKind::SwordShield)
		selected |= WarriorSwordParts(parts, town, variant);
	if (equipment == SkinnedEquipmentKind::Staff)
		selected |= town ? staffStowed : staff;
	if (IsMaceEquipment(equipment))
		selected |= town ? maceStowed : mace;
	if (equipment == SkinnedEquipmentKind::Axe)
		selected |= town ? axeStowed : axe;
	if (equipment == SkinnedEquipmentKind::Bow) {
		const auto *own = BowVariantOf(parts, item.bow);
		selected |= (own != nullptr ? (town ? own->stowed : own->held | parts.bowArrow) : (town ? parts.bowStowed : parts.bowDrawn)) | parts.quiver;
	}
	if (equipment == SkinnedEquipmentKind::TwoHanded) {
		if (const auto *own = TwoHandedVariant(parts.twoHandedVariants, item.twoHanded))
			selected |= town ? own->stowed : own->right | own->sheath;
	}
	if (equipment == SkinnedEquipmentKind::SwordShield || equipment == SkinnedEquipmentKind::UnarmedShield || equipment == SkinnedEquipmentKind::MaceShield) {
		const PartMask own = shieldVariant != WarriorShieldVariant::Shield ? parts.shieldVariants[static_cast<size_t>(shieldVariant) - 1] : 0;
		selected |= own != 0 ? own : parts.shield; // an older pack without the variant keeps the default shield
	}
	return body | selected;
}

/** Exact authored part names; mesh order and rigid joint indices may change between exports. */
constexpr std::array<std::string_view, 5> RogueBowPartNames {
	"Bow_Flexible_Yew_Limbs", "Bow_Leather_Grip_and_Brass_Collars", "Bow_Animated_String",
	"Arrow_Nocked_only", "Quiver_Back"
};

template <typename FindPart>
PartMask RogueBowPartMask(const FindPart &findPart)
{
	PartMask mask = 0;
	for (std::string_view name : RogueBowPartNames) {
		const int part = findPart(name);
		if (part >= 0 && part < MaxSkinnedParts) mask |= PartMask { 1 } << part;
	}
	return mask;
}

/** Name-derived equipment masks; body and hair remain visible for every supported set. */
struct RogueEquipmentParts {
	PartMask bow = 0, sword = 0, swordStowed = 0, mace = 0, shield = 0, scabbard = 0, axe = 0;
	bool completeBow = false;
	PartMask maceStowed = 0, axeStowed = 0, bowStowed = 0, quiver = 0;
	PartMask staff = 0, staffStowed = 0;
	std::array<HeldVariant, MaceVariantNames.size()> maceVariants {};
	std::array<HeldVariant, StaffVariantNames.size()> staffVariants {};
	std::array<HeldVariant, AxeVariantNames.size()> axeVariants {};
	struct SwordVariant {
		PartMask right = 0, stowed = 0, sheath = 0;
	};
	std::array<SwordVariant, WarriorSwordVariantNames.size()> swordVariants {};
	std::array<PartMask, WarriorShieldVariantNames.size()> shieldVariants {};
	std::array<SwordVariant, TwoHandedVariantNames.size()> twoHandedVariants {};
	std::array<HeldVariant, BowVariantNames.size()> bowVariants {};
	std::array<bool, BowVariantNames.size()> bowReady {};
	PartMask bowArrow = 0;
	constexpr PartMask all() const
	{
		PartMask mask = bow | sword | swordStowed | mace | shield | scabbard | axe | maceStowed | axeStowed | bowStowed | staff | staffStowed
		    | HeldVariantMask(maceVariants) | HeldVariantMask(staffVariants) | HeldVariantMask(axeVariants) | HeldVariantMask(bowVariants) | bowArrow;
		for (const SwordVariant &variant : swordVariants)
			mask |= variant.right | variant.stowed | variant.sheath;
		for (const SwordVariant &variant : twoHandedVariants)
			mask |= variant.right | variant.stowed | variant.sheath;
		for (const PartMask variant : shieldVariants)
			mask |= variant;
		return mask;
	}
};

template <typename FindPart>
RogueEquipmentParts FindRogueEquipmentParts(const FindPart &findPart)
{
	const auto bit = [&](std::string_view name) {
		const int part = findPart(name);
		return part >= 0 && part < MaxSkinnedParts ? PartMask { 1 } << part : PartMask { 0 };
	};
	RogueEquipmentParts parts;
	parts.bow = RogueBowPartMask(findPart);
	parts.completeBow = true;
	for (std::string_view name : RogueBowPartNames)
		parts.completeBow = parts.completeBow && bit(name) != 0;
	parts.sword = bit("Sword_RIGHT");
	parts.swordStowed = bit("Sword_Town_Stowed");
	parts.mace = bit("Mace_RIGHT");
	parts.shield = bit("Shield_LEFT");
	parts.scabbard = bit("Scabbard_SIDE");
	parts.axe = bit("Axe_TWOHANDED");
	parts.maceStowed = bit("Mace_Town_Stowed");
	parts.axeStowed = bit("Axe_Town_Stowed");
	parts.bowStowed = bit("Bow_Town_Stowed");
	parts.quiver = bit("Quiver_Back");
	parts.staff = bit("Staff_RIGHT");
	parts.staffStowed = bit("Staff_Town_Stowed");
	const auto held = [&](std::string_view name, std::string_view hand) {
		return HeldVariant { bit(std::string(name) + std::string(hand)), bit(std::string(name) + "_Town_Stowed") };
	};
	for (size_t i = 0; i < MaceVariantNames.size(); ++i)
		parts.maceVariants[i] = held(MaceVariantNames[i], "_RIGHT");
	for (size_t i = 0; i < StaffVariantNames.size(); ++i)
		parts.staffVariants[i] = held(StaffVariantNames[i], "_RIGHT");
	for (size_t i = 0; i < AxeVariantNames.size(); ++i)
		parts.axeVariants[i] = held(AxeVariantNames[i], "_TWOHANDED");
	for (size_t i = 0; i < WarriorSwordVariantNames.size(); ++i) {
		const std::string name(WarriorSwordVariantNames[i]);
		parts.swordVariants[i] = { bit(name + "_RIGHT"), bit(name + "_Town_Stowed"), bit(name + "_Sheath_Empty") };
	}
	for (size_t i = 0; i < WarriorShieldVariantNames.size(); ++i)
		parts.shieldVariants[i] = bit(std::string(WarriorShieldVariantNames[i]) + "_LEFT");
	for (size_t i = 0; i < TwoHandedVariantNames.size(); ++i) {
		const std::string name(TwoHandedVariantNames[i]);
		parts.twoHandedVariants[i] = { bit(name + "_RIGHT"), bit(name + "_Town_Stowed"), bit(name + "_Sheath_Empty") };
	}
	for (size_t i = 0; i < BowVariantNames.size(); ++i)
		parts.bowVariants[i] = held(BowVariantNames[i], "_LEFT");
	parts.bowArrow = bit("Arrow_Nocked_only");
	return parts;
}

constexpr PartMask RogueSelectedEquipment(SkinnedEquipmentKind equipment, const RogueEquipmentParts &parts, bool town)
{
	switch (equipment) {
	case SkinnedEquipmentKind::Unarmed: return 0;
	case SkinnedEquipmentKind::Bow: return parts.bow;
	case SkinnedEquipmentKind::UnarmedShield: return parts.shield;
	case SkinnedEquipmentKind::Sword: return (town ? parts.swordStowed : parts.sword) | parts.scabbard;
	case SkinnedEquipmentKind::SwordShield: return (town ? parts.swordStowed : parts.sword) | parts.scabbard | parts.shield;
	case SkinnedEquipmentKind::Mace: return parts.mace;
	case SkinnedEquipmentKind::MaceShield: return parts.mace | parts.shield;
	case SkinnedEquipmentKind::Axe: return parts.axe;
	case SkinnedEquipmentKind::Staff: return parts.staff;
	case SkinnedEquipmentKind::TwoHanded: return 0; // the item's own meshes, chosen in RogueVisibleParts
	case SkinnedEquipmentKind::Unsupported: return 0;
	}
	return 0;
}

constexpr bool RogueEquipmentAvailable(SkinnedEquipmentKind equipment, const RogueEquipmentParts &parts, bool town, const SkinnedItemLook &item = {})
{
	switch (equipment) {
	case SkinnedEquipmentKind::Unarmed: return true;
	case SkinnedEquipmentKind::Bow: return parts.completeBow;
	case SkinnedEquipmentKind::UnarmedShield: return parts.shield != 0;
	case SkinnedEquipmentKind::Sword: return (town ? parts.swordStowed : parts.sword) != 0 && parts.scabbard != 0;
	case SkinnedEquipmentKind::SwordShield: return RogueEquipmentAvailable(SkinnedEquipmentKind::Sword, parts, town) && parts.shield != 0;
	case SkinnedEquipmentKind::Mace: return parts.mace != 0;
	case SkinnedEquipmentKind::MaceShield: return parts.mace != 0 && parts.shield != 0;
	case SkinnedEquipmentKind::Axe: return parts.axe != 0;
	case SkinnedEquipmentKind::Staff: return (town ? parts.staffStowed : parts.staff) != 0;
	case SkinnedEquipmentKind::TwoHanded: {
		const auto *own = TwoHandedVariant(parts.twoHandedVariants, item.twoHanded);
		return own != nullptr && (town ? own->stowed : own->right) != 0;
	}
	case SkinnedEquipmentKind::Unsupported: return false;
	}
	return false;
}

/** Town carries the selected weapon's stowed mesh (the bow keeps its quiver); exports without one keep it in hand. */
constexpr PartMask RogueTownCarry(PartMask selected, const RogueEquipmentParts &parts)
{
	const auto stow = [&selected](PartMask hand, PartMask stowed) {
		if ((selected & hand) != 0 && stowed != 0) selected = (selected & ~hand) | stowed;
	};
	stow(parts.mace, parts.maceStowed);
	stow(parts.axe, parts.axeStowed);
	stow(parts.staff, parts.staffStowed);
	stow(parts.bow & ~parts.quiver, parts.bowStowed);
	return selected;
}

/** The Rogue's parts with the equipped items' own meshes swapped in, where the export has them. */
constexpr RogueEquipmentParts RogueItemParts(const RogueEquipmentParts &parts, const SkinnedItemLook &item)
{
	RogueEquipmentParts look = parts;
	SwapHeldVariant(look.mace, look.maceStowed, parts.maceVariants, item.mace);
	SwapHeldVariant(look.staff, look.staffStowed, parts.staffVariants, item.staff);
	SwapHeldVariant(look.axe, look.axeStowed, parts.axeVariants, item.axe);
	if (const auto *own = BowVariantOf(parts, item.bow)) { // the arrow and quiver stay shared
		look.bow = own->held | parts.bowArrow | parts.quiver;
		look.bowStowed = own->stowed;
	}
	if (item.sword != WarriorSwordVariant::Sword) {
		const auto &own = parts.swordVariants[static_cast<size_t>(item.sword) - 1];
		if (own.right != 0 && own.stowed != 0) { // the variant's own sheath replaces the scabbard
			look.sword = own.right | own.sheath;
			look.swordStowed = own.stowed;
			look.scabbard = 0;
		}
	}
	if (item.shield != WarriorShieldVariant::Shield) {
		const PartMask own = parts.shieldVariants[static_cast<size_t>(item.shield) - 1];
		if (own != 0) look.shield = own;
	}
	return look;
}

constexpr PartMask RogueVisibleParts(SkinnedEquipmentKind equipment, const RogueEquipmentParts &parts, bool town, const SkinnedItemLook &item = {}, bool dead = false)
{
	if (dead) return ~(parts.all() | parts.quiver);
	if (!RogueEquipmentAvailable(equipment, parts, town, item)) return 0;
	if (equipment == SkinnedEquipmentKind::TwoHanded) {
		const auto &own = *TwoHandedVariant(parts.twoHandedVariants, item.twoHanded);
		return ~parts.all() | (town ? own.stowed : own.right | own.sheath);
	}
	const RogueEquipmentParts look = RogueItemParts(parts, item);
	const PartMask selected = RogueSelectedEquipment(equipment, look, town);
	return ~parts.all() | (town ? RogueTownCarry(selected, look) : selected);
}

// Compatibility for bow-only exports.
constexpr PartMask RogueVisibleParts(SkinnedEquipmentKind equipment, PartMask bowParts)
{
	return equipment == SkinnedEquipmentKind::Bow    ? ~PartMask { 0 }
	    : equipment == SkinnedEquipmentKind::Unarmed ? ~bowParts
	                                                 : PartMask { 0 };
}

/** A material's decoded texture images (-1 when absent or undecodable). */
struct SkinnedMaterialImages {
	int base = -1, normal = -1, mr = -1;
};
/** One equipment texture page: a layer of the base colour, normal and metallic-roughness texture arrays. */
struct SkinnedEquipmentPage {
	int base = -1, normal = -1, mr = -1;
};
/** Which page a material samples, and which of that page's maps. */
struct SkinnedMaterialPage {
	int page = -1;
	bool base = false, normal = false, mr = false;
};

/**
 * Groups equipment textures into pages, so a model can add equipment sheets without new bindings: each distinct
 * non-body base colour image starts a page, in material order, and that page's normal and metallic-roughness maps
 * come from its materials. A material without an equipment base colour (untextured factors) joins the page whose
 * normal or metallic-roughness map it shares, else the first page.
 */
inline std::vector<SkinnedEquipmentPage> AssignEquipmentPages(const std::vector<SkinnedMaterialImages> &materials, int bodyImage,
    std::vector<SkinnedMaterialPage> &assigned)
{
	std::vector<SkinnedEquipmentPage> pages;
	assigned.assign(materials.size(), {});
	for (size_t i = 0; i < materials.size(); ++i) {
		const int base = materials[i].base;
		if (base < 0 || base == bodyImage) continue;
		auto it = std::find_if(pages.begin(), pages.end(), [&](const SkinnedEquipmentPage &page) { return page.base == base; });
		if (it == pages.end()) it = pages.insert(pages.end(), { base, -1, -1 });
		assigned[i] = { static_cast<int>(it - pages.begin()), true, false, false };
	}
	for (size_t i = 0; i < materials.size(); ++i) {
		const SkinnedMaterialImages &images = materials[i];
		SkinnedMaterialPage &page = assigned[i];
		if (page.page < 0 && (images.normal >= 0 || images.mr >= 0)) {
			for (size_t p = 0; p < pages.size() && page.page < 0; ++p)
				if ((images.normal >= 0 && pages[p].normal == images.normal) || (images.mr >= 0 && pages[p].mr == images.mr))
					page.page = static_cast<int>(p);
			if (page.page < 0 && !pages.empty()) page.page = 0;
			if (page.page < 0) {
				pages.push_back({});
				page.page = static_cast<int>(pages.size() - 1);
			}
		}
		if (page.page < 0) continue;
		SkinnedEquipmentPage &target = pages[static_cast<size_t>(page.page)];
		if (images.normal >= 0 && target.normal < 0) target.normal = images.normal;
		if (images.mr >= 0 && target.mr < 0) target.mr = images.mr;
		page.normal = images.normal >= 0 && target.normal == images.normal;
		page.mr = images.mr >= 0 && target.mr == images.mr;
	}
	return pages;
}

constexpr bool SkinnedPoseAvailable(bool enabled, bool loaded, bool gpuBroken, int clip)
{
	return enabled && loaded && !gpuBroken && clip >= 0;
}

inline void SetSkinnedJointBit(std::array<uint32_t, SkinnedJointMaskWords> &mask, size_t entry)
{
	if (entry < MaxSkinnedJoints) mask[entry / 32] |= 1U << (entry % 32);
}

/** Map simulation frames to authored impact, queued join and stopped recovery.
 * The repeat core carries follow-through across its boundary; its remaining tail
 * supplies stopped recovery without wrapping into a phantom additional strike.
 * Equipment speed changes simulation progress, so there is no wall-clock timer.
 */
inline float RogueAttackSampleTime(float frame, int actionFrame, int frameCount, bool repeat,
    float clipDuration, float hitTime, float repeatPeriod)
{
	const float hit = static_cast<float>(std::max(actionFrame - 1, 1));
	const float frames = static_cast<float>(std::max(frameCount, actionFrame + 1));
	frame = std::clamp(frame, 0.0F, frames);
	float time;
	if (repeat) {
		const float end = hit + 1.0F;
		time = frame <= hit ? frame / hit * hitTime
		    : frame <= end  ? hitTime + (frame - hit) * (repeatPeriod - hitTime)
		                    : repeatPeriod + (frame - end) / std::max(frames - end, 1.0F) * (clipDuration - repeatPeriod);
	} else {
		time = frame <= hit ? frame / hit * hitTime
		                    : hitTime + (frame - hit) / std::max(frames - hit, 1.0F) * (clipDuration - hitTime);
	}
	return std::clamp(time, 0.0F, std::max(clipDuration, 0.0F));
}

/** The Warrior mace impacts at action frame 9 and joins the next swing one frame later. */
inline float WarriorMaceSampleTime(float frame, int actionFrame, int frameCount, bool repeat, float clipDuration)
{
	return RogueAttackSampleTime(frame, actionFrame, frameCount, repeat, clipDuration, .40F, .45F);
}

/** The Warrior axe keeps the accepted two-handed strike's impact and queued join. */
inline float WarriorAxeSampleTime(float frame, int actionFrame, int frameCount, bool repeat, float clipDuration)
{
	return RogueAttackSampleTime(frame, actionFrame, frameCount, repeat, clipDuration, .45F, .50F);
}

/** The Warrior bow releases on the simulation action frame, including accelerated shots. */
inline float WarriorBowSampleTime(float frame, int actionFrame, int frameCount, bool repeat, float clipDuration)
{
	return RogueAttackSampleTime(frame, actionFrame, frameCount, repeat, clipDuration, .50F, .55F);
}

/** The Warrior staff sweep impacts at action frame 11 of 16 (0.5 s) and joins the next swing one frame later. */
inline float WarriorStaffSampleTime(float frame, int actionFrame, int frameCount, bool repeat, float clipDuration)
{
	return RogueAttackSampleTime(frame, actionFrame, frameCount, repeat, clipDuration, .50F, .55F);
}

inline float RogueBowSampleTime(float frame, int actionFrame, int frameCount, bool repeat, float clipDuration)
{
	return RogueAttackSampleTime(frame, actionFrame, frameCount, repeat, clipDuration, .30F, .35F);
}

} // namespace devilution::render3d
