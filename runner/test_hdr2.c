#include <stdio.h>
#include <stdint.h>
int main() {
    FILE *f = fopen("test_qwen.pogls","rb");
    if(!f){printf("cannot open test_qwen.pogls\n");return 1;}
    uint8_t buf[128]; fread(buf,1,128,f); fclose(f);
    uint32_t magic=*(uint32_t*)buf, ver=*(uint32_t*)(buf+4), flags=*(uint32_t*)(buf+8);
    uint64_t mm_off=*(uint64_t*)(buf+104);
    uint32_t mm_sz=*(uint32_t*)(buf+112);
    printf("magic=0x%08x ver=%u flags=0x%04x\n",magic,ver,flags);
    printf("model_meta_off=%llu sz=%u HAS_MMETA=%d\n",(unsigned long long)mm_off,mm_sz,(flags&0x2)!=0);
    return 0;
}
