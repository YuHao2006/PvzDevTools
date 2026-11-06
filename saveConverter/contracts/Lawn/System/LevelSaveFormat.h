#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// users.dat (the profile summary list) remains a byte-for-byte 1.2.0.1073
// file.  Per-player details moved to user<ID>.dat64 = the 8-byte magic below
// + the 1073 SyncDetails byte stream at version 13 (GameMode v3 direct
// record slots; v12 migration is the standalone converter's job, the game
// has no runtime compatibility layer).  The Board snapshot schema here is
// for game<ID>_<mode>.dat64.
namespace LevelSaveFormat
{
inline constexpr std::array<std::uint8_t, 8> USER64_MAGIC = {
    'P', 'V', 'Z', 'U', 'S', 'R', '6', '4'
};
inline constexpr std::uint32_t USER64_HEADER_SIZE = 8;
inline constexpr std::uint32_t USER64_VERSION = 13;

inline constexpr std::array<std::uint8_t, 8> DAT64_MAGIC = {
	'P', 'V', 'Z', 'D', 'A', 'T', '6', '4'
};
inline constexpr std::uint32_t DAT64_V1_VERSION = 1;
inline constexpr std::uint32_t DAT64_V2_VERSION = 2;
inline constexpr std::uint32_t DAT64_VERSION = 4;
inline constexpr std::uint32_t DAT64_MINOR_VERSION = 0;
inline constexpr std::uint32_t DAT64_HEADER_SIZE = 48;
inline constexpr std::uint32_t DAT64_AUTHORITY_BUILD = 1073;
inline constexpr std::uint32_t DAT64_FLAG_LITTLE_ENDIAN = 1U << 0;
inline constexpr std::uint32_t DAT64_FLAG_NANOSECONDS = 1U << 1;
inline constexpr std::uint32_t DAT64_V1_FLAGS = DAT64_FLAG_LITTLE_ENDIAN;
inline constexpr std::uint32_t DAT64_FLAGS = DAT64_FLAG_LITTLE_ENDIAN | DAT64_FLAG_NANOSECONDS;
inline constexpr std::uint32_t DAT64_V1_SCHEMA_ID = 0x00010731U;
inline constexpr std::uint32_t DAT64_V2_SCHEMA_ID = 0x00010732U;
inline constexpr std::uint32_t DAT64_SCHEMA_ID = 0x00010745U;
inline constexpr std::uint64_t DAT64_END_MARKER = 0xB4A9ABBEBBA5A9AFULL;

// Header bytes, all little-endian except the literal magic:
//   00 char[8] magic = "PVZDAT64"
//   08 u32 format version
//   0C u32 header size
//   10 u32 authority build (1073)
//   14 u32 flags
//   18 u64 payload size
//   20 u32 ignored former payload CRC32 (v2/v4 write zero; v1 exports retain CRC)
//   24 u32 schema id
//   28 u32 minor version (zero in all supported formats)
//   2C u32 reserved (must be zero)
// RECORD_LAYOUTS and offsets below describe the fixed v1 canonical ABI.
// Dat64TimeSchema expands time fields for v2/v4; v4 appends current exact
// runtime state. All v3 development formats are unsupported.

// Shared by the game reader and converter. Offset 0x20 deliberately has no
// meaning here: valid field edits require no checksum or hash update.
constexpr bool IsSupportedDat64Header(const std::span<const std::uint8_t>& file)
{
	if (file.size() < DAT64_HEADER_SIZE) return false;
	for (std::size_t i = 0; i < DAT64_MAGIC.size(); ++i)
		if (file[i] != DAT64_MAGIC[i]) return false;
	const auto read = [&](std::size_t offset, unsigned length) {
		std::uint64_t value = 0;
		for (unsigned i = 0; i < length; ++i)
			value |= std::uint64_t(file[offset + i]) << (i * 8);
		return value;
	};
    const auto version = read(8, 4);
    const bool current = version == DAT64_VERSION &&
        read(40, 4) == DAT64_MINOR_VERSION && read(36, 4) == DAT64_SCHEMA_ID;
    const bool v1 = version == DAT64_V1_VERSION && read(40, 4) == 0 &&
        read(36, 4) == DAT64_V1_SCHEMA_ID;
    const bool v2 = version == DAT64_V2_VERSION && read(40, 4) == 0 &&
        read(36, 4) == DAT64_V2_SCHEMA_ID;
    return (current || v1 || v2) && read(12, 4) == DAT64_HEADER_SIZE &&
        read(16, 4) == DAT64_AUTHORITY_BUILD &&
        read(20, 4) == (v1 ? DAT64_V1_FLAGS : DAT64_FLAGS) &&
        read(24, 8) == file.size() - DAT64_HEADER_SIZE && read(44, 4) == 0;
}

// The runtime accepts only the current v4 format. Historical formats
// are inputs to the standalone converter, never alternate game load paths.
constexpr bool IsCurrentDat64Header(const std::span<const std::uint8_t>& file)
{
	return IsSupportedDat64Header(file) && file[8] == DAT64_VERSION &&
		file[40] == DAT64_MINOR_VERSION &&
		std::uint32_t(file[36] | (std::uint32_t(file[37]) << 8) |
			(std::uint32_t(file[38]) << 16) | (std::uint32_t(file[39]) << 24)) == DAT64_SCHEMA_ID;
}

struct CopySegment
{
	std::uint32_t mLegacyOffset;
	std::uint32_t mDat64Offset;
	std::uint32_t mSize;
};

enum class RecordKind : std::uint32_t
{
	Zombie,
	Plant,
	Projectile,
	Coin,
	LawnMower,
	GridItem,
	ParticleSystem,
	ParticleEmitter,
	Particle,
	Reanimation,

