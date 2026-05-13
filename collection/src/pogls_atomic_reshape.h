/*
 * pogls_atomic_reshape.h — bridge to the active updates lane
 */
#ifndef POGLS_ATOMIC_RESHAPE_H
#define POGLS_ATOMIC_RESHAPE_H

#if __has_include("../active_updates/pogls_atomic_reshape.h")
#include "../active_updates/pogls_atomic_reshape.h"
#else
#error "pogls_atomic_reshape.h: no active handoff implementation found"
#endif

#endif /* POGLS_ATOMIC_RESHAPE_H */
