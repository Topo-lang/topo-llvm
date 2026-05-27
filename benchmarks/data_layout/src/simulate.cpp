// Node functions — natural C++ struct field access.
// The user writes particles[i].vx, not separate float arrays.
// DataLayoutPass transforms the pipeline body to SoA layout automatically.

#include "particle.h"

namespace physics {

void apply_forces(ParticleArray& particles, float dt) {
    float drag = 1.0f - 0.001f * dt;
    float gravity = -9.81f * dt;
    for (int i = 0; i < COUNT; ++i) {
        particles[i].vx *= drag;
        particles[i].vy *= drag;
        particles[i].vz *= drag;
        particles[i].vy += gravity;
    }
}

void integrate(ParticleArray& particles, float dt) {
    for (int i = 0; i < COUNT; ++i) {
        particles[i].x += particles[i].vx * dt;
        particles[i].y += particles[i].vy * dt;
        particles[i].z += particles[i].vz * dt;
    }
}

void boundary_check(ParticleArray& particles) {
    constexpr float BOUND = 1000.0f;
    for (int i = 0; i < COUNT; ++i) {
        if (particles[i].x < -BOUND || particles[i].x > BOUND) particles[i].vx = -particles[i].vx;
        if (particles[i].y < -BOUND || particles[i].y > BOUND) particles[i].vy = -particles[i].vy;
        if (particles[i].z < -BOUND || particles[i].z > BOUND) particles[i].vz = -particles[i].vz;
    }
}

} // namespace physics
