/**
 * @file itemlabels.h
 *
 * Adds item labels QoL feature
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

void ToggleItemLabelHighlight();
void HighlightKeyPressed(bool pressed);
bool IsItemLabelHighlighted();
void ResetItemlabelHighlighted();
bool IsHighlightingLabelsEnabled();
void AddItemToLabelQueue(int id, Point position);
/** First-person view: queues a label whose bottom centre is at a UI-space point (already projected). */
void AddItemToLabelQueueAt(int id, Point uiAnchor);
void DrawItemNameLabels(const Surface &out);

} // namespace devilution
