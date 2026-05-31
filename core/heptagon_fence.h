/*
 * heptagon_fence.h — bridge to the active updates lane
 */
#ifndef HEPTAGON_FENCE_H
#define HEPTAGON_FENCE_H

#if __has_include("../active_updates/heptagon_fence.h")
#include "../active_updates/heptagon_fence.h"
#else
#error "heptagon_fence.h: no active handoff implementation found"
#endif

#endif /* HEPTAGON_FENCE_H */
