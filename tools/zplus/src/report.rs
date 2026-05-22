//! Z+ runtime reporter / aggregator.
//!
//! Consumes the `RuntimeEvent` stream (via the `Observer` trait) and
//! produces a `Report` summarizing what happened. This is the
//! programmatic foundation for the opt-in test-reporting pipeline
//! described in `specs/OPT_IN_TEST_REPORTING.md` and
//! `specs/MEASUREMENT_TARGETS.md`.
//!
//! Per-window aggregation by default (matches MEASUREMENT_TARGETS §
//! Decisions: per-window minimizes report size and avoids
//! fingerprinting). Per-event for chord-rule fallbacks (where the
//! merge fired with `passed: false` despite some inputs firing) — these
//! are the forensic-detail cases that justify per-event capture.
//!
//! ## Surfaces (subset of MEASUREMENT_TARGETS.md categories)
//!
//! Category 3 (signal-graph runtime / chord rule) is the one v1
//! covers fully. Each `Report` carries:
//!
//! - `total_ticks`               — how many ticks ran
//! - `total_emissions`           — sinks fired total
//! - `emissions_per_sink`        — histogram by sink name
//! - `merge_total_resolutions`   — Merge nodes resolved (per-tick × node)
//! - `merge_passes`              — chord formed (resolved → downstream)
//! - `merge_failures`            — fired < required-by-policy
//! - `merge_fallback_events`     — per-event records of chord misses;
//!                                  forensic detail for tuning
//!
//! Categories 1, 2, 4-10 are reserved for later (see
//! `MEASUREMENT_TARGETS.md`).

use crate::runtime::{Emission, Observer, RuntimeEvent};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

/// One observed merge that didn't form a chord — kept verbatim for
/// per-event reporting per MEASUREMENT_TARGETS.md §Decisions.
#[derive(Debug, Clone, PartialEq)]
pub struct MergeFallbackEvent {
    pub tick: u64,
    pub policy: String,
    pub fired: usize,
    pub total: usize,
}

/// Aggregated runtime report. Snapshot-style; produced by
/// `Aggregator::report()` after a run completes.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Report {
    pub total_ticks: u64,
    pub total_emissions: u64,
    pub emissions_per_sink: HashMap<String, u64>,
    pub merge_total_resolutions: u64,
    pub merge_passes: u64,
    pub merge_failures: u64,
    pub merge_fallback_events: Vec<MergeFallbackEvent>,
}

impl Report {
    /// Pass rate of merges that fired AT ALL (vs ones that didn't form
    /// a chord). 1.0 = every merge that ran produced a downstream
    /// value; 0.0 = every merge missed.
    pub fn merge_pass_rate(&self) -> f64 {
        if self.merge_total_resolutions == 0 {
            return 1.0; // vacuously OK — no merges ran
        }
        self.merge_passes as f64 / self.merge_total_resolutions as f64
    }

    /// Average emissions per tick. Useful as a tuning signal —
    /// programs producing zero emissions over many ticks may be
    /// silent-by-design or stuck.
    pub fn emissions_per_tick(&self) -> f64 {
        if self.total_ticks == 0 {
            return 0.0;
        }
        self.total_emissions as f64 / self.total_ticks as f64
    }

    /// Render the report as a multi-line human-readable summary.
    /// Format is intentionally simple — JSON / `.sgs` serialization
    /// comes later when the transport ships.
    pub fn render(&self) -> String {
        let mut out = String::new();
        out.push_str(&format!("ticks: {}\n", self.total_ticks));
        out.push_str(&format!(
            "emissions: {} ({:.2}/tick)\n",
            self.total_emissions,
            self.emissions_per_tick()
        ));
        if !self.emissions_per_sink.is_empty() {
            out.push_str("  by sink:\n");
            let mut sinks: Vec<_> = self.emissions_per_sink.iter().collect();
            sinks.sort_by(|a, b| b.1.cmp(a.1));
            for (sink, count) in sinks {
                out.push_str(&format!("    {:<14} {}\n", sink, count));
            }
        }
        if self.merge_total_resolutions > 0 {
            out.push_str(&format!(
                "merges: {} resolutions, {} passes, {} failures ({:.0}% pass)\n",
                self.merge_total_resolutions,
                self.merge_passes,
                self.merge_failures,
                self.merge_pass_rate() * 100.0
            ));
            if !self.merge_fallback_events.is_empty() {
                out.push_str(&format!(
                    "  chord-rule misses: {} (per-event detail captured)\n",
                    self.merge_fallback_events.len()
                ));
            }
        }
        out
    }
}

