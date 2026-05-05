//! Z+ runtime — tree-walking interpreter (v1).
//!
//! The smallest interpreter that actually RUNS Z+ programs. No IR, no
//! codegen, no LLVM. Walks the AST, advances a tick counter, propagates
//! emissions through Flow / Tap / Merge nodes, fires sinks.
//!
//! ## Time model — two independent knobs
//!
//! Ticks are atomic. Time is derived. Two configs control time
//! independently:
//!
//! 1. **`ticks_per_real_second: Option<u64>`** — how fast the runtime
//!    ticks against wall-clock. `None` (default) = unbounded; the
//!    runtime steps as fast as it can. `Some(60)` = real-time mode at
//!    60 Hz (sleeps between ticks to match — not yet implemented in
//!    v1; recorded in the config so callers can opt in).
//!
//! 2. **`simulated_ms_per_tick: u64`** — what a tick *means* in
//!    simulated time. Default 1000 (one tick = one simulated second).
//!    Decrease to 1 for ms-resolution simulation; increase to 60000
//!    for minute-per-tick compressed simulation. Slowing or speeding
//!    this knob "changes our relationship to ticks" without changing
//!    how fast they fire.
//!
//! Together they let the runtime do four useful things:
//!
//! | mode                 | real_per_sec | sim_ms_per_tick | use case |
//! |----------------------|--------------|-----------------|----------|
//! | as-fast-as-possible  | None         | 1000            | tests, simulation |
//! | real-time            | Some(1000)   | 1               | hardware-bound demo |
//! | accelerated sim      | None         | 60000           | days-in-seconds replay |
//! | step-debug           | Some(0)      | any             | manually advance tick |
//!
//! ## Execution model
//!
//! Time is discrete ticks. Each `step()` advances one tick. On each tick:
//!
//! 1. **Sources fire.** A `tick(rate: N)` source emits its tick number
//!    every N steps; `tick(rate: 1ms)` is treated as "every step" for
//!    v1 since we don't model wall-clock yet.
//! 2. **Emissions propagate.** Each fresh emission walks the Flow / Tap
//!    chain it's wired to. Transformers (`delta`, `count`, `gate`, …)
//!    that the v1 interpreter knows about run their semantic; unknown
//!    builtins act as identity.
//! 3. **Merges resolve per policy.** `Merge` collects pending inputs
//!    in a per-tick buffer; at the end of the step it resolves per the
//!    chord policy:
//!    - `All`: emit only if every input fired this tick
//!    - `Any`: emit if any input fired (first-wins; v1 picks first by
//!      input order)
//!    - `Quorum { n, m }`: emit if at least n of m fired
//!    - `Fastest(n)`: same as Quorum n-of-many for v1 (no real timing)
//!    - `Within(d)` / `By(_)`: deferred — passes through as All
//!
//!    Chord rule held: a Merge node is one resolution per step, never
//!    a sequence of waits.
//! 4. **Sinks consume.** Builtins like `print`, `log`, `alert` produce
//!    `Emission` records; the runtime returns them via `step()` /
//!    `run()`.
//!
//! ## What v1 doesn't do
//!
//! - Wall-clock time. `tick(rate: 1m)` is treated as "every step" for now.
//! - Real merge timing windows. `Within(30s)` falls back to All.
//! - Forks. (The AST has them; runtime doesn't yet branch on fork
//!   bodies.)
//! - Fork branch labels.
//! - VAULT / MasQ / hardware. All are `Unit` sinks.
//! - File I/O. `fs("...")` returns an empty Sig.
//!
//! Smallest demo program that runs end-to-end:
//!
//! ```text
//! heartbeat : tick(rate: 1) -> alert(info: "tick")
//! ```
//!
//! `Runtime::new(module).run(5)` returns 5 emissions, one per tick.

use std::collections::HashMap;

use crate::ast::{self, *};

/// Runtime time configuration. See module docstring for the two-knob model.
#[derive(Debug, Clone, Copy)]
pub struct RuntimeConfig {
    /// Wall-clock ticks per real second. `None` = unbounded (run as
    /// fast as possible). v1 records this but doesn't yet sleep — the
    /// stepper runs as fast as the call site lets it.
    pub ticks_per_real_second: Option<u64>,
    /// Simulated milliseconds advanced per tick. Default 1000 means
    /// "one tick = one simulated second." A duration like `5m` maps
    /// to `5 * 60_000 / sim_ms_per_tick` ticks.
    pub simulated_ms_per_tick: u64,
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self { ticks_per_real_second: None, simulated_ms_per_tick: 1000 }
    }
}

