/*
 * gpx4_container.h — bridge to the active GeoPixel update lane
 */
#ifndef GPX4_CONTAINER_H
#define GPX4_CONTAINER_H

#if __has_include("../geopixel/geopixel/gpx4_container.h")
#include "../geopixel/geopixel/gpx4_container.h"
#else
#error "gpx4_container.h: no active container implementation found"
#endif

#endif /* GPX4_CONTAINER_H */
