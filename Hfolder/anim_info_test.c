#define GEO_GPX_ANIM_IMPL
#include "geo_gpx_anim.h"
#include <stdio.h>
int main(void){
    Gpx4AnimHdr h;
    if(gpx_anim_info("animated_test.gpx4", &h)!=0){ puts("info-fail"); return 1; }
    printf("frames=%u fps=%u/%u kfi=%u size=%ux%u\n", h.n_frames,h.fps_num,h.fps_den,h.keyframe_interval,h.width_px,h.height_px);
    return 0;
}