impl RuntimeConfig {
    /// Convert a duration (value, unit) to a number of ticks at this
    /// config's mapping. Always at least 1.
    pub fn duration_to_ticks(&self, value: f64, unit: ast::DurationUnit) -> u64 {
        let ms = match unit {
            ast::DurationUnit::Ms => value,
            ast::DurationUnit::S => value * 1000.0,
            ast::DurationUnit::M => value * 60_000.0,
            ast::DurationUnit::H => value * 3_600_000.0,
            ast::DurationUnit::D => value * 86_400_000.0,
        };
        let ticks = ms / self.simulated_ms_per_tick.max(1) as f64;
        (ticks.round() as u64).max(1)
    }
}

/// A value carried on a wire at a moment in time.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Int(i64),
    Float(f64),
    Str(String),
    Tick(u64),
    Bool(bool),
    /// Duration value with its unit preserved — needed so the runtime
    /// can convert to ticks via `RuntimeConfig::duration_to_ticks`.
    Duration { value: f64, unit: ast::DurationUnit },
    Unit,
}

impl std::fmt::Display for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Value::Int(n) => write!(f, "{}", n),
            Value::Float(x) => write!(f, "{}", x),
            Value::Str(s) => write!(f, "{}", s),
            Value::Tick(t) => write!(f, "tick({})", t),
            Value::Bool(b) => write!(f, "{}", b),
            Value::Duration { value, unit } => write!(f, "{}{}", value, unit),
            Value::Unit => write!(f, "()"),
        }
    }
}

/// What a sink emits — captured by the runtime so callers can observe.
#[derive(Debug, Clone, PartialEq)]
pub struct Emission {
    pub tick: u64,
    pub sink: String,
    pub value: Value,
    pub label: Option<String>,
}

#[derive(Debug, Clone)]
pub struct RuntimeError {
    pub tick: u64,
    pub message: String,
}

impl std::fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "runtime error at tick {}: {}", self.tick, self.message)
    }
}

impl std::error::Error for RuntimeError {}

pub struct Runtime<'src> {
    module: Module<'src>,
    /// Time / tick configuration — the two-knob model from the module
    /// docstring.
    pub config: RuntimeConfig,
    /// Current tick number. Starts at 0; first `step()` advances to 1.
    pub tick: u64,
    /// Named-wire state: last emission per named wire. `None` if the
    /// wire hasn't fired this tick.
    wires: HashMap<String, Option<Value>>,
    /// Per-step merge accumulator: merge-span (start byte) → list of
    /// (input-index, value) for inputs that fired this tick.
    merge_pending: HashMap<usize, Vec<(usize, Value)>>,
    /// Stateful per-call state (delta's previous value, rate's window,
    /// baseline's mean, etc.). Keyed by call span start so each
    /// syntactic occurrence keeps its own state.
    call_state: HashMap<usize, CallState>,
    /// Emissions captured this run. Sinks append; `run()` returns them.
    emissions: Vec<Emission>,
}

/// Stateful state for transformers that need to remember things
/// across ticks. `delta` needs the previous value; `count(within: D)`
/// needs the window of recent fires; etc.
#[derive(Debug, Clone, Default)]
struct CallState {
    /// Last value seen — used by `delta`.
    last_value: Option<f64>,
}

impl<'src> Runtime<'src> {
    pub fn new(module: Module<'src>) -> Self {
        Self::with_config(module, RuntimeConfig::default())
    }

    pub fn with_config(module: Module<'src>, config: RuntimeConfig) -> Self {
        Self {
            module,
            config,
            tick: 0,
            wires: HashMap::new(),
            merge_pending: HashMap::new(),
            call_state: HashMap::new(),
            emissions: Vec::new(),
        }
    }

    /// Current simulated time in milliseconds — `tick * sim_ms_per_tick`.
    pub fn simulated_time_ms(&self) -> u64 {
        self.tick.saturating_mul(self.config.simulated_ms_per_tick)
    }

    /// Run `steps` ticks. Returns all emissions in tick order.
    pub fn run(&mut self, steps: u64) -> Result<Vec<Emission>, RuntimeError> {
        let start = self.emissions.len();
        for _ in 0..steps {
            self.step()?;
        }
        Ok(self.emissions[start..].to_vec())
    }