	Attachment,
	CursorObject,
	CursorPreview,
	MessageWidget,
	SeedPacket,
	Challenge,
	Music,
	ReanimatorTrackInstance
};

struct RecordLayout
{
	RecordKind mKind;
	std::uint32_t mLegacySize;
	std::uint32_t mDat64Size;
	std::uint32_t mLegacyArraySlotSize;
	std::uint32_t mDat64ArraySlotSize;
	const CopySegment* mSegments;
	std::size_t mSegmentCount;
};

inline constexpr CopySegment GAME_OBJECT_ZOMBIE_SEGMENTS[] = {
	{8, 16, 28}, {36, 48, 320}
};
inline constexpr CopySegment GAME_OBJECT_PLANT_SEGMENTS[] = {
	{8, 16, 28}, {36, 48, 292}
};
inline constexpr CopySegment GAME_OBJECT_PROJECTILE_SEGMENTS[] = {
	{8, 16, 28}, {36, 48, 108}
};
inline constexpr CopySegment COIN_SEGMENTS[] = {
	{8, 16, 28}, {36, 48, 72}, {112, 120, 96}
};
inline constexpr CopySegment LAWN_MOWER_SEGMENTS[] = {
	{8, 16, 60}
};
inline constexpr CopySegment GRID_ITEM_SEGMENTS[] = {
	{8, 16, 224}
};
inline constexpr CopySegment PARTICLE_SYSTEM_SEGMENTS[] = {
	{0, 0, 4}, {28, 56, 12}
};
inline constexpr CopySegment PARTICLE_EMITTER_SEGMENTS[] = {
	{24, 48, 60}, {88, 120, 84}
};
inline constexpr CopySegment PARTICLE_SEGMENTS[] = {
	{4, 8, 152}
};
inline constexpr CopySegment REANIMATION_SEGMENTS[] = {
	{0, 0, 12}, {16, 24, 72}, {92, 104, 4}, {100, 120, 56}
};
inline constexpr CopySegment TRAIL_SEGMENTS[] = {
	{0, 0, 180}, {188, 200, 44}
};
inline constexpr CopySegment ATTACHMENT_SEGMENTS[] = {
	{0, 0, 776}
};
inline constexpr CopySegment CURSOR_OBJECT_SEGMENTS[] = {
	{8, 16, 28}, {36, 48, 40}
};
inline constexpr CopySegment CURSOR_PREVIEW_SEGMENTS[] = {
	{8, 16, 28}, {36, 48, 8}
};
inline constexpr CopySegment MESSAGE_WIDGET_SEGMENTS[] = {
	{4, 8, 792}
};
inline constexpr CopySegment SEED_PACKET_SEGMENTS[] = {
	{8, 16, 28}, {36, 48, 44}
};
inline constexpr CopySegment CHALLENGE_SEGMENTS[] = {
	{8, 16, 180}
};
inline constexpr CopySegment MUSIC_SEGMENTS[] = {
	{8, 16, 68}
};
inline constexpr CopySegment REANIM_TRACK_SEGMENTS[] = {
	{0, 0, 40}, {52, 64, 16}, {72, 88, 24}
};

template <std::size_t N>
constexpr RecordLayout MakeLayout(
	RecordKind theKind,
	std::uint32_t theLegacySize,
	std::uint32_t theDat64Size,
	std::uint32_t theLegacySlotSize,
	std::uint32_t theDat64SlotSize,
	const CopySegment (&theSegments)[N])
{
	return {
		theKind,
		theLegacySize,
		theDat64Size,
		theLegacySlotSize,
		theDat64SlotSize,
		theSegments,
		N
	};
}

inline constexpr std::array<RecordLayout, 18> RECORD_LAYOUTS = {
	MakeLayout(RecordKind::Zombie, 356, 368, 360, 376, GAME_OBJECT_ZOMBIE_SEGMENTS),
	MakeLayout(RecordKind::Plant, 328, 344, 332, 352, GAME_OBJECT_PLANT_SEGMENTS),
	MakeLayout(RecordKind::Projectile, 144, 160, 148, 168, GAME_OBJECT_PROJECTILE_SEGMENTS),
	MakeLayout(RecordKind::Coin, 208, 216, 216, 224, COIN_SEGMENTS),
	MakeLayout(RecordKind::LawnMower, 68, 80, 72, 88, LAWN_MOWER_SEGMENTS),
	MakeLayout(RecordKind::GridItem, 232, 240, 236, 248, GRID_ITEM_SEGMENTS),
	MakeLayout(RecordKind::ParticleSystem, 40, 72, 44, 80, PARTICLE_SYSTEM_SEGMENTS),
	MakeLayout(RecordKind::ParticleEmitter, 172, 208, 176, 216, PARTICLE_EMITTER_SEGMENTS),
	MakeLayout(RecordKind::Particle, 156, 160, 160, 168, PARTICLE_SEGMENTS),
	MakeLayout(RecordKind::Reanimation, 156, 176, 160, 184, REANIMATION_SEGMENTS),  // 文件记录不含尾部 24B 动作块缓存（纯派生数据，读端 ClearNativeObject 归零）
	MakeLayout(RecordKind::Attachment, 776, 776, 780, 780, ATTACHMENT_SEGMENTS),
	MakeLayout(RecordKind::CursorObject, 76, 88, 0, 0, CURSOR_OBJECT_SEGMENTS),
	MakeLayout(RecordKind::CursorPreview, 44, 56, 0, 0, CURSOR_PREVIEW_SEGMENTS),
	MakeLayout(RecordKind::MessageWidget, 796, 800, 0, 0, MESSAGE_WIDGET_SEGMENTS),
	MakeLayout(RecordKind::SeedPacket, 80, 96, 0, 0, SEED_PACKET_SEGMENTS),
	MakeLayout(RecordKind::Challenge, 188, 200, 0, 0, CHALLENGE_SEGMENTS),
	MakeLayout(RecordKind::Music, 76, 88, 0, 0, MUSIC_SEGMENTS),
	MakeLayout(RecordKind::ReanimatorTrackInstance, 96, 112, 0, 0, REANIM_TRACK_SEGMENTS)
};

constexpr const RecordLayout& GetRecordLayout(RecordKind theKind)
{
	return RECORD_LAYOUTS[static_cast<std::size_t>(theKind)];
}

inline constexpr std::uint32_t LEGACY_BOARD_STATE_SIZE = 22108;
inline constexpr std::uint32_t DAT64_BOARD_STATE_SIZE = 22120;
inline constexpr std::uint32_t LEGACY_BOARD_GAME_ID_OFFSET = 22076;
inline constexpr std::uint32_t DAT64_BOARD_GAME_ID_OFFSET = 22080;
inline constexpr std::uint32_t LEGACY_BOARD_AFTER_GAME_ID_OFFSET = 22080;
inline constexpr std::uint32_t DAT64_BOARD_AFTER_GAME_ID_OFFSET = 22088;
inline constexpr std::uint32_t BOARD_AFTER_GAME_ID_SIZE = 28;

inline constexpr std::uint32_t LEGACY_SEED_BANK_SIZE = 848;
inline constexpr std::uint32_t DAT64_SEED_BANK_SIZE = 1024;
inline constexpr std::uint32_t SEED_BANK_PACKET_COUNT = 10;
inline constexpr std::uint32_t LEGACY_SEED_BANK_PACKET_OFFSET = 40;
inline constexpr std::uint32_t DAT64_SEED_BANK_PACKET_OFFSET = 56;
inline constexpr std::uint32_t LEGACY_SEED_BANK_TAIL_OFFSET = 840;
inline constexpr std::uint32_t DAT64_SEED_BANK_TAIL_OFFSET = 1016;
inline constexpr std::uint32_t SEED_BANK_TAIL_SIZE = 8;
}
