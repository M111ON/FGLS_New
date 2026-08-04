#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal test - just test the primitives */
#define FT_FRAME_CYCLE 1440u
#define FT_FRAME_STRIDE 37u

static inline uint16_t ft_enc_to_pipe(uint16_t enc) {
    return (uint16_t)((enc * 973u) % FT_FRAME_CYCLE);
}

static inline uint8_t ft_enc_to_tick(uint16_t enc) {
    return (uint8_t)(enc % 12u);
}

int main(void) {
    printf("Testing primitives...\n");
    for (int i = 0; i < 10; i++) {
        uint16_t pipe = ft_enc_to_pipe(i);
        uint8_t tick = ft_enc_to_tick(i);
        printf("enc=%d -> pipe=%d, tick=%d\n", i, pipe, tick);
    }
    printf("Done\n");
    return 0;
}
