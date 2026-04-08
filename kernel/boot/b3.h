/*
 * Zeos — B3 (Brad's Bayesian Balance)
 *
 * The decision engine for Z-OS. Every chain carries a b3_state_t.
 *
 * Three layers, stacked:
 *
 *   Layer 1 — Brad's Balance (BB):
 *     O = P × f(C,I,V)
 *     where f(C,I,V) = (C · I · V) / 1000
 *     P = population, C = cultural, I = individual, V = values
 *     O = oppositional response count (static prediction)
 *
 *   Layer 2 — Fred's Framework (Delta Root):
 *     DR = average of O across datasets
 *     Collapses multiple BB calculations into one prior.
 *
 *   Layer 3 — Bayesian Update:
 *     f ~ Beta(α, β)
 *     Start with a prior (uniform or derived from DR).
 *     Each observation updates: α += success, β += (1 - success).
 *     Prediction = E[f|data] = α / (α + β).
 *     Full B3 output: P × E[f|data].
 *
 * No stdlib. No malloc. Stack/static allocation only.
 * Pure float arithmetic — no libm required.
 */

#ifndef ZEOS_B3_H
#define ZEOS_B3_H

/* ── State ──────────────────────────────────────── */

/*
 * Bayesian state for one chain.
 *
 * alpha, beta:     Beta distribution parameters.
 *                  Prior: Beta(1,1) = uniform (no opinion).
 *                  After n observations: Beta(α₀+s, β₀+n-s).
 *
 * observations:    Total evidence count (n).
 * successes:       How many times the oppositional response was observed.
 *
 * prior_dr:        Delta Root from Fred's Framework (Layer 2).
 *                  Used to seed the prior when we have historical data.
 *
 * confidence_k:    Prior strength. Higher k means the prior resists
 *                  more evidence before shifting. Grows with observations
 *                  in practice — early evidence matters most.
 */
typedef struct {
    float   alpha;
    float   beta;
    int     observations;
    int     successes;
    float   prior_dr;
    float   confidence_k;
} b3_state_t;

/* ── API ────────────────────────────────────────── */

/*
 * Initialize with uniform prior Beta(1,1).
 * No opinion — 50/50 until evidence arrives.
 */
void b3_init(b3_state_t *state);

/*
 * Initialize from a Delta Root value.
 *
 * delta_root: expected oppositional fraction (0.0 to 1.0).
 *             e.g., DR = 0.3 means we expect 30% oppositional.
 *
 * k:          prior strength (pseudo-observations).
 *             k = 10 means the prior is worth 10 observations.
 *             Higher k = more stubborn prior.
 *
 * Sets alpha = DR × k, beta = (1 - DR) × k.
 * So the prior mean = DR, and it takes ~k real observations
 * to overwhelm it.
 */
void b3_init_from_delta_root(b3_state_t *state, float delta_root, float k);

/*
 * Observe one data point.
 *
 * success = 1: oppositional response was observed.
 * success = 0: oppositional response was NOT observed.
 *
 * Bayesian update:
 *   α += success
 *   β += (1 - success)
 *   n += 1
 */
void b3_observe(b3_state_t *state, int success);

/*
 * Predict the oppositional fraction.
 *
 * Returns E[f|data] = α / (α + β).
 * This is the posterior mean of the Beta distribution.
 * Range: 0.0 to 1.0.
 */
float b3_predict(b3_state_t *state);

/*
 * Variance of the Beta distribution.
 *
 * Var = (α × β) / ((α + β)² × (α + β + 1))
 *
 * High variance = uncertain. Low variance = confident.
 * Shrinks as observations accumulate.
 */
float b3_variance(b3_state_t *state);

/*
 * Confidence score: 1.0 - variance.
 *
 * Not a formal statistical measure — a convenience.
 * Approaches 1.0 as evidence accumulates.
 * Starts near 0.75 with uniform prior (variance ~0.25 at n=0).
 */
float b3_confidence(b3_state_t *state);

/*
 * Layer 1 — Brad's Balance, static formula.
 *
 * f(C, I, V) = (C × I × V) / 1000
 *
 * C: cultural factor
 * I: individual factor
 * V: values factor
 *
 * Returns the oppositional fraction for one scenario.
 * Multiply by population P to get oppositional count O.
 */
float b3_bb_static(float C, float I, float V);

/*
 * Full B3 output.
 *
 * Combines population with Bayesian posterior:
 *   result = P × E[f|data]
 *          = population × (α / (α + β))
 *
 * This is the Bayesian-corrected oppositional count.
 * Starts at the prior, converges to observed reality.
 */
float b3_full(b3_state_t *state, int population);

/*
 * Reset to uniform prior Beta(1,1).
 * Clears all observations. Forgets everything.
 */
void b3_reset(b3_state_t *state);

/*
 * Merge two independent evidence streams.
 *
 * Beta distributions combine by adding parameters:
 *   dst.α = a.α + b.α - 1   (subtract 1 to remove double-counted prior)
 *   dst.β = a.β + b.β - 1
 *
 * The -1 corrects for both streams starting from Beta(1,1).
 * If either has α or β < 1 after subtraction, we clamp to 1.
 */
void b3_merge(b3_state_t *dst, b3_state_t *a, b3_state_t *b);

#endif /* ZEOS_B3_H */
