#include <gtest/gtest.h>

#include "dvlnet/session.h"

namespace devilution::net::session {
namespace {
Hello HelloFor(std::string mod = "base")
{
	return Hello{WireVersion, Capability::WebSocket | Capability::StateHash, {"1", "classic", std::move(mod), 1}};
}

TEST(DvlNetSession, NegotiatesCompatiblePeers)
{
	auto result = Negotiate(HelloFor(), HelloFor());
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->version, WireVersion);
	EXPECT_TRUE(result->capabilities & Capability::StateHash);
}

TEST(DvlNetSession, RejectsVersionAndModMismatch)
{
	auto version = HelloFor();
	version.version++;
	EXPECT_EQ(Negotiate(HelloFor(), version).error(), RejectReason::UnsupportedVersion);
	auto mod = HelloFor("other");
	EXPECT_EQ(Negotiate(HelloFor(), mod).error(), RejectReason::ModMismatch);
}

TEST(DvlNetSession, EncodesStateHashAndRejectsMalformedInput)
{
	Message message;
	message.type = MessageType::StateHash;
	message.sequence = 4;
	message.state = {12, 0x1234};
	auto encoded = Encode(message);
	ASSERT_TRUE(encoded.has_value());
	auto decoded = Decode(*encoded);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->state.hash, message.state.hash);
	encoded->push_back(0);
	EXPECT_EQ(Decode(*encoded).error(), RejectReason::Malformed);
}

TEST(DvlNetSession, ValidatesSequenceAndWraps)
{
	EXPECT_TRUE(ValidateSequence(4, 5).has_value());
	EXPECT_TRUE(ValidateSequence(UINT32_MAX, 0).has_value());
	EXPECT_EQ(ValidateSequence(4, 6).error(), RejectReason::SequenceViolation);
}
} // namespace
} // namespace devilution::net::session
