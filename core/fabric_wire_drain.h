/*
 * fabric_wire_drain.h — bridge to the active updates lane
 */
#ifndef FABRIC_WIRE_DRAIN_BRIDGE_H
#define FABRIC_WIRE_DRAIN_BRIDGE_H

/* ── Include the real implementation ────────────────────────
 * NOTE: must undef FABRIC_WIRE_DRAIN_H first because the bridge
 * header guards are the same name as the active_updates header.
 * The bridge sets FABRIC_WIRE_DRAIN_H, then the active_updates
 * version sees it's already defined and skips its contents.
 * We use a separate guard name to avoid the collision. */
#if __has_include("../collection/active_updates/fabric_wire_drain.h")
#include "../collection/active_updates/fabric_wire_drain.h"
#else
#error "fabric_wire_drain.h: no active handoff implementation found"
#endif

#endif /* FABRIC_WIRE_DRAIN_BRIDGE_H */
