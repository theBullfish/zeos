//! `zplus-lex` — dump the token stream for a `.zp` source file.
//!
//! Usage: `zplus-lex <file.zp>`. Writes one token per line to stdout in
//! `Kind("text") @ start..end` format.

use std::env;
use std::fs;
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} <file.zp>", args.first().map(String::as_str).unwrap_or("zplus-lex"));
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

    let mut had_error = false;
    for tok in zplus::lex(&src) {
        println!("{}", tok);
        if matches!(tok.kind, zplus::TokenKind::Error) {
            had_error = true;
        }
    }
    if had_error {
        ExitCode::from(1)
    } else {
        ExitCode::from(0)
    }
}
