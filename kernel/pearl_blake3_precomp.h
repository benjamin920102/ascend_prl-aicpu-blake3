/* ABI shared by host, AICPU and AIV. Only message-independent work lives here. */
#ifndef PEARL_BLAKE3_PRECOMP_H
#define PEARL_BLAKE3_PRECOMP_H
#include <stdint.h>

enum {
    PEARL_B3_STATE = 0,       // [0,16): initial compression state.
    PEARL_B3_FIRST_ADD = 16,  // [16,20): key[i] + key[i+4] modulo 2^32.
    PEARL_B3_TARGET = 20,     // [20,28): little-endian target limbs.
    PEARL_B3_PREP_WORDS = 32  // [28,32): zero padding. Total GM size: 128 bytes.
};

struct PearlBlake3Prepared { uint32_t words[PEARL_B3_PREP_WORDS]; };
struct PearlBlake3PrepArgs {
    PearlBlake3Prepared* output;
    uint32_t key[8];
    uint32_t target[8];
};
static_assert(sizeof(PearlBlake3Prepared) == 128, "unexpected prepared-state ABI");
static_assert(sizeof(PearlBlake3PrepArgs) == 72, "AICPU launch requires 64-bit ABI");

static inline void PearlBlake3Prepare(PearlBlake3Prepared& output,
                                     const uint32_t key[8], const uint32_t target[8])
{
    uint32_t* p = output.words;
    for (int i = 0; i < PEARL_B3_PREP_WORDS; ++i) p[i] = 0;
    for (int i = 0; i < 8; ++i) p[PEARL_B3_STATE + i] = key[i];
    p[8] = 0x6A09E667u;
    p[9] = 0xBB67AE85u;
    p[10] = 0x3C6EF372u;
    p[11] = 0xA54FF53Au;
    // Single 64-byte keyed root block: counter=0, length=64, flags=27.
    p[14] = 64;
    p[15] = 1u | 2u | 8u | 16u;
    for (int i = 0; i < 4; ++i) p[PEARL_B3_FIRST_ADD + i] = key[i] + key[i + 4];
    for (int i = 0; i < 8; ++i) p[PEARL_B3_TARGET + i] = target[i];
}
#endif
