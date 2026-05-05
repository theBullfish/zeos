//! Z+ heuristic tuner.
//!
//! Consumes a corpus of `Report`s (from running programs at varying
//! configs) and produces `Suggestion`s — concrete `RuntimeConfig`
//! adjustments that should improve some metric. v1 is heuristic-only;
//! when the report corpus is large enough, a real ML pass replaces it
//! (regression / decision tree on program-shape × config →
//! emission-rate / chord-pass-rate).
//!
//! Heuristics implemented:
//!
//! 1. **Chord-miss → faster ticks.** If a program's `merge_pass_rate`
//!    is low (< 0.5), its inputs are firing on different ticks. Cutting
//!    `simulated_ms_per_tick` makes durations map to more ticks, giving
//!    inputs more chances to align in time. Suggest halving until
//!    chords form ≥ 80% of the time.
//!
//! 2. **Silent run → check for stuck.** If `total_emissions == 0` over
//!    many ticks, either the program is silent-by-design (unlikely
//!    for a runtime test) or a transformer is filtering everything.
//!    Suggest tracing — no config change can rescue this; it's a
//!    program-shape issue.
//!
//! 3. **Excessive emissions → consider compression.** If
//!    `emissions_per_tick > 100`, the simulated clock might be too
//!    fast for the workload. Suggest INCREASING
//!    `simulated_ms_per_tick` (compress simulated time) so each tick
//!    represents more real activity per emission.
//!
//! These are starting heuristics. They get refined as we collect real
//! reports.

use crate::report::Report;
use crate::runtime::RuntimeConfig;

/// A concrete suggestion for tuning. `before` is the config used during
/// the run that produced the report; `after` is what to try.
#[derive(Debug, Clone, PartialEq)]
pub struct Suggestion {
    pub reason: String,
    pub before: RuntimeConfig,
    pub after: RuntimeConfig,
    /// Expected impact on a key metric, if the heuristic has one. Free
    /// text for v1; structured for v2 once we calibrate.
    pub expected_impact: String,
}

impl Suggestion {
    pub fn render(&self) -> String {
        let format_cfg = |c: &RuntimeConfig| {
            format!(
                "ticks_per_real_second={:?}, simulated_ms_per_tick={}",
                c.ticks_per_real_second, c.simulated_ms_per_tick
            )
        };
        format!(
            "suggestion: {}\n  before: {}\n   after: {}\n  expect: {}",
            self.reason,
            format_cfg(&self.before),
            format_cfg(&self.after),
            self.expected_impact,
        )
    }
}

/// Suggest config tunings for a single (config, report) pair.
/// Returns zero or more suggestions; ordered by heuristic priority
/// (the first is the most impactful).
pub fn tune_one(config: RuntimeConfig, report: &Report) -> Vec<Suggestion> {
    let mut suggestions = Vec::new();

    // Heuristic 1: chord-miss → faster ticks
    if report.merge_total_resolutions > 0 {
        let pass_rate = report.merge_pass_rate();
        if pass_rate < 0.8 && report.merge_failures > 0 {
            let halved = (config.simulated_ms_per_tick / 2).max(1);
            if halved < config.simulated_ms_per_tick {
                suggestions.push(Suggestion {
                    reason: format!(
                        "chord pass rate {:.0}% — inputs are missing each other in time. \
                         Halving simulated_ms_per_tick gives durations more granular tick \
                         coverage so inputs have more chances to align.",
                        pass_rate * 100.0
                    ),
                    before: config,
                    after: RuntimeConfig {
                        simulated_ms_per_tick: halved,
                        ..config
                    },
                    expected_impact: format!(
                        "merge_pass_rate {:.0}% → expected ≥ 60% (heuristic)",
                        pass_rate * 100.0
                    ),
                });
            }
        }
    }

    // Heuristic 2: silent run
    if report.total_ticks > 0 && report.total_emissions == 0 {
        suggestions.push(Suggestion {
            reason: format!(
                "ran {} ticks with zero emissions — either silent-by-design \
                 or a transformer is filtering everything. No config change \
                 will help; trace the chain (zplus run with verbose / \
                 instrumented observer) to find the gate.",
                report.total_ticks
            ),
            before: config,
            after: config,
            expected_impact: "no config change — diagnostic only".into(),
        });
    }

    // Heuristic 3: excessive emissions → compress time
    if report.emissions_per_tick() > 100.0 {
        let compressed = config.simulated_ms_per_tick.saturating_mul(10);
        suggestions.push(Suggestion {
            reason: format!(
                "{:.0} emissions/tick — simulated clock may be too fast for \
                 the workload. Increasing simulated_ms_per_tick compresses \
                 simulated time so each tick represents more activity.",
                report.emissions_per_tick()
            ),
            before: config,
            after: RuntimeConfig {
                simulated_ms_per_tick: compressed,
                ..config
            },
            expected_impact: format!(
                "emissions/tick from {:.0} → expected ~{:.0}",
                report.emissions_per_tick(),
                report.emissions_per_tick() / 10.0
            ),
        });
    }

    suggestions
}

