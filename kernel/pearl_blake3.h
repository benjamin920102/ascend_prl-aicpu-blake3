/* Portable, allocation-free keyed BLAKE3 for one 64-byte transcript.
 * Compiled for the device CPU by BiSheng and for host-side correctness tests.
 */
#ifndef PEARL_BLAKE3_H
#define PEARL_BLAKE3_H

#include <stdint.h>
#include "../src/pearl_pipeline.h"

struct PearlBlake3Args {
    const uint32_t* transcript;  // GM layout: [band][16 words][64 columns].
    uint32_t* hits;              // Existing padded host result ABI.
    uint32_t key[8];             // Copied into the launch arguments, not shared UB.
    uint32_t target[8];
    int32_t nbands;
    int32_t n_hi;
    int32_t strip;
    int32_t doHash;
    int32_t hitWorkers;
};

static_assert(sizeof(void*) == 8, "AICPU launch arguments require a 64-bit ABI");
static_assert(sizeof(PearlBlake3Args) == 104, "unexpected AICPU launch argument ABI");

static inline uint32_t PearlRotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static inline void PearlBlake3Mix(uint32_t* v, int a, int b, int c, int d,
                                 uint32_t x, uint32_t y)
{
    v[a] += v[b] + x;
    v[d] = PearlRotr32(v[d] ^ v[a], 16);
    v[c] += v[d];
    v[b] = PearlRotr32(v[b] ^ v[c], 12);
    v[a] += v[b] + y;
    v[d] = PearlRotr32(v[d] ^ v[a], 8);
    v[c] += v[d];
    v[b] = PearlRotr32(v[b] ^ v[c], 7);
}

static inline void PearlBlake3Round(uint32_t* v, const uint32_t* m, const uint8_t* s)
{
    PearlBlake3Mix(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
    PearlBlake3Mix(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
    PearlBlake3Mix(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
    PearlBlake3Mix(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
    PearlBlake3Mix(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
    PearlBlake3Mix(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
    PearlBlake3Mix(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
    PearlBlake3Mix(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
}

static inline void PearlBlake3Keyed64(uint32_t out[8], const uint32_t input[16],
                                    const uint32_t key[8])
{
    static const uint8_t sigma[7][16] = {
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
        { 2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
        { 3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
        {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
        {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
        { 9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
        {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}
    };
    // Exactly one complete block: counter=0, block_len=64,
    // flags=KEYED_HASH | CHUNK_START | CHUNK_END | ROOT (27).
    uint32_t v[16] = {
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0, 0, 64, 27
    };
    PearlBlake3Round(v, input, sigma[0]);
    PearlBlake3Round(v, input, sigma[1]);
    PearlBlake3Round(v, input, sigma[2]);
    PearlBlake3Round(v, input, sigma[3]);
    PearlBlake3Round(v, input, sigma[4]);
    PearlBlake3Round(v, input, sigma[5]);
    PearlBlake3Round(v, input, sigma[6]);
    for (int i = 0; i < 8; ++i) out[i] = v[i] ^ v[i + 8];
}

static inline bool PearlLe256(const uint32_t hash[8], const uint32_t target[8])
{
    // Both arrays contain little-endian limbs; compare the most significant first.
    for (int i = 7; i >= 0; --i) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

static inline uint32_t PearlBlake3Scan(const PearlBlake3Args& args)
{
    if (!args.hits || args.hitWorkers <= 0) return 1;
    // Preserve pearl_reduce_hits()'s ABI and clear stale hits on EVERY slot reuse.
    // Only record 0 publishes a hit; padding stays untouched.
    for (int i = 0; i < args.hitWorkers; ++i) {
        args.hits[i * PEARL_HIT_STRIDE_WORDS] = 0;
        args.hits[i * PEARL_HIT_STRIDE_WORDS + 1] = 0;
    }
    if (!args.doHash) return 0;  // Existing PRL_NOFOLD diagnostic: never submit a hit.
    if (!args.transcript || args.nbands <= 0 || args.n_hi < 1 || args.n_hi > 64 ||
        args.strip < 0) return 1;
    const uint64_t tiles = (uint64_t)args.nbands * (uint32_t)args.n_hi;
    if ((uint64_t)args.strip * tiles + tiles - 1 > INT32_MAX) return 1;

    for (int band = 0; band < args.nbands; ++band) {
        const uint32_t* tr = args.transcript + (uint64_t)band * 16 * 64;
        for (int hi = 0; hi < args.n_hi; ++hi) {
            uint32_t input[16], hash[8];
            for (int w = 0; w < 16; ++w) input[w] = tr[w * 64 + hi];
            PearlBlake3Keyed64(hash, input, args.key);
            if (PearlLe256(hash, args.target)) {
                // Same [strip][hi][band] encoding used by miner.c/proof verification.
                args.hits[0] = (uint32_t)((uint64_t)args.strip * tiles +
                                        (uint64_t)hi * args.nbands + band);
                args.hits[1] = 1;
                return 0;  // A single valid tile is sufficient.
            }
        }
    }
    return 0;
}
#endif
