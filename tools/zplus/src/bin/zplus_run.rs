//! `zplus-run` — parse, type-check, and EXECUTE a `.zp` file.
//!
//! Usage: `zplus-run <file.zp> [ticks] [--ms-per-tick N]`
//!   - default ticks: 5
//!   - default ms-per-tick: 1000 (one tick = one simulated second)
//!     change this to redefine our relationship to ticks — e.g.
//!     `--ms-per-tick 60000` makes one tick = one simulated minute.
//!   - exits 0 if the run completed, 1 on type errors / runtime errors,
//!     2 on parse error or argv mismatch.
//!
//! Skips the type-checker passes intentionally — `zplus-check` is the
//! tool for that. `zplus-run` is "make it go."

use std::env;
use std::fs;
use std::process::ExitCode;

use zplus::{parse, Runtime};
use zplus::runtime::RuntimeConfig;

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    let prog = args.first().map(String::as_str).unwrap_or("zplus-run");

    // Parse args: <file> [ticks] [--ms-per-tick N]
    let mut path: Option<String> = None;
    let mut ticks: u64 = 5;
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
            ticks = a.parse().unwrap_or(5);
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
    let emissions = match rt.run(ticks) {
        Ok(e) => e,
        Err(e) => {
            eprintln!("{}: {}", path, e);
            return ExitCode::from(1);
        }
    };

    if emissions.is_empty() {
        println!("(no emissions over {} ticks; sim time {}ms)", ticks, rt.simulated_time_ms());
    } else {
        for e in &emissions {
            match &e.label {
                Some(label) => println!("[t={}] {}({}: {})", e.tick, e.sink, label, e.value),
                None => println!("[t={}] {}: {}", e.tick, e.sink, e.value),
            }
        }
    }

    ExitCode::from(0)
}

