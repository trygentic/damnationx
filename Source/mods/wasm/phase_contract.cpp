#include "mods/wasm/phase_contract.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace devilution::wasm {

PhaseJournal::PhaseJournal(Snapshot snapshot, Checkpoint checkpoint)
    : snapshot_(std::move(snapshot)), checkpoint_(std::move(checkpoint)) {}

bool PhaseJournal::QueueCommand(Command command)
{
	if (aborted_ || command.targetSlot == 0 || command.targetGeneration == 0
	    || command.sequence != commands_.size() || command.modOrder > 0xFFFFu)
		return false;
	if (snapshot_.identity.checkpointHash != checkpoint_.hash)
		return false;
	commands_.push_back(command);
	return true;
}

bool PhaseJournal::QueueLight(LightRequest request)
{
	if (aborted_ || request.ownerSlot == 0 || request.ownerGeneration == 0
	    || request.radius == 0 || request.radius > 128 || request.sequence != lights_.size())
		return false;
	lights_.push_back(request);
	return true;
}

bool PhaseJournal::ConsumeRng(uint32_t amount)
{
	if (aborted_ || amount > 1024 || stagedRngUses_ > std::numeric_limits<uint32_t>::max() - amount)
		return false;
	stagedRngUses_ += amount;
	return true;
}

bool PhaseJournal::Validate() const
{
	if (aborted_ || snapshot_.identity.checkpointHash != checkpoint_.hash)
		return false;
	for (size_t i = 0; i < commands_.size(); ++i) {
		if (commands_[i].sequence != i || commands_[i].targetSlot == 0 || commands_[i].targetGeneration == 0)
			return false;
		for (size_t j = i + 1; j < commands_.size(); ++j) {
			if (commands_[i].targetSlot == commands_[j].targetSlot
			    && commands_[i].targetGeneration == commands_[j].targetGeneration
			    && commands_[i].field == commands_[j].field
			    && (!commands_[i].additive || !commands_[j].additive)
			    && (commands_[i].value != commands_[j].value || commands_[i].modOrder != commands_[j].modOrder))
				return false;
		}
	}
	return std::all_of(lights_.begin(), lights_.end(), [](const LightRequest &light) {
		return light.ownerSlot != 0 && light.ownerGeneration != 0 && light.radius <= 128;
	});
}

void PhaseJournal::Abort()
{
	commands_.clear();
	lights_.clear();
	stagedRngUses_ = 0;
	aborted_ = true;
}

} // namespace devilution::wasm
