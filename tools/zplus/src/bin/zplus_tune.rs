//! `zplus-tune` — run a `.zp` file, generate a Report, run the heuristic
//! tuner, print suggested config changes.
//!
//! Usage: `zplus-tune <file.zp> [ticks] [--ms-per-tick N]`
//!
//! Output: the report summary followed by zero or more tuning
//! suggestions. Each suggestion shows before/after RuntimeConfig and
//! an expected-impact note.
//!
//! This is the v1 tuning surface — heuristic-only. When the report
//! corpus is large enough, a real ML pass replaces it (predict best
//! config given program-shape signature).

use std::env;
use std::fs;
use std::process::ExitCode;

use zplus::report::Aggregator;
use zplus::runtime::RuntimeConfig;
use zplus::tune::tune_one;
use zplus::{parse, Runtime};

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    let prog = args.first().map(String::as_str).unwrap_or("zplus-tune");

    let mut path: Option<String> = None;
    let mut ticks: u64 = 50;
    let mut ms_per_tick: u64 = 1000;
    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        if a == "--ms-per-tick" {
            i += 1;
            if i >= args.len() {
                eprintln!("usage: {} <file.zp> [ticks] [--ms-per-tick N]", prog);
                return ExitCode::from(2);
            }
            ms_per_tick = args[i].parse().unwrap_or(1000);
        } else if path.is_none() {
            path = Some(a.clone());
        } else {
            ticks = a.parse().unwrap_or(50);
        }
        i += 1;
    }
    let Some(path) = path else {
        eprintln!("usage: {} <file.zp> [ticks] [--ms-per-tick N]", prog);
        return ExitCode::from(2);
    };

    let src = match fs::read_to_string(&path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: failed to read {}: {}", path, e);
            return ExitCode::from(2);
        }
    };
    let module = match parse(&src) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("{}: parse error: {}", path, e.message);
            return ExitCode::from(2);
        }
    };

    let config = RuntimeConfig {
        ticks_per_real_second: None,
        simulated_ms_per_tick: ms_per_tick,
    };
    let mut rt = Runtime::with_config(module, config);
    let agg = Aggregator::attach(&mut rt);
    if let Err(e) = rt.run(ticks) {
        eprintln!("{}: runtime error: {}", path, e);
        return ExitCode::from(1);
    }

    let report = agg.lock().unwrap().report();
    println!("=== Report for {} ({} ticks) ===", path, ticks);
    print!("{}", report.render());

    let suggestions = tune_one(config, &report);
    if suggestions.is_empty() {
        println!();
        println!("no tuning suggestions — config looks good for this program");
    } else {
        println!();
        println!("=== {} Suggestion(s) ===", suggestions.len());
        for (i, s) in suggestions.iter().enumerate() {
            println!();
            println!("[{}] {}", i + 1, s.render());
        }
    }
    ExitCode::from(0)
}
