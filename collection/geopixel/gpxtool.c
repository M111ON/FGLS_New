#include "hb_vault.h"
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: gpx5_tool [pack/unpack] [target]\n");
        return 1;
    }

    if (strcmp(argv[1], "pack") == 0) {
        // บีบ folder "geopixel" เป็นไฟล์ .gpx5
        return hbv_pack_folder(argv[2], "output.gpx5");
    } 
    else if (strcmp(argv[1], "unpack") == 0) {
        // คลายไฟล์ .gpx5 ออกมา
        return hbv_unpack(argv[2], "restored_data", 1);
    }

    return 0;
}