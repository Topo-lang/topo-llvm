#pragma once

#include <topo/array.h>
#include <topo/pipeline.h>
#include <cstdint>

// Particle: 16 fields, 64 bytes (one cache line).
// Streaming traversal only touches 6 fields (24 bytes) per particle.
// AoS cache utilization: 24/64 = 37.5%.
// After DataLayoutPass SoA transformation: 100%.

struct Particle {
    float x, y, z;
    float vx, vy, vz;
    float mass;
    float charge;
    float radius;
    uint32_t color;
    uint32_t flags;
    float lifetime;
    float energy;
    float temperature;
    int32_t group_id;
    int32_t collision_count;
};

static constexpr int COUNT = 262144;

// The data container — topo::array, not raw pointer.
// DataLayoutPass recognizes this type and can transform AoS → SoA.
using ParticleArray = topo::array<Particle, COUNT>;

namespace physics {

// Pipeline entry
void simulate(ParticleArray& particles, float dt);

// Node functions — natural struct field access, zero SoA code
void apply_forces(ParticleArray& particles, float dt);
void integrate(ParticleArray& particles, float dt);
void boundary_check(ParticleArray& particles);

} // namespace physics

// Unfriendly workload: random-access traversal of all fields.
void unfriendly_work(ParticleArray& particles, const int* indices);
