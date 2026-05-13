/*
 * geo_o4_connector.h — bridge to the active GeoPixel update lane
 */
#ifndef GEO_O4_CONNECTOR_H
#define GEO_O4_CONNECTOR_H

#if __has_include("../geopixel/geopixel/geo_o4_connector.h")
#include "../geopixel/geopixel/geo_o4_connector.h"
#else
#error "geo_o4_connector.h: no active connector implementation found"
#endif

#endif /* GEO_O4_CONNECTOR_H */
