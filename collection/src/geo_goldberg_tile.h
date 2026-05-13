/*
 * geo_goldberg_tile.h — bridge to the active GeoPixel update lane
 */
#ifndef GEO_GOLDBERG_TILE_H
#define GEO_GOLDBERG_TILE_H

#if __has_include("../geopixel/geopixel/geo_goldberg_tile.h")
#include "../geopixel/geopixel/geo_goldberg_tile.h"
#else
#error "geo_goldberg_tile.h: no active goldberg tile implementation found"
#endif

#endif /* GEO_GOLDBERG_TILE_H */
