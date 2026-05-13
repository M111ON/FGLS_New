/*
 * geo_gpx_anim.h — bridge to the active GeoPixel update lane
 *
 * Prefer the newer implementation under geopixel/geopixel.
 */
#ifndef GEO_GPX_ANIM_H
#define GEO_GPX_ANIM_H

#if __has_include("../geopixel/geopixel/geo_gpx_anim_o23.h")
#include "../geopixel/geopixel/geo_gpx_anim_o23.h"
#else
#error "geo_gpx_anim.h: no active GeoPixel implementation found"
#endif

#endif /* GEO_GPX_ANIM_H */
