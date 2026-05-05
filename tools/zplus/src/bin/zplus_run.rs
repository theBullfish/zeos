//! `zplus-run` — parse, type-check, and EXECUTE a `.zp` file.
//!
//! Usage: `zplus-run <file.zp> [ticks]`
//!   - default ticks: 5
//!   - exits 0 if the run completed, 1 on type errors / runtime errors,
//!     2 on parse error or argv mismatch.
//!
//! Skips the type-checker passes intentionally — `zplus-check` is the
//! tool for that. `zplus-run` is "make it go."

use std::env;
use std::fs;
use std::process::ExitCode;

use zplus::{parse, Runtime};

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 || args.len() > 3 {
        eprintln!("usage: {} <file.zp> [ticks]", args.first().map(String::as_str).unwrap_or("zplus-run"));
        return ExitCode::from(2);
    }
    let path = &args[1];
    let ticks: u64 = args
        .get(2)
        .map(|s| s.parse().unwrap_or(5))
        .unwrap_or(5);
    let src = match fs::read_to_string(path) {
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

    let mut rt = Runtime::new(module);
    let emissions = match rt.run(ticks) {
        Ok(e) => e,
        Err(e) => {
            eprintln!("{}: {}", path, e);
            return ExitCode::from(1);
        }
    };

    if emissions.is_empty() {
        println!("(no emissions over {} ticks)", ticks);
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
