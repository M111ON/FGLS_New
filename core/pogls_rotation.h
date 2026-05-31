/*
 * pogls_rotation.h — bridge to the active updates lane
 */
#ifndef POGLS_ROTATION_H
#define POGLS_ROTATION_H

#if __has_include("../active_updates/pogls_rotation.h")
#include "../active_updates/pogls_rotation.h"
#else
#error "pogls_rotation.h: no active handoff implementation found"
#endif

#endif /* POGLS_ROTATION_H */
