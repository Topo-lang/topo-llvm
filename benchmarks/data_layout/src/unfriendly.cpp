// Unfriendly workload: random-access traversal reading ALL 16 fields.
// SoA transformation provides no benefit here — random stride dominates
// cache behavior, and touching all fields means no column pruning win.

#include "particle.h"

// Consume all fields to prevent dead-code elimination.
static volatile float g_sink = 0;

void unfriendly_work(ParticleArray& particles, const int* indices) {
    float accum = 0.0f;
    for (int k = 0; k < COUNT; ++k) {
        int i = indices[k];
        Particle& p = particles[i];
        // Touch every field — defeats column pruning
        accum += p.x + p.y + p.z;
        accum += p.vx + p.vy + p.vz;
        accum += p.mass + p.charge + p.radius;
        accum += static_cast<float>(p.color) + static_cast<float>(p.flags);
        accum += p.lifetime + p.energy + p.temperature;
        accum += static_cast<float>(p.group_id) + static_cast<float>(p.collision_count);
    }
    g_sink = accum;
}
