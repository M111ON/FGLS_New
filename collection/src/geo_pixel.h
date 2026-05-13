/*
 * geo_pixel.h — bridge to the active GeoPixel update lane
 */
#ifndef GEO_PIXEL_H
#define GEO_PIXEL_H

#if __has_include("../geopixel/geopixel/geo_pixel.h")
#include "../geopixel/geopixel/geo_pixel.h"
#else
#error "geo_pixel.h: no active pixel implementation found"
#endif

#endif /* GEO_PIXEL_H */
