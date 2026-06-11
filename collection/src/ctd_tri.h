#ifndef CTD_TRI_H
#define CTD_TRI_H

#include <stdint.h>

typedef enum {
    CTD_CHIRAL_L = 0,
    CTD_CHIRAL_R = 1
} CTDChirality;

typedef struct {
    uint8_t dodeca_face;
    uint8_t chirality;
} CTDCard;

#endif /* CTD_TRI_H */
