/*
 * fabric_wire_drain.h — bridge to the active updates lane
 */
#ifndef FABRIC_WIRE_DRAIN_H
#define FABRIC_WIRE_DRAIN_H

#if __has_include("../active_updates/fabric_wire_drain.h")
#include "../active_updates/fabric_wire_drain.h"
#else
#error "fabric_wire_drain.h: no active handoff implementation found"
#endif

#endif /* FABRIC_WIRE_DRAIN_H */
