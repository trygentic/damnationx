#pragma once

#include <cstdint>
#include <vector>

namespace devilution::wasm {

// Runtime-neutral journal used by the embedded wasmi adapter on every platform.
// Bytes are owned by the host; guests only receive copies of them.
struct PhaseIdentity {
	uint64_t checkpointHash = 0;
	uint32_t tick = 0;
	uint32_t snapshotVersion = 1;
	bool operator==(const PhaseIdentity &) const = default;
};

struct Snapshot {
	PhaseIdentity identity;
	std::vector<uint8_t> bytes;
};

struct Event {
	uint32_t sequence = 0;
	uint32_t type = 0;
	std::vector<uint8_t> payload;
};

struct Command {
	uint32_t modOrder = 0;
	uint32_t sequence = 0;
	uint32_t targetSlot = 0;
	uint32_t targetGeneration = 0;
	uint32_t field = 0;
	int32_t value = 0;
	bool additive = false;
};

struct LightRequest {
	uint32_t modOrder = 0;
	uint32_t sequence = 0;
	uint32_t ownerSlot = 0;
	uint32_t ownerGeneration = 0;
	uint32_t radius = 0;
	uint32_t rgb = 0;
};

struct Checkpoint {
	uint64_t hash = 0;
	std::vector<uint8_t> authoritativeState;
	std::vector<uint8_t> modState;
	uint64_t rngState = 0;
	uint32_t nextEventSequence = 0;
	uint32_t nextCommandSequence = 0;
};

class PhaseJournal {
public:
	explicit PhaseJournal(Snapshot snapshot, Checkpoint checkpoint);

	const Snapshot &GetSnapshot() const { return snapshot_; }
	const Checkpoint &GetCheckpoint() const { return checkpoint_; }
	const std::vector<Command> &Commands() const { return commands_; }
	const std::vector<LightRequest> &Lights() const { return lights_; }

	bool QueueCommand(Command command);
	bool QueueLight(LightRequest request);
	bool ConsumeRng(uint32_t amount);
	bool Validate() const;
	void Abort();
	bool Aborted() const { return aborted_; }

	// The caller replaces quarantined guests with instances loaded from this
	// exact checkpoint; no guest-owned memory is retained by the journal.
	Checkpoint RecoveryCheckpoint() const { return checkpoint_; }

private:
	Snapshot snapshot_;
	Checkpoint checkpoint_;
	std::vector<Command> commands_;
	std::vector<LightRequest> lights_;
	uint64_t stagedRngUses_ = 0;
	bool aborted_ = false;
};

} // namespace devilution::wasm
