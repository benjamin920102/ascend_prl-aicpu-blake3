// Host-only correctness checks. No CANN headers or device are needed.
#include "../kernel/pearl_blake3.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static uint32_t LoadLE(const unsigned char* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static void CheckOfficialVector()
{
    // BLAKE3-team/BLAKE3 test_vectors/test_vectors.json, input_len=64.
    // Input bytes repeat 0..250; key is exactly these 32 ASCII bytes.
    const char keyBytes[] = "whats the Elvish word for friend";
    const char expected[] = "ba8ced36f327700d213f120b1a207a3b"
                            "8c04330528586f414d09f2f7d9ccb7e6";
    unsigned char bytes[64];
    for (int i = 0; i < 64; ++i) bytes[i] = (unsigned char)i;
    uint32_t key[8], input[16], out[8];
    for (int i = 0; i < 8; ++i)
        key[i] = LoadLE(reinterpret_cast<const unsigned char*>(keyBytes) + 4 * i);
    for (int i = 0; i < 16; ++i) input[i] = LoadLE(bytes + 4 * i);
    PearlBlake3Keyed64(out, input, key);
    char hex[65];
    for (int i = 0; i < 32; ++i)
        std::snprintf(hex + 2 * i, 3, "%02x", (out[i / 4] >> (8 * (i % 4))) & 255);
    assert(std::strcmp(hex, expected) == 0);
}

static void CheckComparator()
{
    uint32_t a[8] = {}, b[8] = {};
    assert(PearlLe256(a, b));  // Target equality is a hit.
    a[0] = UINT32_MAX;
    b[7] = 1;
    assert(PearlLe256(a, b));  // High limbs must outweigh low limbs.
    assert(!PearlLe256(b, a));
    a[7] = b[7];
    assert(!PearlLe256(a, b));
}

static void CheckLayoutAndReuse(int nbands, int n_hi, int strip)
{
    constexpr int workers = 40;
    constexpr uint32_t padding = 0x5a5a5a5au;
    std::vector<uint32_t> transcript((size_t)nbands * 16 * 64);
    std::vector<uint32_t> hits(PEARL_HIT_WORDS(workers / PEARL_GRID), padding);
    uint32_t rng = 0x74a1b392u + (uint32_t)strip;
    auto next = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (auto& word : transcript) word = next();
    PearlBlake3Args args = {};
    args.transcript = transcript.data();
    args.hits = hits.data();
    args.nbands = nbands;
    args.n_hi = n_hi;
    args.strip = strip;
    args.doHash = 1;
    args.hitWorkers = workers;
    for (auto& k : args.key) k = next();
    std::fill(args.target, args.target + 8, UINT32_MAX);

    // Find the unique minimum active tile. Using it as the inclusive target
    // checks strided transcript gathering, n_hi bounds and encoded hit indices.
    int minBand = -1, minHi = -1;
    for (int band = 0; band < nbands; ++band) {
        for (int hi = 0; hi < n_hi; ++hi) {
            uint32_t input[16], hash[8];
            for (int w = 0; w < 16; ++w)
                input[w] = transcript[((size_t)band * 16 + w) * 64 + hi];
            PearlBlake3Keyed64(hash, input, args.key);
            if (PearlLe256(hash, args.target)) {
                std::copy(hash, hash + 8, args.target);
                minBand = band;
                minHi = hi;
            }
        }
    }
    for (int w = 0; w < workers; ++w) hits[w * PEARL_HIT_STRIDE_WORDS + 1] = 1;
    assert(PearlBlake3Scan(args) == 0);
    uint32_t encoded = UINT32_MAX;
    assert(pearl_reduce_hits(hits.data(), workers, &encoded) == 1);
    const uint32_t expected = (uint32_t)(strip * (nbands * n_hi) + minHi * nbands + minBand);
    assert(encoded == expected);
    for (int w = 0; w < workers; ++w) {
        if (w) assert(hits[w * PEARL_HIT_STRIDE_WORDS + 1] == 0);
        for (int p = 2; p < PEARL_HIT_STRIDE_WORDS; ++p)
            assert(hits[w * PEARL_HIT_STRIDE_WORDS + p] == padding);
    }

    // Reuse exactly the same result storage with target=min-1: now no tile
    // passes, and no previous worker's hit may survive.
    for (int i = 0; i < 8; ++i) {
        if (args.target[i]-- != 0) break;
        assert(i != 7);
    }
    assert(PearlBlake3Scan(args) == 0);
    assert(pearl_reduce_hits(hits.data(), workers, &encoded) == 0);

    // PRL_NOFOLD skips hashing and still clears all prior hit flags.
    hits[1] = 1;
    args.doHash = 0;
    args.transcript = nullptr;
    assert(PearlBlake3Scan(args) == 0);
    assert(pearl_reduce_hits(hits.data(), workers, &encoded) == 0);

    args.doHash = 1;
    args.transcript = transcript.data();
    args.n_hi = 65;
    assert(PearlBlake3Scan(args) != 0);
    args.n_hi = n_hi;
    args.strip = INT32_MAX;
    assert(PearlBlake3Scan(args) != 0);
}

int main()
{
    CheckOfficialVector();
    CheckComparator();
    // Reuse hit storage with 96 different strip indices/inputs/keys.
    // Small/uneven band counts also cover strips with idle Vector lanes.
    for (int strip = 0; strip < 96; ++strip) {
        CheckLayoutAndReuse(7, 1, strip);
        CheckLayoutAndReuse(23, 4, strip);
        CheckLayoutAndReuse(3, 64, strip);
    }
    std::puts("PASS: official keyed BLAKE3 vector, target comparison, transcript layout, hit encoding, stale-hit clearing, diagnostic and bounds checks");
}