/// Aggregate suggestions across a corpus of (config, report) pairs.
/// v1 just concatenates per-program suggestions; v2 may dedupe and
/// rank across the whole corpus.
pub fn tune_corpus(pairs: &[(RuntimeConfig, Report)]) -> Vec<Suggestion> {
    let mut all = Vec::new();
    for (cfg, report) in pairs {
        all.extend(tune_one(*cfg, report));
    }
    all
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn report_with(merge_total: u64, merge_pass: u64, emit_total: u64, ticks: u64) -> Report {
        Report {
            total_ticks: ticks,
            total_emissions: emit_total,
            emissions_per_sink: HashMap::new(),
            merge_total_resolutions: merge_total,
            merge_passes: merge_pass,
            merge_failures: merge_total - merge_pass,
            merge_fallback_events: Vec::new(),
        }
    }

    #[test]
    fn no_suggestions_for_clean_run() {
        let cfg = RuntimeConfig::default();
        let report = report_with(10, 10, 50, 50); // 100% pass, 1 emit/tick
        assert!(tune_one(cfg, &report).is_empty());
    }

    #[test]
    fn chord_miss_suggests_faster_ticks() {
        let cfg = RuntimeConfig::default();
        let report = report_with(10, 3, 30, 30); // 30% pass
        let suggestions = tune_one(cfg, &report);
        assert!(!suggestions.is_empty());
        assert!(suggestions[0].reason.contains("chord pass rate"));
        // Should suggest a smaller sim_ms_per_tick.
        assert!(suggestions[0].after.simulated_ms_per_tick < cfg.simulated_ms_per_tick);
    }

    #[test]
    fn chord_miss_at_floor_suggests_nothing() {
        // Already at min granularity.
        let cfg = RuntimeConfig {
            ticks_per_real_second: None,
            simulated_ms_per_tick: 1,
        };
        let report = report_with(10, 1, 5, 30); // 10% pass
        // Halving 1 → still 1; no suggestion (already at floor).
        let suggestions = tune_one(cfg, &report);
        // Still empty for the chord-miss heuristic since halving has no
        // effect at floor. (Other heuristics may fire, but won't here.)
        assert!(
            !suggestions.iter().any(|s| s.reason.contains("chord pass rate")),
            "shouldn't suggest tick reduction at floor: {:?}",
            suggestions
        );
    }

    #[test]
    fn silent_run_diagnostic_suggestion() {
        let cfg = RuntimeConfig::default();
        let report = report_with(0, 0, 0, 50); // 50 ticks, 0 emissions
        let suggestions = tune_one(cfg, &report);
        assert!(!suggestions.is_empty());
        assert!(suggestions[0].reason.contains("zero emissions"));
        assert_eq!(suggestions[0].before, suggestions[0].after); // diagnostic
    }

    #[test]
    fn excessive_emissions_suggests_compression() {
        let cfg = RuntimeConfig::default();
        let report = report_with(0, 0, 5000, 10); // 500 emit/tick
        let suggestions = tune_one(cfg, &report);
        assert!(suggestions
            .iter()
            .any(|s| s.reason.contains("simulated clock may be too fast")));
        assert!(suggestions
            .iter()
            .any(|s| s.after.simulated_ms_per_tick > cfg.simulated_ms_per_tick));
    }

    #[test]
    fn render_includes_before_after() {
        let cfg = RuntimeConfig::default();
        let report = report_with(10, 1, 10, 10);
        let suggestions = tune_one(cfg, &report);
        let rendered = suggestions[0].render();
        assert!(rendered.contains("before:"));
        assert!(rendered.contains("after:"));
        assert!(rendered.contains("expect:"));
    }

    #[test]
    fn tune_corpus_concatenates() {
        let cfg = RuntimeConfig::default();
        let r1 = report_with(10, 1, 10, 10);
        let r2 = report_with(0, 0, 0, 50);
        let pairs = vec![(cfg, r1), (cfg, r2)];
        let suggestions = tune_corpus(&pairs);
        assert!(suggestions.len() >= 2);
    }
}