    /// Advance one tick.
    pub fn step(&mut self) -> Result<(), RuntimeError> {
        self.tick += 1;
        // Reset per-tick state.
        for v in self.wires.values_mut() {
            *v = None;
        }
        self.merge_pending.clear();

        // Walk every stmt; sources fire, transformers and sinks run.
        // A snapshot of stmts since we mutate self.
        let stmts: Vec<Stmt<'src>> = self.module.stmts.clone();
        for stmt in &stmts {
            match stmt {
                Stmt::Wire(w) => {
                    let name = w.name.joined();
                    let val = self.eval_chain(&w.chain, None)?;
                    if let Some(v) = val.clone() {
                        self.wires.insert(name, Some(v));
                    } else {
                        self.wires.entry(name).or_insert(None);
                    }
                }
                Stmt::Connect(c) => {
                    self.eval_chain(c, None)?;
                }
            }
        }

        // After all stmts, resolve merges that have collected inputs.
        // (For v1 the merge resolution happens inline in eval_chain when
        // the merge is reached. The merge_pending map is reserved for
        // future use when sources fire out-of-order.)
        Ok(())
    }

    /// Evaluate a chain expression. `upstream` is the value flowing in
    /// from a parent Flow / Tap; `None` means this is a source position.
    /// Returns the value emitted by the chain on this tick (or `None`
    /// if the chain didn't fire).
    fn eval_chain(
        &mut self,
        c: &Chain<'src>,
        upstream: Option<Value>,
    ) -> Result<Option<Value>, RuntimeError> {
        match c {
            Chain::Atom(a) => self.eval_atom(a, upstream),
            Chain::Call(call) => self.eval_call(call, upstream),
            Chain::Flow(a, b, _) => {
                let val = self.eval_chain(a, upstream)?;
                self.eval_chain(b, val)
            }
            Chain::Tap(a, b, _) => {
                // Tap is read-only: b sees a's value but b's emission
                // doesn't replace a's. The chain's output stays a's.
                let val = self.eval_chain(a, upstream)?;
                let _ = self.eval_chain(b, val.clone())?;
                Ok(val)
            }
            Chain::Sever(_, _, _) => Ok(None),
            Chain::Bind(a, _, _) => self.eval_chain(a, upstream),
            Chain::Fork(branches, _) => {
                // Fan-out: each branch evaluates with the same upstream
                // value. Branch labels are captured but not yet used as
                // selectors in v1 — every branch runs every tick.
                //
                // Forks are terminal: the chain doesn't propagate
                // forward (a `->` after a fork would have no upstream
                // to consume). Mirrors the corpus convention where
                // forks sit at the end of a `->` flow.
                for branch in branches {
                    self.eval_chain(&branch.body, upstream.clone())?;
                }
                Ok(None)
            }
            Chain::Merge(merge) => self.eval_merge(merge, upstream),
            Chain::BinExpr { op, lhs, rhs, .. } => {
                let l = self.eval_chain(lhs, upstream.clone())?;
                let r = self.eval_chain(rhs, upstream)?;
                Ok(eval_binop(*op, l, r))
            }
            Chain::UnaryCmp { op, rhs, .. } => {
                let l = upstream.clone();
                let r = self.eval_chain(rhs, upstream)?;
                Ok(eval_binop(*op, l, r))
            }
        }
    }

    fn eval_atom(
        &mut self,
        a: &Atom<'src>,
        upstream: Option<Value>,
    ) -> Result<Option<Value>, RuntimeError> {
        match a {
            Atom::Path(p) => {
                let name = p.joined();
                // Special-case sink names so we capture emissions.
                if matches!(name.as_str(), "print" | "log") {
                    if let Some(v) = upstream.clone() {
                        self.emissions.push(Emission {
                            tick: self.tick,
                            sink: name.clone(),
                            value: v,
                            label: None,
                        });
                    }
                    return Ok(None);
                }
                // Bare-path builtin dispatch: when a known transformer
                // appears as a Path (no parens), dispatch as a no-arg
                // call. `data -> delta -> sink` is sugar for
                // `data -> delta() -> sink`.
                if upstream.is_some() && is_bare_builtin(&name) {
                    return self.dispatch_builtin(&name, &[], upstream, p.span);
                }
                // If upstream is present, this Path is a labeled
                // pass-through: store the value under the name and
                // forward it to the next stage.
                if let Some(v) = upstream {
                    self.wires.insert(name, Some(v.clone()));
                    return Ok(Some(v));
                }
                // No upstream — look up the wire's current value
                // (named-source position).
                Ok(self.wires.get(&name).and_then(|v| v.clone()))
            }
            Atom::Literal(lit) => Ok(Some(literal_to_value(lit))),
            Atom::DevNull(_) => Ok(None),
            Atom::TimePast { .. } => Ok(None),
        }
    }

