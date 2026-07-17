#include <stdio.h>
#include <stdint.h>
#include "geo_tring_goldberg_wire.h"
#include "tgw_stream_dispatch.h"
int main(void) {
    printf("sizeof(TStreamPkt)=%zu\n", sizeof(TStreamPkt));
    printf("sizeof(TStreamChunk)=%zu\n", sizeof(TStreamChunk));
    printf("sizeof(TGWCtx)=%zu\n", sizeof(TGWCtx));
    printf("sizeof(TGWDispatch)=%zu\n", sizeof(TGWDispatch));
    printf("TSTREAM_MAX_PKTS=%u\n", TSTREAM_MAX_PKTS);
    printf("stack for pkts[720] = %zu bytes\n", sizeof(TStreamPkt) * TSTREAM_MAX_PKTS);
    return 0;
}
