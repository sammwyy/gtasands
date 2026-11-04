#pragma once

#include "game/text/gxt.h"

// CTheZones-lite: parses data/info.zon and resolves the smallest zone box
// containing a world point, returning its GXT key (like CTheZones::FindZone).
namespace zones {

const GxtChar* nameAt(float x, float y); // GXT string or nullptr

}
