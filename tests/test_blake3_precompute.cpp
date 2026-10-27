#include "../kernel/pearl_blake3_precomp.h"
#include "../kernel/pearl_blake3_finish.h"
#include "reference_blake3_original.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static void Finish(uint32_t out[8], const uint32_t input[16], const PearlBlake3Prepared& p)
{
    PearlBlake3Finish64(out, input, p.words + PEARL_B3_STATE,
                       p.words + PEARL_B3_FIRST_ADD);
}

static void OfficialVector()
{
    const char keyBytes[] = "whats the Elvish word for friend";
    const char expected[] = "ba8ced36f327700d213f120b1a207a3b"
                            "8c04330528586f414d09f2f7d9ccb7e6";
    static_assert(sizeof(keyBytes) == 33, "official key must be 32 bytes");
    static_assert(sizeof(expected) == 65, "official digest must be 32 bytes");
    uint32_t key[8] = {}, input[16] = {}, target[8] = {}, out[8];
    for (int b = 0; b < 32; ++b)
        key[b / 4] |= uint32_t(static_cast<unsigned char>(keyBytes[b])) << (8 * (b % 4));
    for (int b = 0; b < 64; ++b) input[b / 4] |= uint32_t(b) << (8 * (b % 4));
    PearlBlake3Prepared prepared;
    PearlBlake3Prepare(prepared, key, target);
    Finish(out, input, prepared);
    char actual[65];
    for (int b = 0; b < 32; ++b)
        std::snprintf(actual + b * 2, 3, "%02x", (out[b / 4] >> (8 * (b % 4))) & 255);
    assert(std::strcmp(actual, expected) == 0);
}

static void CompareWithOriginal(uint32_t key[8], uint32_t input[16], uint32_t target[8])
{
    PearlBlake3Prepared gm;
    std::memset(&gm, 0xa5, sizeof(gm));
    PearlBlake3Prepare(gm, key, target);
    for (int i = 0; i < 8; ++i) {
        assert(gm.words[i] == key[i]);
        assert(gm.words[PEARL_B3_TARGET + i] == target[i]);
    }
    for (int i = 0; i < 4; ++i)
        assert(gm.words[PEARL_B3_FIRST_ADD + i] == uint32_t(uint64_t(key[i]) + key[i + 4]));
    assert(gm.words[12] == 0 && gm.words[13] == 0);
    assert(gm.words[14] == 64 && gm.words[15] == 27);
    for (int i = 28; i < PEARL_B3_PREP_WORDS; ++i) assert(gm.words[i] == 0);

    // Model Vector's per-kernel scalar snapshot: several different transcripts
    // use the same prepared context, with no AICPU call per tile.
    PearlBlake3Prepared snapshot = gm;
    for (int tile = 0; tile < 4; ++tile) {
        uint32_t actual[8], expected[8];
        input[tile] ^= 0x91563badu;
        Finish(actual, input, snapshot);
        OriginalVectorHash::blake3_hash_keyed(expected, input, key, 64);
        assert(std::memcmp(actual, expected, sizeof(actual)) == 0);
    }

    // Updating target must not change hash state or first-column key sums.
    target[7] ^= 0xfedcba98u;
    PearlBlake3Prepare(gm, key, target);
    assert(std::memcmp(gm.words, snapshot.words, PEARL_B3_TARGET * sizeof(uint32_t)) == 0);
    assert(gm.words[PEARL_B3_TARGET + 7] == target[7]);
}

int main()
{
    OfficialVector();
    uint32_t key[8] = {}, input[16] = {}, target[8] = {};
    CompareWithOriginal(key, input, target);
    for (auto& k : key) k = UINT32_MAX;
    for (auto& x : input) x = UINT32_MAX;
    CompareWithOriginal(key, input, target);  // Addition wraparound is required.
    uint32_t rng = 0xa31964d5u;
    auto next = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (int scan = 0; scan < 4096; ++scan) {
        for (auto& k : key) k = next();
        for (auto& x : input) x = next();
        for (auto& t : target) t = next();
        CompareWithOriginal(key, input, target);
    }
    std::puts("PASS: official keyed BLAKE3 vector; 16,392 hashes match original Vector code; preprocessing ABI, overflow and target update checks");
}
