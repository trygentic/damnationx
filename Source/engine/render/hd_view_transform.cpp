#include "engine/render/hd_view_transform.hpp"

namespace devilution {
namespace {

HdWorldTransform CurrentTransform;

} // namespace

const HdWorldTransform &HdCurrentWorldTransform()
{
	return CurrentTransform;
}

void HdSetWorldTransform(const HdWorldTransform &transform)
{
	CurrentTransform = transform;
}

} // namespace devilution
