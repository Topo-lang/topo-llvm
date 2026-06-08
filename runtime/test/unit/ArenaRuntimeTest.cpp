#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "topo/rt/arena_rt.h"

TEST(ArenaRuntimeTest, CreateAndDestroy) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);
    EXPECT_GE(topo_arena_capacity(arena), 4096u);
    EXPECT_EQ(topo_arena_bytes_used(arena), 0u);
    topo_arena_destroy(arena);
}

TEST(ArenaRuntimeTest, AllocAndUse) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);

    auto* ptr = static_cast<int*>(topo_arena_alloc(arena, sizeof(int), alignof(int)));
    ASSERT_NE(ptr, nullptr);
    *ptr = 42;
    EXPECT_EQ(*ptr, 42);
    EXPECT_GT(topo_arena_bytes_used(arena), 0u);

    topo_arena_destroy(arena);
}

TEST(ArenaRuntimeTest, MultipleAllocations) {
    auto* arena = topo_arena_create(256);
    ASSERT_NE(arena, nullptr);

    for (int i = 0; i < 100; ++i) {
        auto* p = static_cast<int*>(topo_arena_alloc(arena, sizeof(int), alignof(int)));
        ASSERT_NE(p, nullptr);
        *p = i;
    }

    topo_arena_destroy(arena);
}

TEST(ArenaRuntimeTest, ResetFreesUsage) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);

    topo_arena_alloc(arena, 1024, 8);
    EXPECT_GT(topo_arena_bytes_used(arena), 0u);

    topo_arena_reset(arena);
    EXPECT_EQ(topo_arena_bytes_used(arena), 0u);

    topo_arena_destroy(arena);
}

TEST(ArenaRuntimeTest, AlignmentRespected) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);

    // Allocate with 64-byte alignment
    auto* ptr = topo_arena_alloc(arena, 128, 64);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 64, 0u);

    topo_arena_destroy(arena);
}

TEST(ArenaRuntimeTest, NullArenaHandled) {
    EXPECT_EQ(topo_arena_alloc(nullptr, 10, 8), nullptr);
    EXPECT_EQ(topo_arena_bytes_used(nullptr), 0u);
    EXPECT_EQ(topo_arena_capacity(nullptr), 0u);
    // Should not crash
    topo_arena_reset(nullptr);
    topo_arena_destroy(nullptr);
}

TEST(ArenaRuntimeTest, ZeroSizeReturnsNull) {
    auto* arena = topo_arena_create(4096);
    EXPECT_EQ(topo_arena_alloc(arena, 0, 8), nullptr);
    topo_arena_destroy(arena);
}

// ---------------------------------------------------------------------------
// Edge-case tests (stress + alignment + reset + nesting + thread-safety
// contract). All tests must pass cleanly under ASan and TSan.
// ---------------------------------------------------------------------------

// Zero-size returns null, but the arena remains usable for subsequent
// allocations — no silent corruption of internal state.
TEST(ArenaRuntimeTest, ZeroSizeDoesNotBreakArena) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);

    EXPECT_EQ(topo_arena_alloc(arena, 0, 8), nullptr);
    EXPECT_EQ(topo_arena_alloc(arena, 0, 16), nullptr);
    EXPECT_EQ(topo_arena_bytes_used(arena), 0u);

    // Arena must still honor a real allocation afterwards.
    auto* p = static_cast<int*>(topo_arena_alloc(arena, sizeof(int), alignof(int)));
    ASSERT_NE(p, nullptr);
    *p = 0x5A5A5A5A;
    EXPECT_EQ(*p, 0x5A5A5A5A);

    topo_arena_destroy(arena);
}

// Request 1 MiB from an arena with a tiny initial chunk. The arena must
// grow a new chunk and the pointer must be writable end-to-end.
TEST(ArenaRuntimeTest, HugeAllocationGrowsChunk) {
    auto* arena = topo_arena_create(128);
    ASSERT_NE(arena, nullptr);

    constexpr size_t kHuge = 1u << 20; // 1 MiB
    auto* ptr = static_cast<uint8_t*>(topo_arena_alloc(arena, kHuge, 16));
    ASSERT_NE(ptr, nullptr);

    // Write every byte and read back — exercises the full span.
    std::memset(ptr, 0xAB, kHuge);
    for (size_t i = 0; i < kHuge; i += 4096) {
        ASSERT_EQ(ptr[i], 0xAB) << "at offset " << i;
    }
    ASSERT_EQ(ptr[kHuge - 1], 0xAB);

    EXPECT_GE(topo_arena_capacity(arena), kHuge);
    topo_arena_destroy(arena);
}

