#pragma once

#include "ignis/core/types.h"

namespace ignis {

// Entities are lightweight ids; all data lives in components inside the World.
using EntityId = u32;
inline constexpr EntityId NULL_ENTITY = 0;

} // namespace ignis
