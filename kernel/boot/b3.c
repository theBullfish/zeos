/*
 * Zeos — B3 (Brad's Bayesian Balance)
 *
 * Implementation of the three-layer decision engine.
 * See b3.h for the full mathematical description.
 *
 * No stdlib. No libm. Pure float arithmetic.
 */

#include "b3.h"

/* ── Layer 3: Bayesian core ─────────────────────── */

void b3_init(b3_state_t *state)
{
    /*
     * Uniform prior: Beta(1, 1).
     * This is a flat distribution over [0, 1] — no opinion.
     * Mean = 0.5, variance = 1/12 ≈ 0.083.
     */
    state->alpha        = 1.0f;
    state->beta         = 1.0f;
    state->observations = 0;
    state->successes    = 0;
    state->prior_dr     = 0.5f;
    state->confidence_k = 2.0f;     /* 2 pseudo-observations (α + β) */
}

void b3_init_from_delta_root(b3_state_t *state, float delta_root, float k)
{
    /*
     * Seed the prior from Fred's Framework.
     *
     * If DR = 0.3 and k = 20:
     *   α = 0.3 × 20 = 6
     *   β = 0.7 × 20 = 14
     *   Mean = 6/20 = 0.3 (matches DR)
     *   Weight = 20 pseudo-observations
     *
     * Clamp DR to [0.01, 0.99] to avoid degenerate distributions.
     * Clamp k to minimum of 2 to ensure valid Beta parameters.
     */
    if (delta_root < 0.01f) delta_root = 0.01f;
    if (delta_root > 0.99f) delta_root = 0.99f;
    if (k < 2.0f) k = 2.0f;

    state->alpha        = delta_root * k;
    state->beta         = (1.0f - delta_root) * k;
    state->observations = 0;
    state->successes    = 0;
    state->prior_dr     = delta_root;
    state->confidence_k = k;
}

void b3_observe(b3_state_t *state, int success)
{
    /*
     * Bayesian conjugate update for Beta-Binomial model.
     *
     * Observed oppositional response (success=1):
     *   α += 1  (one more "success" in Beta terms)
     *
     * Observed non-oppositional response (success=0):
     *   β += 1  (one more "failure" in Beta terms)
     *
     * The posterior is still a Beta distribution.
     * This is exact — no approximation.
     */
    if (success) {
        state->alpha += 1.0f;
        state->successes++;
    } else {
        state->beta += 1.0f;
    }
    state->observations++;
}

float b3_predict(b3_state_t *state)
{
    /*
     * Posterior mean of Beta(α, β):
     *   E[f|data] = α / (α + β)
     *
     * With uniform prior and n observations (s successes):
     *   = (1 + s) / (2 + n)
     *
     * This naturally handles the cold-start problem:
     *   n=0: returns 0.5 (uniform prior, no opinion)
     *   n=1, s=1: returns 0.667 (one hit, slightly above 0.5)
     *   n=100, s=30: returns 0.304 (converging on 30%)
     */
    float total = state->alpha + state->beta;
    if (total < 0.001f) return 0.5f;    /* safety: avoid div by zero */
    return state->alpha / total;
}

float b3_variance(b3_state_t *state)
{
    /*
     * Variance of Beta(α, β):
     *   Var = (α × β) / ((α + β)² × (α + β + 1))
     *
     * For uniform prior Beta(1,1):
     *   Var = 1 / (4 × 3) = 0.083
     *
     * After 100 observations:
     *   Var ≈ 0.002 (very tight)
     *
     * Variance shrinks as ~1/n. More data = more certainty.
     */
    float ab = state->alpha + state->beta;
    if (ab < 0.001f) return 1.0f;       /* safety */

    float ab_sq = ab * ab;
    float denom = ab_sq * (ab + 1.0f);
    if (denom < 0.001f) return 1.0f;    /* safety */

    return (state->alpha * state->beta) / denom;
}

float b3_confidence(b3_state_t *state)
{
    /*
     * Convenience metric: 1.0 - variance.
     *
     * Not a p-value or credible interval — just a quick
     * "how sure are we" number for chain scheduling.
     *
     * 0.92 = pretty sure. 0.75 = brand new. 0.99 = locked in.
     */
    float v = b3_variance(state);
    float c = 1.0f - v;
    if (c < 0.0f) c = 0.0f;
    return c;
}

/* ── Layer 1: Brad's Balance (static) ───────────── */

float b3_bb_static(float C, float I, float V)
{
    /*
     * Original Brad's Balance formula:
     *   f(C, I, V) = (C × I × V) / 1000
     *
     * Returns the oppositional fraction for a single scenario.
     *
     * Example:
     *   C=8, I=5, V=3 → (8×5×3)/1000 = 120/1000 = 0.12
     *   With population P=1000: O = 1000 × 0.12 = 120 oppositional
     *
     * This is Layer 1 — no learning, no updates.
     * Use b3_full() for the Bayesian-corrected version.
     */
    return (C * I * V) / 1000.0f;
}

/* ── Full B3: population × Bayesian posterior ───── */

float b3_full(b3_state_t *state, int population)
{
    /*
     * The complete B3 pipeline:
     *
     *   result = P × E[f|data]
     *
     * Where E[f|data] is the Bayesian posterior mean,
     * which starts at the prior (from DR or uniform)
     * and converges toward observed reality as evidence
     * accumulates.
     *
     * This replaces the static BB formula with a living
     * estimate that gets better with every observation.
     */
    float f = b3_predict(state);
    return (float)population * f;
}

/* ── Lifecycle ──────────────────────────────────── */

void b3_reset(b3_state_t *state)
{
    /* Back to uniform prior. All evidence erased. */
    b3_init(state);
}

void b3_merge(b3_state_t *dst, b3_state_t *a, b3_state_t *b)
{
    /*
     * Combine two independent evidence streams.
     *
     * If both started from Beta(1,1), naive addition gives:
     *   α_merged = αA + αB
     *   β_merged = βA + βB
     *
     * But that double-counts the prior (both include Beta(1,1)).
     * So we subtract one prior:
     *   α_merged = αA + αB - 1
     *   β_merged = βA + βB - 1
     *
     * Clamp to minimum 1.0 to keep a valid distribution.
     *
     * Observations and successes simply add.
     */
    float ma = a->alpha + b->alpha - 1.0f;
    float mb = a->beta  + b->beta  - 1.0f;

    if (ma < 1.0f) ma = 1.0f;
    if (mb < 1.0f) mb = 1.0f;

    dst->alpha        = ma;
    dst->beta         = mb;
    dst->observations = a->observations + b->observations;
    dst->successes    = a->successes    + b->successes;

    /*
     * Merged prior_dr: weighted average by confidence_k.
     * If one stream has stronger priors, it dominates.
     */
    float total_k = a->confidence_k + b->confidence_k;
    if (total_k > 0.001f) {
        dst->prior_dr     = (a->prior_dr * a->confidence_k +
                             b->prior_dr * b->confidence_k) / total_k;
        dst->confidence_k = total_k;
    } else {
        dst->prior_dr     = 0.5f;
        dst->confidence_k = 2.0f;
    }
}
