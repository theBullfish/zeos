//! `zplus` — single umbrella binary for the Z+ toolchain.
//!
//! Subdispatches to the four toolchain commands:
//!
//!   zplus lex   <file.zp>            — dump token stream
//!   zplus parse <file.zp>            — parse and report stmt count
//!   zplus check <file.zp>            — full type check with source-context
//!   zplus run   <file.zp> [ticks]    — execute via tree-walking runtime
//!                                      (accepts --ms-per-tick N)
//!
//! The four sub-binaries (`zplus-lex`, `zplus-parse`, `zplus-check`,
//! `zplus-run`) still ship for scripting / piping. This umbrella is
//! the front door.

use std::env;
use std::process::{Command, ExitCode};

const COMMANDS: &[(&str, &str, &str)] = &[
    ("lex",   "zplus-lex",   "dump token stream for a .zp file"),
    ("parse", "zplus-parse", "parse a .zp file and report stmt count"),
    ("check", "zplus-check", "type-check a .zp file with source-context errors"),
    ("run",   "zplus-run",   "execute a .zp file via the runtime"),
    ("tune",  "zplus-tune",  "run + report + suggest config tunings"),
];

fn print_help(prog: &str) {
    eprintln!("zplus — Z+ language toolchain");
    eprintln!();
    eprintln!("usage: {} <command> [args...]", prog);
    eprintln!();
    eprintln!("commands:");
    for (name, _, desc) in COMMANDS {
        eprintln!("  {:<6}  {}", name, desc);
    }
    eprintln!();
    eprintln!("examples:");
    eprintln!("  {} run programs/demos/heartbeat.zp 5", prog);
    eprintln!("  {} run programs/demos/time_machine.zp 5 --ms-per-tick 60000", prog);
    eprintln!("  {} check programs/02_log_monitor.zp", prog);
    eprintln!();
    eprintln!("Each subcommand also exists as a standalone binary");
    eprintln!("(zplus-lex, zplus-parse, zplus-check, zplus-run).");
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    let prog = args
        .first()
        .map(|s| s.rsplit('/').next().unwrap_or(s).to_string())
        .unwrap_or_else(|| "zplus".to_string());

    let Some(cmd) = args.get(1) else {
        print_help(&prog);
        return ExitCode::from(2);
    };

    if cmd == "-h" || cmd == "--help" || cmd == "help" {
        print_help(&prog);
        return ExitCode::from(0);
    }

    let Some(target) = COMMANDS.iter().find(|(name, _, _)| *name == cmd).map(|(_, bin, _)| *bin)
    else {
        eprintln!("zplus: unknown command '{}'", cmd);
        eprintln!("run `{} help` for the list", prog);
        return ExitCode::from(2);
    };

    // Forward the rest of argv to the target binary. Same directory as
    // the umbrella so `cargo install --path tools/zplus` puts them all
    // together.
    let target_path = match env::current_exe() {
        Ok(p) => p
            .parent()
            .map(|d| d.join(target))
            .unwrap_or_else(|| target.into()),
        Err(_) => target.into(),
    };

    let status = Command::new(&target_path)
        .args(&args[2..])
        .status();

    match status {
        Ok(s) => match s.code() {
            Some(c) => ExitCode::from(c.min(255).max(0) as u8),
            None => ExitCode::from(1),
        },
        Err(e) => {
            eprintln!(
                "zplus: failed to launch {}: {}\n\
                 (make sure the sub-binary is on PATH or installed alongside)",
                target_path.display(),
                e
            );
            ExitCode::from(2)
        }
    }
}