// For alignments 1, 2, 4, 8, 16, 32, 64, 128 the returned pointer must be
// aligned. Catches a missing align_up in the bump fast path.
TEST(ArenaRuntimeTest, AlignmentStress) {
    auto* arena = topo_arena_create(16 * 1024);
    ASSERT_NE(arena, nullptr);

    const size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128};
    for (size_t a : alignments) {
        // Allocate something tiny first to push the cursor off a natural
        // boundary, then perform the aligned allocation.
        auto* pad = topo_arena_alloc(arena, 1, 1);
        ASSERT_NE(pad, nullptr);

        auto* ptr = topo_arena_alloc(arena, 17, a);
        ASSERT_NE(ptr, nullptr) << "alignment=" << a;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % a, 0u)
            << "alignment=" << a << " ptr=" << ptr;

        // Touch the whole block so ASan would flag any out-of-chunk write.
        std::memset(ptr, static_cast<int>(a & 0xFF), 17);
    }

    topo_arena_destroy(arena);
}

// Fill the first chunk almost completely, then request an aligned
// allocation that must come from a fresh chunk. The new chunk must still
// honor the requested alignment.
TEST(ArenaRuntimeTest, AlignmentAcrossChunkBoundary) {
    constexpr size_t kInitial = 256;
    auto* arena = topo_arena_create(kInitial);
    ASSERT_NE(arena, nullptr);

    // Consume most of the first chunk with a 1-byte-aligned padding
    // allocation so the cursor lands in an awkward spot.
    auto* fill = topo_arena_alloc(arena, kInitial - 3, 1);
    ASSERT_NE(fill, nullptr);

    // Now request 64-byte alignment with a size that almost certainly
    // forces a new chunk.
    constexpr size_t kAlign = 64;
    auto* aligned = topo_arena_alloc(arena, 200, kAlign);
    ASSERT_NE(aligned, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned) % kAlign, 0u);

    // Subsequent smaller alloc should still be fine — sanity that chunk
    // bookkeeping survived the growth.
    auto* tail = topo_arena_alloc(arena, 8, 8);
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(tail) % 8, 0u);

    topo_arena_destroy(arena);
}

// Two arenas live simultaneously — destroying one must not affect the
// other. Guards against any accidental global state.
TEST(ArenaRuntimeTest, NestedArenasAreIndependent) {
    auto* a = topo_arena_create(1024);
    ASSERT_NE(a, nullptr);

    auto* pa = static_cast<uint32_t*>(topo_arena_alloc(a, sizeof(uint32_t) * 32, alignof(uint32_t)));
    ASSERT_NE(pa, nullptr);
    for (int i = 0; i < 32; ++i) pa[i] = 0xA0000000u | static_cast<uint32_t>(i);

    auto* b = topo_arena_create(1024);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);

    auto* pb = static_cast<uint32_t*>(topo_arena_alloc(b, sizeof(uint32_t) * 32, alignof(uint32_t)));
    ASSERT_NE(pb, nullptr);
    for (int i = 0; i < 32; ++i) pb[i] = 0xB0000000u | static_cast<uint32_t>(i);

    // Verify arena a's data is untouched while b is live.
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(pa[i], 0xA0000000u | static_cast<uint32_t>(i));
    }

    // Destroy b, keep using a.
    topo_arena_destroy(b);

    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(pa[i], 0xA0000000u | static_cast<uint32_t>(i));
    }
    auto* pa2 = static_cast<uint64_t*>(topo_arena_alloc(a, sizeof(uint64_t), alignof(uint64_t)));
    ASSERT_NE(pa2, nullptr);
    *pa2 = 0xDEADBEEFCAFEBABEull;
    EXPECT_EQ(*pa2, 0xDEADBEEFCAFEBABEull);

    topo_arena_destroy(a);
}

// Allocate, write, reset, re-allocate the same size. The second allocation
// should succeed and the byte pattern from before must not survive in the
// caller's view (i.e. the test reinitializes and reads its own value —
// this ensures bytes_used is truly reset and the cursor rewound).
TEST(ArenaRuntimeTest, ResetReusesBufferCorrectly) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);

    constexpr size_t kSize = 512;
    auto* first = static_cast<uint8_t*>(topo_arena_alloc(arena, kSize, 16));
    ASSERT_NE(first, nullptr);
    std::memset(first, 0x11, kSize);
    EXPECT_GE(topo_arena_bytes_used(arena), kSize);

    topo_arena_reset(arena);
    EXPECT_EQ(topo_arena_bytes_used(arena), 0u);

    auto* second = static_cast<uint8_t*>(topo_arena_alloc(arena, kSize, 16));
    ASSERT_NE(second, nullptr);
    // After reset, caller must reinitialize — we verify they CAN by
    // overwriting and reading back a distinct pattern.
    std::memset(second, 0x22, kSize);
    for (size_t i = 0; i < kSize; ++i) {
        ASSERT_EQ(second[i], 0x22) << "at offset " << i;
    }
    EXPECT_GE(topo_arena_bytes_used(arena), kSize);

    topo_arena_destroy(arena);
}

