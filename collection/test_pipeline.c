#include <stdio.h>
#include "pogls_pipeline.h"

int main(void) {
    int r = pogls_pipeline_verify();
    if (r == 0) {
        printf("PASS: all 8 tests\n");
    } else {
        printf("FAIL: test %d\n", r);
    }

    /* Print key sizes for reference */
    printf("POGLSHeader    = %u B\n", (unsigned)sizeof(POGLSHeader));
    printf("ChunkDesc      = %u B\n", (unsigned)sizeof(ChunkDesc));
    printf("ColdEntry      = %u B\n", (unsigned)sizeof(ColdEntry));
    printf("ColdStore      = %u B\n", (unsigned)sizeof(ColdStore));
    printf("HotStore       = %u B\n", (unsigned)sizeof(HotStore));
    printf("PoglsPipeline  = %u B\n", (unsigned)sizeof(PoglsPipeline));
    printf("PipelineStats  = %u B\n", (unsigned)sizeof(PipelineStats));
    return r;
}
