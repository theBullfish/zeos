//! `zplus-emit` — parse a `.zp` file and emit ZIR v1 JSON to stdout.
//!
//! This is the Rust front-end's codegen backend: `.zp` → ZIR, the interchange
//! format the kernel loader (`zplus_zir.c`) consumes. See `ZIR.md`.
//!
//! Usage: `zplus-emit <file.zp>`
//!   - exit 0: emitted ZIR to stdout
//!   - exit 2: parse error or bad argv

use std::env;
use std::fs;
use std::path::Path;
use std::process::ExitCode;

use zplus::{parse, Zir};

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    let Some(path) = args.get(1) else {
        eprintln!("usage: zplus-emit <file.zp>");
        return ExitCode::from(2);
    };

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

    let source_name = Path::new(path)
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or(path);

    let zir = Zir::lower(&module, source_name);
    print!("{}", zir.to_json());
    ExitCode::SUCCESS
}
