#pragma once
#ifndef VISUAL_ANCHOR_H
#define VISUAL_ANCHOR_H

#include "hwagd_structs.h"
#include <UIAutomation.h>

// Captures a 200x200 pixel visual crop of the focused UI Automation element and returns its Base64 PNG + SHA256 Hash.
// Includes a check to ensure the captured area is not fully occluded/black.
VisualAnchorArtifact CaptureLocalizedVisualAnchor(IUIAutomationElement* pElement);

#endif // VISUAL_ANCHOR_H
