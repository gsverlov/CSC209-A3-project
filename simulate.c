/*
 * simulate.c – Monte Carlo simulation engines.
 *
 * Each simulation uses random sampling to estimate a well-known
 * mathematical constant.  Workers call run_simulation() with the
 * parameters received from the parent over the task pipe.
 */

#include "montecarlo.h"

/*
 * Estimate π using the classic unit-circle method.
 *
 * Generate random points (x, y) in the unit square [0,1]×[0,1].
 * A point "hits" if x² + y² ≤ 1 (it falls inside the quarter
 * circle of radius 1).  The ratio hits/trials ≈ π/4.
 */
static double estimate_pi(uint32_t trials, uint32_t seed,
                           uint32_t *hits_out) {
    unsigned int rng = seed;
    uint32_t hits = 0;

    for (uint32_t i = 0; i < trials; i++) {
        double x = (double)rand_r(&rng) / RAND_MAX;
        double y = (double)rand_r(&rng) / RAND_MAX;
        if (x * x + y * y <= 1.0) {
            hits++;
        }
    }

    *hits_out = hits;
    return 4.0 * (double)hits / (double)trials;
}

/*
 * Estimate e using the random-sum method.
 *
 * For each trial, keep drawing uniform random numbers in (0,1)
 * and summing them until the sum exceeds 1.  The expected number
 * of draws needed is e ≈ 2.71828…
 *
 * Here "hits" is the total number of draws across all trials,
 * so estimate = hits / trials.
 */
static double estimate_e(uint32_t trials, uint32_t seed,
                          uint32_t *hits_out) {
    unsigned int rng = seed;
    uint64_t total_draws = 0;

    for (uint32_t i = 0; i < trials; i++) {
        double sum = 0.0;
        uint32_t draws = 0;
        while (sum < 1.0) {
            sum += (double)rand_r(&rng) / RAND_MAX;
            draws++;
        }
        total_draws += draws;
    }

    *hits_out = (uint32_t)(total_draws & 0xFFFFFFFF);
    return (double)total_draws / (double)trials;
}

/*
 * Estimate √2 using geometric probability.
 *
 * Generate random points (x, y) in [0,1]×[0,1].  Count the
 * fraction where x² + y² ≤ 2·x (equivalently (x−1)² + y² ≤ 1,
 * i.e. points inside a unit circle centered at (1,0)).  The area
 * of intersection of that circle with the unit square equals
 * π/4 − (√2 − 1 approximation area).
 *
 * Simpler approach: generate x in [0,2], y in [0,2].  The ratio
 * of points with x² < 2 gives P(x < √2) = √2/2, so
 * estimate = 2 * hits / trials.
 */
static double estimate_sqrt2(uint32_t trials, uint32_t seed,
                              uint32_t *hits_out) {
    unsigned int rng = seed;
    uint32_t hits = 0;

    for (uint32_t i = 0; i < trials; i++) {
        /* x uniform in [0, 2] */
        double x = 2.0 * (double)rand_r(&rng) / RAND_MAX;
        if (x * x < 2.0) {
            hits++;
        }
    }

    *hits_out = hits;
    return 2.0 * (double)hits / (double)trials;
}

/*
 * Dispatch to the appropriate simulation engine.
 */
double run_simulation(uint32_t sim_type, uint32_t trials,
                      uint32_t seed, uint32_t *hits_out) {
    switch (sim_type) {
        case SIM_PI:
            return estimate_pi(trials, seed, hits_out);
        case SIM_E:
            return estimate_e(trials, seed, hits_out);
        case SIM_SQRT2:
            return estimate_sqrt2(trials, seed, hits_out);
        default:
            fprintf(stderr, "Unknown simulation type: %u\n", sim_type);
            *hits_out = 0;
            return 0.0;
    }
}

/*
 * Return a human-readable name for each simulation type.
 */
const char *sim_type_name(uint32_t sim_type) {
    switch (sim_type) {
        case SIM_PI:    return "Estimate pi";
        case SIM_E:     return "Estimate e";
        case SIM_SQRT2: return "Estimate sqrt(2)";
        default:        return "Unknown";
    }
}

/*
 * Return the true mathematical value for comparison.
 */
double sim_true_value(uint32_t sim_type) {
    switch (sim_type) {
        case SIM_PI:    return M_PI;
        case SIM_E:     return M_E;
        case SIM_SQRT2: return sqrt(2.0);
        default:        return 0.0;
    }
}
