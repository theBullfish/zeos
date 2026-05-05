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