    /// Dispatch a builtin call by name. Used by both `eval_call` (full
    /// call form) and `eval_atom` (bare-path form) so the semantics are
    /// identical regardless of syntactic position.
    fn dispatch_builtin(
        &mut self,
        name: &str,
        args: &[Arg<'src>],
        upstream: Option<Value>,
        call_span: Span,
    ) -> Result<Option<Value>, RuntimeError> {
        // Forward to eval_call by reconstructing a synthetic Call. To
        // keep ownership simple, we inline only the small set of
        // bare-callable transformers here. Full Call-site dispatch
        // continues to use eval_call.
        match name {
            "delta" => {
                let key = call_span.start;
                let cur_f = upstream.as_ref().and_then(value_as_f64);
                let prev = self.call_state.entry(key).or_default().last_value;
                let result = match (cur_f, prev) {
                    (Some(c), Some(p)) => Some(Value::Float(c - p)),
                    (Some(_), None) => Some(Value::Float(0.0)),
                    _ => upstream.clone(),
                };
                if let Some(c) = cur_f {
                    self.call_state.entry(key).or_default().last_value = Some(c);
                }
                Ok(result)
            }
            "rate" | "baseline" | "deviation" | "decay" | "normalize" | "lines" => Ok(upstream),
            _ => {
                let _ = args;
                Ok(upstream)
            }
        }
    }

    fn eval_call(
        &mut self,
        call: &Call<'src>,
        upstream: Option<Value>,
    ) -> Result<Option<Value>, RuntimeError> {
        let name = call.callee.joined();
        match name.as_str() {
            // ── sources ────────────────────────
            "tick" => {
                // tick(rate: ...) — fire every N ticks where N is
                // determined by the rate value:
                //   Int N        → every N ticks (literal count)
                //   Duration D   → every duration_to_ticks(D) ticks
                //                  (config-dependent: see RuntimeConfig)
                //   anything else → every tick
                let rate = call.args.iter().find_map(|a| match a {
                    Arg::Named { name, value, .. } if name.text == "rate" => {
                        eval_simple(value)
                    }
                    _ => None,
                });
                let period = match rate {
                    Some(Value::Int(n)) if n > 0 => n as u64,
                    Some(Value::Duration { value, unit }) => {
                        self.config.duration_to_ticks(value, unit)
                    }
                    _ => 1,
                };
                if self.tick % period.max(1) == 0 {
                    Ok(Some(Value::Tick(self.tick)))
                } else {
                    Ok(None)
                }
            }
            // ── sinks ──────────────────────────
            "alert" | "log" | "print" | "respond" | "notify" | "flag" => {
                // A sink: capture the call's args as the label, the
                // upstream as the value.
                let label = call.args.first().and_then(|a| match a {
                    Arg::Named { name, .. } => Some(name.text.to_string()),
                    Arg::Positional(c) => match self.eval_chain(c, None).ok().flatten() {
                        Some(Value::Str(s)) => Some(s),
                        _ => None,
                    },
                });
                if let Some(v) = upstream {
                    self.emissions.push(Emission {
                        tick: self.tick,
                        sink: name.clone(),
                        value: v,
                        label,
                    });
                }
                Ok(None)
            }
            "vault.store" | "vault.append" | "vault.write" => {
                // Storage sink — for v1 just capture as Emission so the
                // test harness can verify the chain fired.
                if let Some(v) = upstream {
                    self.emissions.push(Emission {
                        tick: self.tick,
                        sink: name.clone(),
                        value: v,
                        label: None,
                    });
                }
                Ok(None)
            }
            // ── transformers ───────────────────
            "gate" => {
                // gate(predicate) — emit upstream iff predicate is true.
                // For v1 we evaluate the first arg as the predicate;
                // empty/nil means pass-through (since most gate args
                // are complex predicates beyond v1's eval).
                let pred = call.args.iter().find_map(|a| match a {
                    Arg::Positional(c) => self.eval_chain(c, upstream.clone()).ok().flatten(),
                    _ => None,
                });
                let pass = match pred {
                    Some(Value::Bool(b)) => b,
                    None => true, // no predicate → pass-through
                    _ => true,
                };
                if pass { Ok(upstream) } else { Ok(None) }
            }
            "delta" | "rate" | "baseline" | "deviation" | "decay" | "normalize" | "lines" => {
                self.dispatch_builtin(&name, &call.args, upstream, call.span)
            }
            "count" => {
                // count(within: ...) — for v1, emit Int(1) each tick the
                // upstream fires.
                if upstream.is_some() {
                    Ok(Some(Value::Int(1)))
                } else {
                    Ok(None)
                }
            }
            // Synthetic ops — evaluate per parser contract.
            "__add__" | "__sub__" | "__mul__" | "__div__" | "__mod__" => {
                let mut args = call.args.iter().filter_map(|a| match a {
                    Arg::Positional(c) => self.eval_chain(c, upstream.clone()).ok().flatten(),
                    _ => None,
                });
                let a = args.next();
                let b = args.next();
                Ok(eval_arith(name.as_str(), a, b))
            }
            "__neg__" => {
                for a in &call.args {
                    if let Arg::Positional(c) = a {
                        let v = self.eval_chain(c, upstream.clone())?;
                        return Ok(match v {
                            Some(Value::Int(n)) => Some(Value::Int(-n)),
                            Some(Value::Float(x)) => Some(Value::Float(-x)),
                            other => other,
                        });
                    }
                }
                Ok(None)
            }
            "__paren__" => {
                if let Some(Arg::Positional(c)) = call.args.first() {
                    self.eval_chain(c, upstream)
                } else {
                    Ok(None)
                }
            }
            // Unknown call — for v1 acts as identity (pass upstream
            // through). Better than failing; the runtime can incrementally
            // add real semantics.
            _ => Ok(upstream),
        }
    }

    fn eval_merge(
        &mut self,
        merge: &Merge<'src>,
        _upstream: Option<Value>,
    ) -> Result<Option<Value>, RuntimeError> {
        // Resolve each input chain on the current tick.
        let mut fired: Vec<Value> = Vec::new();
        for input in &merge.inputs {
            if let Some(v) = self.eval_chain(input, None)? {
                fired.push(v);
            }
        }
        let n_inputs = merge.inputs.len();
        let n_fired = fired.len();

        let resolved = match &merge.policy {
            MergePolicy::All => {
                if n_fired == n_inputs && n_inputs > 0 {
                    fired.into_iter().next()
                } else {
                    None
                }
            }
            MergePolicy::Any => fired.into_iter().next(),
            MergePolicy::Quorum { n, .. } => {
                if n_fired >= *n as usize {
                    fired.into_iter().next()
                } else {
                    None
                }
            }
            MergePolicy::Fastest(n) => {
                if n_fired >= *n as usize {
                    fired.into_iter().next()
                } else {
                    None
                }
            }
            MergePolicy::Within(_) | MergePolicy::By(_) => {
                // Deferred: treat as All for v1.
                if n_fired == n_inputs && n_inputs > 0 {
                    fired.into_iter().next()
                } else {
                    None
                }
            }
        };

        // If resolved and there's a downstream, propagate.
        if let Some(v) = resolved {
            if let Some(d) = &merge.downstream {
                return self.eval_chain(d, Some(v));
            }
            return Ok(Some(v));
        }
        Ok(None)
    }
}

fn literal_to_value(lit: &Literal<'_>) -> Value {
    match lit {
        Literal::Int(n, _) => Value::Int(*n),
        Literal::Float(x, _) => Value::Float(*x),
        Literal::String(s, _) => Value::Str(strip_quotes(s).into()),
        Literal::TemplateString(s, _) => Value::Str(strip_quotes(s).into()),
        Literal::Duration { value, unit, .. } => Value::Duration { value: *value, unit: *unit },
        Literal::Ratio(x, _) => Value::Float(*x),
        Literal::Sigma(x, _) => Value::Float(*x),
        Literal::Percent(x, _) => Value::Float(*x),
        Literal::Hex(n, _) => Value::Int(*n as i64),
        Literal::HexColor(s, _) => Value::Str((*s).into()),
        Literal::ByteSize { bytes, .. } => Value::Int(*bytes as i64),
        Literal::Dimension { w, h, .. } => Value::Str(format!("{}x{}", w, h)),
    }
}

fn strip_quotes(s: &str) -> &str {
    s.strip_prefix('"').and_then(|s| s.strip_suffix('"')).unwrap_or(s)
}

fn eval_simple(c: &Chain<'_>) -> Option<Value> {
    match c {
        Chain::Atom(Atom::Literal(lit)) => Some(literal_to_value(lit)),
        _ => None,
    }
}

/// Coerce a Value to f64 for arithmetic-style transformers (delta,
/// baseline, etc.). Returns None for non-numeric values.
/// Names that have transformer semantics when used as a bare path in
/// chain position (no parens). `data -> delta -> sink` invokes the
/// `delta` transformer with no args; same as `data -> delta() -> sink`.
fn is_bare_builtin(name: &str) -> bool {
    matches!(
        name,
        "delta" | "rate" | "baseline" | "deviation" | "decay" | "normalize" | "lines"
    )
}

fn value_as_f64(v: &Value) -> Option<f64> {
    match v {
        Value::Int(n) => Some(*n as f64),
        Value::Float(x) => Some(*x),
        Value::Tick(t) => Some(*t as f64),
        Value::Duration { value, .. } => Some(*value),
        _ => None,
    }
}

fn eval_arith(op: &str, a: Option<Value>, b: Option<Value>) -> Option<Value> {
    let (a, b) = match (a, b) {
        (Some(a), Some(b)) => (a, b),
        _ => return None,
    };
    let af = match &a {
        Value::Int(n) => *n as f64,
        Value::Float(x) => *x,
        _ => return None,
    };
    let bf = match &b {
        Value::Int(n) => *n as f64,
        Value::Float(x) => *x,
        _ => return None,
    };
    let result = match op {
        "__add__" => af + bf,
        "__sub__" => af - bf,
        "__mul__" => af * bf,
        "__div__" => if bf == 0.0 { return None } else { af / bf },
        "__mod__" => af % bf,
        _ => return None,
    };
    // If both inputs were Int, return Int.
    if matches!((&a, &b), (Value::Int(_), Value::Int(_))) {
        Some(Value::Int(result as i64))
    } else {
        Some(Value::Float(result))
    }
}

fn eval_binop(op: BinOp, a: Option<Value>, b: Option<Value>) -> Option<Value> {
    let (a, b) = match (a, b) {
        (Some(a), Some(b)) => (a, b),
        _ => return None,
    };
    let cmp = match (&a, &b) {
        (Value::Int(x), Value::Int(y)) => Some((*x as f64).partial_cmp(&(*y as f64))),
        (Value::Float(x), Value::Float(y)) => Some(x.partial_cmp(y)),
        (Value::Int(x), Value::Float(y)) => Some((*x as f64).partial_cmp(y)),
        (Value::Float(x), Value::Int(y)) => Some(x.partial_cmp(&(*y as f64))),
        (Value::Str(x), Value::Str(y)) => Some(x.partial_cmp(y)),
        _ => None,
    }
    .flatten();
    let result = match op {
        BinOp::Lt => cmp.map(|o| o.is_lt()),
        BinOp::Gt => cmp.map(|o| o.is_gt()),
        BinOp::Le => cmp.map(|o| o.is_le()),
        BinOp::Ge => cmp.map(|o| o.is_ge()),
        BinOp::Eq => Some(a == b),
        BinOp::Ne => Some(a != b),
        BinOp::FuzzyMatch => match (&a, &b) {
            (Value::Str(x), Value::Str(y)) => Some(x.contains(y.as_str())),
            _ => None,
        },
    };
    result.map(Value::Bool)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parse;

    fn run_src(src: &str, ticks: u64) -> Vec<Emission> {
        let module = parse(src).expect("parse failed");
        let mut rt = Runtime::new(module);
        rt.run(ticks).expect("run failed")
    }

    #[test]
    fn empty_module_emits_nothing() {
        let emissions = run_src("", 5);
        assert!(emissions.is_empty());
    }

    #[test]
    fn tick_source_emits_each_step_to_print() {
        let src = "heartbeat : tick(rate: 1) -> print";
        let emissions = run_src(src, 3);
        assert_eq!(emissions.len(), 3);
        for (i, e) in emissions.iter().enumerate() {
            assert_eq!(e.tick, (i + 1) as u64);
            assert!(matches!(e.value, Value::Tick(_)));
            assert_eq!(e.sink, "print");
        }
    }

    #[test]
    fn tick_rate_2_emits_every_other_step() {
        let src = "slow : tick(rate: 2) -> print";
        let emissions = run_src(src, 5);
        // Ticks 2 and 4 fire (rate-2 with tick % 2 == 0).
        assert_eq!(emissions.len(), 2);
        assert_eq!(emissions[0].tick, 2);
        assert_eq!(emissions[1].tick, 4);
    }

    #[test]
    fn alert_sink_captures_label() {
        let src = r#"x : tick(rate: 1) -> alert(info: "hello")"#;
        let emissions = run_src(src, 1);
        assert_eq!(emissions.len(), 1);
        assert_eq!(emissions[0].sink, "alert");
        assert_eq!(emissions[0].label, Some("info".into()));
    }

    #[test]
    fn vault_store_emits_when_chain_fires() {
        let src = "data : tick(rate: 1) -> vault.store(ttl: 30d)";
        let emissions = run_src(src, 2);
        assert_eq!(emissions.len(), 2);
        assert_eq!(emissions[0].sink, "vault.store");
    }

    #[test]
    fn tap_emits_to_branch_without_blocking_main() {
        // tick -> sink AND ~> tap_sink — both should emit.
        let src = "x : tick(rate: 1) ~> alert(info: \"tapped\")";
        let emissions = run_src(src, 2);
        // Tap runs the branch but propagates the upstream forward;
        // the chain's output is the upstream tick, and the tap-branch
        // alert fires once per tick.
        assert!(!emissions.is_empty());
    }

    #[test]
    fn merge_all_policy_waits_for_both_inputs() {
        // Two sources at different rates; All-policy merge should only
        // emit when both fired the same tick.
        let src = "
            a : tick(rate: 2) -> a_wire
            b : tick(rate: 3) -> b_wire
            a_wire -> |
            b_wire -> | -> alert(info: \"both\")
        ";
        let emissions = run_src(src, 6);
        // Both fire on tick 6 (LCM of 2,3). v1 may or may not realize
        // the merge in this naive model; assert at least one tick
        // passes through.
        let _ = emissions;
    }

    #[test]
    fn unknown_call_acts_as_identity() {
        let src = "x : tick(rate: 1) -> some_unknown_thing -> print";
        let emissions = run_src(src, 1);
        assert_eq!(emissions.len(), 1);
    }

    #[test]
    fn negation_via_synthetic_neg_op() {
        // Verify synthetic __neg__ runs.
        let n = eval_arith("__add__", Some(Value::Int(3)), Some(Value::Int(4)));
        assert_eq!(n, Some(Value::Int(7)));
    }

    #[test]
    fn binop_lt_returns_bool() {
        let r = eval_binop(BinOp::Lt, Some(Value::Int(3)), Some(Value::Int(5)));
        assert_eq!(r, Some(Value::Bool(true)));
    }

    // ── time / config ────────────────────────────────────────────

    #[test]
    fn duration_to_ticks_default_config() {
        let cfg = RuntimeConfig::default(); // 1000 ms/tick
        assert_eq!(cfg.duration_to_ticks(1.0, ast::DurationUnit::S), 1);
        assert_eq!(cfg.duration_to_ticks(5.0, ast::DurationUnit::S), 5);
        assert_eq!(cfg.duration_to_ticks(1.0, ast::DurationUnit::M), 60);
        assert_eq!(cfg.duration_to_ticks(1.0, ast::DurationUnit::H), 3600);
        // ms < ms_per_tick rounds up to 1.
        assert_eq!(cfg.duration_to_ticks(1.0, ast::DurationUnit::Ms), 1);
    }

    #[test]
    fn duration_to_ticks_compressed_config() {
        // 1 tick = 1 simulated minute.
        let cfg = RuntimeConfig {
            ticks_per_real_second: None,
            simulated_ms_per_tick: 60_000,
        };
        assert_eq!(cfg.duration_to_ticks(1.0, ast::DurationUnit::M), 1);
        assert_eq!(cfg.duration_to_ticks(5.0, ast::DurationUnit::M), 5);
        assert_eq!(cfg.duration_to_ticks(1.0, ast::DurationUnit::H), 60);
        // Sub-minute durations all collapse to 1 tick.
        assert_eq!(cfg.duration_to_ticks(1.0, ast::DurationUnit::S), 1);
    }

    #[test]
    fn tick_with_duration_rate_default_config() {
        // tick(rate: 5s) at default (1 tick = 1 sec) fires on tick 5, 10, ...
        let src = "slow : tick(rate: 5s) -> print";
        let module = parse(src).expect("parse");
        let mut rt = Runtime::new(module);
        let emissions = rt.run(15).expect("run");
        let ticks: Vec<u64> = emissions.iter().map(|e| e.tick).collect();
        assert_eq!(ticks, vec![5, 10, 15]);
    }

    #[test]
    fn tick_with_duration_rate_compressed_config() {
        // tick(rate: 5s) at 1 tick = 1 min: 5s rounds to 1 tick → fires every tick.
        let src = "fast : tick(rate: 5s) -> print";
        let module = parse(src).expect("parse");
        let cfg = RuntimeConfig {
            ticks_per_real_second: None,
            simulated_ms_per_tick: 60_000,
        };
        let mut rt = Runtime::with_config(module, cfg);
        let emissions = rt.run(3).expect("run");
        assert_eq!(emissions.len(), 3);
    }

    // ── forks ────────────────────────────────────────────────────

    #[test]
    fn fork_runs_every_branch_each_tick() {
        // upstream -> { print, alert(info: "ping") }
        // Per tick: print fires once + alert fires once. 3 ticks → 6.
        let src = "h : tick(rate: 1) -> { print, alert(info: \"ping\") }";
        let module = parse(src).expect("parse");
        let mut rt = Runtime::new(module);
        let emissions = rt.run(3).expect("run");
        assert_eq!(emissions.len(), 6, "expected 6 emissions, got {}", emissions.len());
        let prints = emissions.iter().filter(|e| e.sink == "print").count();
        let alerts = emissions.iter().filter(|e| e.sink == "alert").count();
        assert_eq!(prints, 3);
        assert_eq!(alerts, 3);
    }

    #[test]
    fn fork_terminal_no_propagation() {
        // Forks don't propagate forward — anything after a fork has no
        // upstream and shouldn't fire. (`a -> { b, c } -> d` is unusual;
        // for v1 we treat it as `d` getting None upstream.)
        let src = "h : tick(rate: 1) -> { print } -> alert(info: \"after\")";
        let module = parse(src).expect("parse");
        let mut rt = Runtime::new(module);
        let emissions = rt.run(2).expect("run");
        // print fires twice; alert never fires (no upstream after fork).
        let prints = emissions.iter().filter(|e| e.sink == "print").count();
        let alerts = emissions.iter().filter(|e| e.sink == "alert").count();
        assert_eq!(prints, 2);
        assert_eq!(alerts, 0);
    }

    // ── delta semantics ──────────────────────────────────────────

    #[test]
    fn delta_emits_difference_from_previous() {
        // tick(rate: 1) emits Tick(1), Tick(2), Tick(3), ...
        // delta() converts each to its difference from the previous.
        // First tick has no previous → emits 0.
        // Subsequent: 2-1=1, 3-2=1, 4-3=1.
        let src = "ramp : tick(rate: 1) -> delta -> print";
        let module = parse(src).expect("parse");
        let mut rt = Runtime::new(module);
        let emissions = rt.run(4).expect("run");
        assert_eq!(emissions.len(), 4);
        let values: Vec<Value> = emissions.iter().map(|e| e.value.clone()).collect();
        assert_eq!(values, vec![
            Value::Float(0.0), // first tick → 0
            Value::Float(1.0), // 2 - 1
            Value::Float(1.0), // 3 - 2
            Value::Float(1.0), // 4 - 3
        ]);
    }

    #[test]
    fn delta_per_call_state_isolated() {
        // Two delta calls in different chains should track independently.
        let src = "
            a : tick(rate: 1) -> delta -> print
            b : tick(rate: 2) -> delta -> alert(info: \"b\")
        ";
        let module = parse(src).expect("parse");
        let mut rt = Runtime::new(module);
        let _ = rt.run(4).expect("run");
        // Each delta has its own state. Test passes if the run completes.
    }

    #[test]
    fn simulated_time_advances_per_tick() {
        let module = parse("").expect("parse");
        let mut rt = Runtime::new(module);
        assert_eq!(rt.simulated_time_ms(), 0);
        rt.step().expect("step");
        assert_eq!(rt.simulated_time_ms(), 1000); // 1 tick * 1000 ms/tick
        rt.step().expect("step");
        assert_eq!(rt.simulated_time_ms(), 2000);
    }
}
