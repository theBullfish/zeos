//! `zplus-parse` — tokenize then parse a `.zp` file. Exit 0 on success;
//! prints the error and exits 1 on parse failure.

use std::env;
use std::fs;
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} <file.zp>", args.first().map(String::as_str).unwrap_or("zplus-parse"));
        return ExitCode::from(2);
    }
    let path = &args[1];
    let src = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: failed to read {}: {}", path, e);
            return ExitCode::from(1);
        }
    };

    match zplus::parse(&src) {
        Ok(m) => {
            println!("{} stmts", m.stmts.len());
            ExitCode::from(0)
        }
        Err(e) => {
            eprintln!("{}: {}", path, e);
            ExitCode::from(1)
        }
    }
}
