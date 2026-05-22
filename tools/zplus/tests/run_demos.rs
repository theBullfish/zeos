//! Integration test: actually RUN the demo programs in
//! `programs/demos/` and assert the runtime emits the expected
//! shape of output.
//!
//! These are the smallest-possible Z+ programs that execute end-to-end:
//! parse → AST → Runtime → emissions. If any of them stop running,
//! the runtime regressed.

use std::fs;
use zplus::{parse, Runtime, Value};

fn run_file(rel: &str, ticks: u64) -> Vec<zplus::Emission> {
    let src = fs::read_to_string(rel)
        .or_else(|_| fs::read_to_string(format!("../{}", rel)))
        .or_else(|_| fs::read_to_string(format!("../../{}", rel)))
        .expect("read demo");
    let module = parse(&src).expect("parse demo");
    let mut rt = Runtime::new(module);
    rt.run(ticks).expect("run demo")
}

#[test]
fn heartbeat_emits_one_tick_per_step() {
    let emissions = run_file("programs/demos/heartbeat.zp", 5);
    assert_eq!(emissions.len(), 5, "expected 5 emissions, got {}", emissions.len());
    for (i, e) in emissions.iter().enumerate() {
        assert_eq!(e.tick, (i + 1) as u64);
        assert_eq!(e.sink, "print");
        assert!(matches!(e.value, Value::Tick(_)));
    }
}

#[test]
fn fanout_emits_three_per_tick() {
    let emissions = run_file("programs/demos/fanout.zp", 3);
    assert_eq!(emissions.len(), 9, "expected 9 emissions, got {}", emissions.len());
    for tick_n in 1..=3 {
        let on_this_tick: Vec<_> = emissions.iter().filter(|e| e.tick == tick_n).collect();
        assert_eq!(on_this_tick.len(), 3, "tick {}: expected 3 sinks", tick_n);
    }
}

#[test]
fn threshold_gate_filters_below_3() {
    let emissions = run_file("programs/demos/threshold.zp", 5);
    let ticks: Vec<u64> = emissions.iter().map(|e| e.tick).collect();
    assert_eq!(ticks, vec![4, 5]);
}

#[test]
fn ramp_delta_emits_zero_then_ones() {
    let emissions = run_file("programs/demos/ramp.zp", 5);
    assert_eq!(emissions.len(), 5);
    let values: Vec<Value> = emissions.iter().map(|e| e.value.clone()).collect();
    assert_eq!(values, vec![
        Value::Float(0.0),
        Value::Float(1.0),
        Value::Float(1.0),
        Value::Float(1.0),
        Value::Float(1.0),
    ]);
}

#[test]
fn two_speeds_fast_and_slow() {
    let emissions = run_file("programs/demos/two_speeds.zp", 6);
    let fast: Vec<_> = emissions
        .iter()
        .filter(|e| e.label.as_deref() == Some("info") && matches!(e.value, Value::Tick(_)))
        .collect();
    // fast (rate 1) fires every tick: 6 emissions.
    // slow (rate 2) fires on 2,4,6: 3 emissions.
    // alert sink doesn't distinguish them, so total = 6 + 3 = 9.
    assert_eq!(fast.len(), 9, "expected 9 emissions across both rates, got {}", fast.len());
}