/// Live aggregator. Implements `Observer` so it can be installed via
/// `Runtime::set_observer`. After the run, call `report()` to extract.
///
/// Wrap in `Arc<Mutex<...>>` if you want to read it back after install
/// — `set_observer` takes ownership. The convenience `attach()` method
/// installs the aggregator and returns the shared handle.
#[derive(Debug, Default)]
pub struct Aggregator {
    state: ReportState,
}

#[derive(Debug, Default)]
struct ReportState {
    total_ticks: u64,
    total_emissions: u64,
    emissions_per_sink: HashMap<String, u64>,
    merge_total_resolutions: u64,
    merge_passes: u64,
    merge_failures: u64,
    merge_fallback_events: Vec<MergeFallbackEvent>,
}

impl Aggregator {
    pub fn new() -> Self { Self::default() }

    /// Install on a runtime via a shared `Arc<Mutex<...>>`. Returns the
    /// shared handle so callers can read the aggregated state after
    /// the run. Common pattern:
    ///
    /// ```no_run
    /// use zplus::{parse, Runtime};
    /// use zplus::report::Aggregator;
    /// let module = parse("x : tick(rate: 1) -> print").unwrap();
    /// let mut rt = Runtime::new(module);
    /// let agg = Aggregator::attach(&mut rt);
    /// rt.run(5).unwrap();
    /// let report = agg.lock().unwrap().report();
    /// println!("{}", report.render());
    /// ```
    pub fn attach<'src>(rt: &mut crate::Runtime<'src>) -> Arc<Mutex<Aggregator>> {
        let shared = Arc::new(Mutex::new(Aggregator::new()));
        let observer = AggregatorObserver { shared: shared.clone() };
        rt.set_observer(observer);
        shared
    }

    pub fn report(&self) -> Report {
        Report {
            total_ticks: self.state.total_ticks,
            total_emissions: self.state.total_emissions,
            emissions_per_sink: self.state.emissions_per_sink.clone(),
            merge_total_resolutions: self.state.merge_total_resolutions,
            merge_passes: self.state.merge_passes,
            merge_failures: self.state.merge_failures,
            merge_fallback_events: self.state.merge_fallback_events.clone(),
        }
    }

    fn ingest(&mut self, e: &RuntimeEvent) {
        match e {
            RuntimeEvent::TickStart { .. } => {
                self.state.total_ticks += 1;
            }
            RuntimeEvent::Emitted(Emission { sink, .. }) => {
                self.state.total_emissions += 1;
                *self.state.emissions_per_sink.entry(sink.clone()).or_insert(0) += 1;
            }
            RuntimeEvent::MergeResolved { tick, policy, fired, total, passed } => {
                self.state.merge_total_resolutions += 1;
                if *passed {
                    self.state.merge_passes += 1;
                } else {
                    self.state.merge_failures += 1;
                    // Per-event capture: chord-rule miss is the case
                    // MEASUREMENT_TARGETS.md §Decisions promotes to
                    // per-event reporting.
                    self.state.merge_fallback_events.push(MergeFallbackEvent {
                        tick: *tick,
                        policy: policy.clone(),
                        fired: *fired,
                        total: *total,
                    });
                }
            }
            RuntimeEvent::BuiltinCall { .. } => {
                // Reserved for category 1 (language drift) — count
                // builtin call shapes per program. v1 doesn't emit
                // these yet.
            }
        }
    }
}

