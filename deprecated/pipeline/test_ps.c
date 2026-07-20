#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pipeline_store.h"
int main() { PipelineStore s; ps_init(&s, 1024*1024); ps_destroy(&s); return 0; }