// The arena does NOT document thread-safety (no internal synchronization
// in topo_arena::alloc). This test stress-exercises the single-threaded
// allocator with many small allocations to surface any counter overflow
// or chunk-leak regression. Thread safety is intentionally NOT a contract.
TEST(ArenaRuntimeTest, SingleThreadStress10000Allocations) {
    auto* arena = topo_arena_create(256);
    ASSERT_NE(arena, nullptr);

    constexpr int kCount = 10000;
    std::vector<uint64_t*> ptrs;
    ptrs.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        auto* p = static_cast<uint64_t*>(topo_arena_alloc(arena, sizeof(uint64_t), alignof(uint64_t)));
        ASSERT_NE(p, nullptr) << "iteration " << i;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(uint64_t), 0u);
        *p = static_cast<uint64_t>(i);
        ptrs.push_back(p);
    }

    // Verify every pointer is still valid and holds the expected value —
    // catches any chunk-list invalidation on growth.
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(*ptrs[i], static_cast<uint64_t>(i)) << "slot " << i;
    }
    EXPECT_GE(topo_arena_bytes_used(arena), sizeof(uint64_t) * kCount);

    topo_arena_destroy(arena);
}

// OOM path — force addBlock() to fail by requesting a size so large that
// std::malloc cannot satisfy it. The alloc() implementation must return
// nullptr (not crash, not return a stale pointer, not silently succeed on
// a smaller-than-requested block). Covers topo_arena.cpp:52 conditional.
//
// SIZE_MAX / 2 is used rather than SIZE_MAX because some libc
// implementations eagerly reject SIZE_MAX at the API boundary with EINVAL
// before reaching the heap allocator, which would exercise a different
// path than the one we care about (std::malloc returning nullptr under
// address-space exhaustion).
TEST(ArenaRuntimeTest, HugeAllocationExhaustsMemoryReturnsNull) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);

    // Successful allocation first — arena is usable.
    auto* ok = topo_arena_alloc(arena, 64, 8);
    ASSERT_NE(ok, nullptr);

    // A request larger than any plausible address-space must fail cleanly.
    constexpr size_t kHuge = static_cast<size_t>(-1) / 2;
    auto* bad = topo_arena_alloc(arena, kHuge, 16);
    EXPECT_EQ(bad, nullptr) << "oversized alloc must return nullptr";

    // Arena must remain usable after OOM — the failed request must not
    // corrupt the block list or leave a partial block pushed.
    auto* after = topo_arena_alloc(arena, 64, 8);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after, ok);

    topo_arena_destroy(arena);
}

// Integer-overflow boundary — a size near SIZE_MAX must be rejected with
// nullptr, NOT silently accepted by a wrapped capacity check. Two wrap
// paths are exercised:
//   * fast path: `used + padding + size` wraps to a tiny value <= capacity,
//     handing back a pointer into the tiny initial block while the caller
//     believes it owns ~SIZE_MAX bytes (heap buffer overflow on first write);
//   * new-block path: `size + alignment` wraps to a tiny value, so addBlock
//     allocates only defaultBlockSize, then `used = padding + size` claims
//     far more usable space than exists.
// The overflow guard in topo_arena::alloc must turn both into a clean
// nullptr. The arena must remain usable afterwards (no partial/poisoned
// block pushed).
TEST(ArenaRuntimeTest, SizeNearMaxOverflowReturnsNull) {
    auto* arena = topo_arena_create(4096);
    ASSERT_NE(arena, nullptr);

    // Seed a successful allocation so `used > 0` and the fast-path sum
    // `used + padding + size` is the one that wraps.
    auto* seed = topo_arena_alloc(arena, 64, 8);
    ASSERT_NE(seed, nullptr);
    *static_cast<uint8_t*>(seed) = 0x7E;

    // SIZE_MAX with alignment 16: `size + alignment` wraps to 15, and
    // `used + padding + size` wraps below capacity — both old wrap paths.
    constexpr size_t kMax = static_cast<size_t>(-1);
    EXPECT_EQ(topo_arena_alloc(arena, kMax, 16), nullptr)
        << "SIZE_MAX alloc must be rejected, not wrapped";

    // SIZE_MAX - 8 with alignment 16: size + alignment = SIZE_MAX + 7 wraps.
    EXPECT_EQ(topo_arena_alloc(arena, kMax - 8, 16), nullptr)
        << "near-SIZE_MAX alloc must be rejected, not wrapped";

    // The exact boundary the guard rejects: size > SIZE_MAX - alignment.
    EXPECT_EQ(topo_arena_alloc(arena, kMax - 15, 16), nullptr)
        << "size == SIZE_MAX - alignment + 1 must be rejected";

    // Arena must still serve a legitimate allocation, and the seed byte
    // must be intact (no wrapped write clobbered the block).
    EXPECT_EQ(*static_cast<uint8_t*>(seed), 0x7E);
    auto* ok = topo_arena_alloc(arena, 128, 16);
    ASSERT_NE(ok, nullptr);
    std::memset(ok, 0x33, 128);
    EXPECT_EQ(*static_cast<uint8_t*>(seed), 0x7E);

    topo_arena_destroy(arena);
}

