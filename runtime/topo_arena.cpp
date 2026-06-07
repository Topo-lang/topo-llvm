#include "topo/rt/arena_rt.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

struct topo_arena {
    struct Block {
        uint8_t* data;
        size_t capacity;
        size_t used;
    };

    std::vector<Block> blocks;
    size_t defaultBlockSize;

    explicit topo_arena(size_t initialCapacity) : defaultBlockSize(initialCapacity) { addBlock(initialCapacity); }

    ~topo_arena() {
        for (auto& b : blocks) {
            std::free(b.data);
        }
    }

    // Returns true on success, false if malloc failed. A failed addBlock
    // does not push anything onto blocks, so callers must check the
    // return value rather than inspecting blocks.back().
    bool addBlock(size_t minSize) {
        size_t cap = (minSize > defaultBlockSize) ? minSize : defaultBlockSize;
        auto* ptr = static_cast<uint8_t*>(std::malloc(cap));
        if (!ptr) return false;
        blocks.push_back({ptr, cap, 0});
        return true;
    }

    void* alloc(size_t size, size_t alignment) {
        if (blocks.empty()) return nullptr;

        // Overflow guard: `size` is caller-influenced (e.g. LifetimeArenaPass
        // lowers calloc(count, elemSize) to an unchecked count*elemSize). If
        // padding + size or size + alignment wraps size_t, the capacity checks
        // below pass spuriously and we hand back a pointer claiming far more
        // usable space than the block actually has — a heap buffer overflow on
        // first write. Reject any request that cannot be expressed without
        // wrapping. `alignment` is small (a power of two), so this only fails
        // for genuinely absurd (overflowed) sizes; a legitimate large alloc
        // still falls through to the (failing) malloc and returns nullptr.
        if (size > SIZE_MAX - alignment) return nullptr;

        // Try current block
        Block& current = blocks.back();
        uintptr_t addr = reinterpret_cast<uintptr_t>(current.data + current.used);
        uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
        size_t padding = aligned - addr;

        // Non-wrapping fast-path test. `padding <= alignment - 1` and
        // `size <= SIZE_MAX - alignment`, so `padding + size` cannot wrap.
        // Compare against remaining room computed by subtraction (used is
        // always <= capacity, so capacity - used is non-negative).
        if (padding + size <= current.capacity - current.used) {
            current.used += padding + size;
            return reinterpret_cast<void*>(aligned);
        }

        // Allocate a new block. `size + alignment` is wrap-safe given the
        // guard above. addBlock rounds up to defaultBlockSize, so the new
        // block always has room for `padding + size` (padding < alignment),
        // but assert that invariant defensively before claiming the bytes.
        if (!addBlock(size + alignment)) return nullptr;

        Block& newBlock = blocks.back();
        addr = reinterpret_cast<uintptr_t>(newBlock.data);
        aligned = (addr + alignment - 1) & ~(alignment - 1);
        padding = aligned - addr;
        if (padding + size > newBlock.capacity) {
            // Should be unreachable (cap >= size + alignment > padding + size),
            // but never hand back a pointer the block cannot back.
            return nullptr;
        }
        newBlock.used = padding + size;
        return reinterpret_cast<void*>(aligned);
    }

    void reset() {
        // Keep only the first block, reset all usage
        if (blocks.size() > 1) {
            for (size_t i = 1; i < blocks.size(); ++i) {
                std::free(blocks[i].data);
            }
            blocks.resize(1);
        }
        if (!blocks.empty()) {
            blocks[0].used = 0;
        }
    }

    size_t bytesUsed() const {
        size_t total = 0;
        for (const auto& b : blocks)
            total += b.used;
        return total;
    }

    size_t totalCapacity() const {
        size_t total = 0;
        for (const auto& b : blocks)
            total += b.capacity;
        return total;
    }
};

extern "C" {

uint32_t topo_arena_version(void) {
    return TOPO_ARENA_ABI_VERSION;
}

topo_arena_t topo_arena_create(size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = 4096;
    try {
        return new topo_arena(initial_capacity);
    } catch (...) {
        return nullptr;
    }
}

void* topo_arena_alloc(topo_arena_t arena, size_t size, size_t alignment) {
    if (!arena || size == 0) return nullptr;
    if (alignment == 0) alignment = alignof(std::max_align_t);
    return arena->alloc(size, alignment);
}

void topo_arena_reset(topo_arena_t arena) {
    if (arena) arena->reset();
}

void topo_arena_destroy(topo_arena_t arena) {
    delete arena;
}

size_t topo_arena_bytes_used(topo_arena_t arena) {
    return arena ? arena->bytesUsed() : 0;
}

size_t topo_arena_capacity(topo_arena_t arena) {
    return arena ? arena->totalCapacity() : 0;
}

} // extern "C"
