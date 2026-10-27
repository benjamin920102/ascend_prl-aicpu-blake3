/* Vector's message-dependent BLAKE3. Also compiled natively by correctness tests.
 * PEARL_B3_VECTOR_SOURCE is an internal address-space annotation, not a build flag.
 */
#ifndef PEARL_BLAKE3_FINISH_H
#define PEARL_BLAKE3_FINISH_H
#include <stdint.h>
#if defined(PEARL_B3_VECTOR_SOURCE)
#define PEARL_B3_INLINE __aicore__ inline
#else
#define PEARL_B3_INLINE static inline
#endif

constexpr int32_t PEARL_B3_SIGMA[7][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    { 2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    { 3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    { 9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}
};

PEARL_B3_INLINE uint32_t PearlB3Rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

PEARL_B3_INLINE void PearlB3Mix(uint32_t* v, int a, int b, int c, int d,
                              uint32_t x, uint32_t y)
{
    v[a] = v[a] + v[b] + x;
    v[d] = PearlB3Rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = PearlB3Rotr(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y;
    v[d] = PearlB3Rotr(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];
    v[b] = PearlB3Rotr(v[b] ^ v[c], 7);
}

PEARL_B3_INLINE void PearlB3FirstMix(uint32_t* v, int a, int b, int c, int d,
                                   uint32_t firstAdd, uint32_t x, uint32_t y)
{
    // firstAdd is key[a] + key[b], precomputed once by AICPU.
    v[a] = firstAdd + x;
    v[d] = PearlB3Rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = PearlB3Rotr(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y;
    v[d] = PearlB3Rotr(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];
    v[b] = PearlB3Rotr(v[b] ^ v[c], 7);
}

PEARL_B3_INLINE void PearlBlake3Finish64(uint32_t out[8], const uint32_t block[16],
                                      const uint32_t initial[16], const uint32_t firstAdd[4])
{
    uint32_t v[16];
#if defined(PEARL_B3_VECTOR_SOURCE)
    #pragma unroll
#endif
    for (int i = 0; i < 16; ++i) v[i] = initial[i];
    // The four first columns are independent and start from the initial state.
    PearlB3FirstMix(v, 0, 4, 8, 12, firstAdd[0], block[0], block[1]);
    PearlB3FirstMix(v, 1, 5, 9, 13, firstAdd[1], block[2], block[3]);
    PearlB3FirstMix(v, 2, 6, 10, 14, firstAdd[2], block[4], block[5]);
    PearlB3FirstMix(v, 3, 7, 11, 15, firstAdd[3], block[6], block[7]);
    PearlB3Mix(v, 0, 5, 10, 15, block[8], block[9]);
    PearlB3Mix(v, 1, 6, 11, 12, block[10], block[11]);
    PearlB3Mix(v, 2, 7, 8, 13, block[12], block[13]);
    PearlB3Mix(v, 3, 4, 9, 14, block[14], block[15]);
#if defined(PEARL_B3_VECTOR_SOURCE)
    #pragma unroll
#endif
    for (int round = 1; round < 7; ++round) {
        PearlB3Mix(v, 0, 4, 8, 12, block[PEARL_B3_SIGMA[round][0]], block[PEARL_B3_SIGMA[round][1]]);
        PearlB3Mix(v, 1, 5, 9, 13, block[PEARL_B3_SIGMA[round][2]], block[PEARL_B3_SIGMA[round][3]]);
        PearlB3Mix(v, 2, 6, 10, 14, block[PEARL_B3_SIGMA[round][4]], block[PEARL_B3_SIGMA[round][5]]);
        PearlB3Mix(v, 3, 7, 11, 15, block[PEARL_B3_SIGMA[round][6]], block[PEARL_B3_SIGMA[round][7]]);
        PearlB3Mix(v, 0, 5, 10, 15, block[PEARL_B3_SIGMA[round][8]], block[PEARL_B3_SIGMA[round][9]]);
        PearlB3Mix(v, 1, 6, 11, 12, block[PEARL_B3_SIGMA[round][10]], block[PEARL_B3_SIGMA[round][11]]);
        PearlB3Mix(v, 2, 7, 8, 13, block[PEARL_B3_SIGMA[round][12]], block[PEARL_B3_SIGMA[round][13]]);
        PearlB3Mix(v, 3, 4, 9, 14, block[PEARL_B3_SIGMA[round][14]], block[PEARL_B3_SIGMA[round][15]]);
    }
    for (int i = 0; i < 8; ++i) out[i] = v[i] ^ v[i + 8];
}
#undef PEARL_B3_INLINE
#endif
