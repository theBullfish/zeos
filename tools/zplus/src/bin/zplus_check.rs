//! `zplus-check` — parse + type-check a `.zp` file end-to-end.
//!
//! Runs the three checker passes:
//!   1. `check_module` (merge arity / chord rule)
//!   2. `check_flow_connectivity` (output(a) ↔ input(b) on Flow / Tap)
//!   3. `check_calls` (named-arg type checking against builtin signatures)
//!
//! Renders errors with source context — file:line:col header, the source
//! line, a carat under the span, and the error message. Exit 0 on clean,
//! 1 on any errors, 2 on parse error or argv mismatch.
//!
//! Usage: `zplus-check <file.zp>`

use std::env;
use std::fs;
use std::process::ExitCode;

use zplus::{check_calls, check_flow_connectivity, check_module, parse, TypeEnv, TypeError};
use zplus::ast::Span;

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} <file.zp>", args.first().map(String::as_str).unwrap_or("zplus-check"));
        return ExitCode::from(2);
    }
    let path = &args[1];
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
            render_parse_error(path, &src, &e);
            return ExitCode::from(2);
        }
    };

    let env = TypeEnv::default_with_builtins();
    let mut errors: Vec<TypeError> = Vec::new();
    errors.extend(check_module(&module));
    errors.extend(check_flow_connectivity(&module, &env));
    errors.extend(check_calls(&module, &env));

    if errors.is_empty() {
        println!("{}: ok ({} stmts)", path, module.stmts.len());
        return ExitCode::from(0);
    }

    let line_starts = compute_line_starts(&src);
    for err in &errors {
        render_type_error(path, &src, &line_starts, err);
    }
    eprintln!("\n{} error(s)", errors.len());
    ExitCode::from(1)
}

fn compute_line_starts(src: &str) -> Vec<usize> {
    let mut starts = vec![0];
    for (i, b) in src.bytes().enumerate() {
        if b == b'\n' {
            starts.push(i + 1);
        }
    }
    starts
}

fn line_col_of(line_starts: &[usize], offset: usize) -> (usize, usize) {
    // Binary search for the line containing `offset`.
    let line_idx = match line_starts.binary_search(&offset) {
        Ok(i) => i,
        Err(i) => i.saturating_sub(1),
    };
    let line_start = line_starts[line_idx];
    (line_idx + 1, offset - line_start + 1)
}

fn line_text<'a>(src: &'a str, line_starts: &[usize], line_1based: usize) -> &'a str {
    let idx = line_1based - 1;
    let start = line_starts[idx];
    let end = line_starts.get(idx + 1).copied().unwrap_or(src.len());
    let line = &src[start..end];
    line.strip_suffix('\n').unwrap_or(line).strip_suffix('\r').unwrap_or_else(|| line.strip_suffix('\n').unwrap_or(line))
}

fn render_type_error(path: &str, src: &str, line_starts: &[usize], err: &TypeError) {
    let (line, col) = line_col_of(line_starts, err.span.start);
    let text = line_text(src, line_starts, line);
    eprintln!("{}:{}:{}: error: {}", path, line, col, err.message);
    eprintln!("  | {}", text);
    let span_len = err.span.end.saturating_sub(err.span.start).max(1);
    let carets = "^".repeat(span_len);
    let lead = " ".repeat(col.saturating_sub(1));
    eprintln!("  | {}{}", lead, carets);
}

fn render_parse_error(path: &str, src: &str, err: &zplus::ParseError) {
    let line_starts = compute_line_starts(src);
    let (line, col) = line_col_of(&line_starts, err.span.start);
    let text = line_text(src, &line_starts, line);
    eprintln!("{}:{}:{}: parse error: {}", path, line, col, err.message);
    eprintln!("  | {}", text);
    let span_len = err.span.end.saturating_sub(err.span.start).max(1);
    let carets = "^".repeat(span_len);
    let lead = " ".repeat(col.saturating_sub(1));
    eprintln!("  | {}{}", lead, carets);
    let _ = Span::dummy(); // suppress unused
}