// Differential vs malloc — allocate the same size+alignment from both an
// arena and malloc, verify the two pointers behave identically for a
// write-then-read check (alignment, writability, byte-fidelity). Catches
// silent corruption or alignment drift in the arena's bump allocator.
TEST(ArenaRuntimeTest, DifferentialAgainstMalloc) {
    auto* arena = topo_arena_create(16 * 1024);
    ASSERT_NE(arena, nullptr);

    struct Record {
        uint64_t a;
        uint32_t b;
        uint16_t c;
        uint8_t d;
    };
    constexpr size_t kSize = sizeof(Record);
    constexpr size_t kAlign = alignof(Record);

    // Pairs of (arena, malloc) allocations, same inputs each time.
    for (int i = 0; i < 32; ++i) {
        auto* fromArena = static_cast<Record*>(topo_arena_alloc(arena, kSize, kAlign));
        auto* fromMalloc = static_cast<Record*>(std::malloc(kSize));
        ASSERT_NE(fromArena, nullptr) << "arena alloc failed at i=" << i;
        ASSERT_NE(fromMalloc, nullptr) << "malloc failed at i=" << i;

        // Alignment semantics must match.
        EXPECT_EQ(reinterpret_cast<uintptr_t>(fromArena) % kAlign, 0u);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(fromMalloc) % kAlign, 0u);

        // Both allocators hand back UNINITIALIZED memory, and `Record` carries
        // a trailing alignment-padding byte (uint64+uint32+uint16+uint8 = 15
        // payload bytes inside a 16-byte struct) that a struct assignment does
        // NOT write. Comparing the raw regions — or even comparing after only a
        // struct assignment — reads that residual padding byte, so the result
        // depends on prior heap/arena state and is order-dependent flaky.
        //
        // Make the comparison deterministic by writing a KNOWN pattern across
        // the full region of both before writing the payload. The differential
        // then tests behavior under writes (alignment, writability, byte
        // fidelity of the struct store) rather than freshly-allocated garbage.
        std::memset(fromArena, 0x5A, kSize);
        std::memset(fromMalloc, 0x5A, kSize);

        Record payload{0xFEEDFACE00000000ull + static_cast<uint64_t>(i),
                       static_cast<uint32_t>(0xA5A50000u | i),
                       static_cast<uint16_t>(0x1000 + i),
                       static_cast<uint8_t>(i & 0xFF)};
        *fromArena = payload;
        *fromMalloc = payload;

        EXPECT_EQ(std::memcmp(fromArena, fromMalloc, kSize), 0)
            << "arena and malloc produced different byte content at i=" << i;

        std::free(fromMalloc);
    }

    topo_arena_destroy(arena);
}

// Many small reset cycles — 1000 iterations of alloc -> reset -> alloc.
// Catches a chunk leak (capacity monotonically growing) or a counter
// overflow on the bytes_used accumulator.
TEST(ArenaRuntimeTest, ManyResetCycles) {
    auto* arena = topo_arena_create(1024);
    ASSERT_NE(arena, nullptr);

    size_t capacityAfterFirst = 0;
    for (int i = 0; i < 1000; ++i) {
        auto* p = static_cast<uint32_t*>(topo_arena_alloc(arena, sizeof(uint32_t) * 16, alignof(uint32_t)));
        ASSERT_NE(p, nullptr) << "iteration " << i;
        for (int j = 0; j < 16; ++j) p[j] = static_cast<uint32_t>(i * 16 + j);
        // Read back immediately so ASan can catch out-of-chunk writes.
        for (int j = 0; j < 16; ++j) {
            ASSERT_EQ(p[j], static_cast<uint32_t>(i * 16 + j));
        }

        topo_arena_reset(arena);
        EXPECT_EQ(topo_arena_bytes_used(arena), 0u);

        if (i == 0) {
            capacityAfterFirst = topo_arena_capacity(arena);
        } else {
            // After the first reset, capacity must not grow — reset keeps
            // one chunk and drops any extras. If capacity grows each
            // cycle we have a chunk leak.
            EXPECT_EQ(topo_arena_capacity(arena), capacityAfterFirst)
                << "chunk leak at iteration " << i;
        }
    }

    topo_arena_destroy(arena);
}