struct AggregatorObserver {
    shared: Arc<Mutex<Aggregator>>,
}

impl Observer for AggregatorObserver {
    fn on_event(&mut self, event: &RuntimeEvent) {
        if let Ok(mut agg) = self.shared.lock() {
            agg.ingest(event);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{parse, Runtime};

    #[test]
    fn report_default_is_clean() {
        let r = Report::default();
        assert_eq!(r.merge_pass_rate(), 1.0); // vacuous
        assert_eq!(r.emissions_per_tick(), 0.0);
        assert!(r.render().contains("ticks: 0"));
    }

    #[test]
    fn aggregator_counts_ticks_and_emissions() {
        let module = parse("x : tick(rate: 1) -> print").expect("parse");
        let mut rt = Runtime::new(module);
        let agg = Aggregator::attach(&mut rt);
        rt.run(5).expect("run");
        let r = agg.lock().unwrap().report();
        assert_eq!(r.total_ticks, 5);
        assert_eq!(r.total_emissions, 5);
        assert_eq!(r.emissions_per_sink.get("print"), Some(&5));
        assert_eq!(r.emissions_per_tick(), 1.0);
    }

    #[test]
    fn aggregator_tracks_merge_passes() {
        // Two clocks at rate 1 + All-policy merge → both fire each tick
        // → all merges pass.
        let src = "
            a : tick(rate: 1) -> a_wire
            b : tick(rate: 1) -> b_wire
            a_wire -> |
            b_wire -> | -> alert(info: \"both\")
        ";
        let module = parse(src).expect("parse");
        let mut rt = Runtime::new(module);
        let agg = Aggregator::attach(&mut rt);
        rt.run(3).expect("run");
        let r = agg.lock().unwrap().report();
        assert!(r.merge_total_resolutions >= 3);
        assert_eq!(r.merge_failures, 0);
        assert_eq!(r.merge_pass_rate(), 1.0);
        assert!(r.merge_fallback_events.is_empty());
    }

    #[test]
    fn aggregator_captures_chord_rule_misses() {
        // Two clocks at different rates, All-policy merge — many ticks
        // only ONE input fires, so the merge misses the chord.
        let src = "
            fast : tick(rate: 1) -> fast_wire
            slow : tick(rate: 3) -> slow_wire
            fast_wire -> |
            slow_wire -> | -> alert(info: \"both\")
        ";
        let module = parse(src).expect("parse");
        let mut rt = Runtime::new(module);
        let agg = Aggregator::attach(&mut rt);
        rt.run(6).expect("run");
        let r = agg.lock().unwrap().report();
        // slow fires on tick 3 and 6 — chord forms twice.
        // Other ticks: only fast fired → miss.
        assert!(r.merge_failures >= 1, "expected misses, got: {:?}", r);
        assert!(!r.merge_fallback_events.is_empty());
        // Per-event detail captured.
        for ev in &r.merge_fallback_events {
            assert_eq!(ev.policy, "all");
            assert_eq!(ev.total, 2);
            assert!(ev.fired < ev.total);
        }
    }

    #[test]
    fn render_is_human_readable() {
        let mut state = ReportState::default();
        state.total_ticks = 10;
        state.total_emissions = 30;
        state.emissions_per_sink.insert("print".into(), 20);
        state.emissions_per_sink.insert("alert".into(), 10);
        state.merge_total_resolutions = 5;
        state.merge_passes = 4;
        state.merge_failures = 1;
        let agg = Aggregator { state };
        let r = agg.report();
        let rendered = r.render();
        assert!(rendered.contains("ticks: 10"));
        assert!(rendered.contains("emissions: 30"));
        assert!(rendered.contains("print"));
        assert!(rendered.contains("merges: 5"));
        assert!(rendered.contains("80%"));
    }
}
