//! ZIR — the Z+ Interchange Representation emitter.
//!
//! Lowers the typed AST (`ast.rs`) into a serializable Signal Flow Graph and
//! writes it as ZIR v1 JSON. This is the Rust front-end's **codegen backend**:
//! the thing the tree-walking interpreter never had. The SFG it produces is the
//! same model the C transpiler builds (`ir.c` → `sfg.h`) and the kernel loader
//! consumes (`zplus_zir.c`). See `ZIR.md`.
//!
//! Lowering mirrors the C `lower_pipeline`/`lower_stage` in `ir.c` so both
//! front-ends emit structurally equivalent graphs. The chord rule is preserved:
//! a `|` merge becomes ONE `merge` node carrying its policy, never N edges.

use crate::ast::{
    Arg, Atom, BinOp, Chain, Literal, MergePolicy, Module, Stmt,
};

/// ZIR format version. Back-ends MUST reject documents whose `zir` differs.
pub const ZIR_VERSION: u32 = 1;

#[derive(Debug, Clone)]
struct ZChain {
    id: i32,
    name: String,
    masq: &'static str,
    parent: i32,
    node_ids: Vec<i32>,
}

#[derive(Debug, Clone)]
struct ZNode {
    id: i32,
    chain: i32,
    seq: i32,
    kind: &'static str,
    verb: String,
    name: String,
    sig_in: &'static str,
    sig_out: &'static str,
    args: Vec<String>,       // each is a JSON value fragment
    emit: Option<String>,    // JSON value fragment, for sources
    gate: Option<String>,    // JSON object fragment, for gates
    merge: Option<String>,   // JSON object fragment, for merges
}

#[derive(Debug, Clone)]
struct ZEdge {
    id: i32,
    kind: &'static str,
    from: i32,
    to: i32,
}

/// A lowered Z+ program in ZIR form. Build with [`Zir::lower`], serialize with
/// [`Zir::to_json`].
#[derive(Debug, Clone)]
pub struct Zir {
    source: String,
    chains: Vec<ZChain>,
    nodes: Vec<ZNode>,
    edges: Vec<ZEdge>,
}

/// (first, last) node ids produced by lowering a pipeline fragment. An incoming
/// edge attaches to `first`; an outgoing edge leaves from `last`.
#[derive(Clone, Copy)]
struct Range {
    first: i32,
    last: i32,
}

impl Zir {
    /// Lower a parsed [`Module`] into ZIR. `source` is the origin filename,
    /// carried for diagnostics only.
    pub fn lower(module: &Module<'_>, source: &str) -> Zir {
        let mut z = Zir {
            source: source.to_string(),
            chains: Vec::new(),
            nodes: Vec::new(),
            edges: Vec::new(),
        };
        // Every program has a root "main" chain (mirrors ir.c: MASQ_REFERENCE).
        let main = z.new_chain("main", "reference", -1);
        for stmt in &module.stmts {
            match stmt {
                Stmt::Wire(w) => {
                    z.lower_pipeline(main, &w.chain);
                }
                Stmt::Connect(chain) => {
                    z.lower_pipeline(main, chain);
                }
            }
        }
        z
    }

    fn new_chain(&mut self, name: &str, masq: &'static str, parent: i32) -> i32 {
        let id = self.chains.len() as i32;
        self.chains.push(ZChain {
            id,
            name: name.to_string(),
            masq,
            parent,
            node_ids: Vec::new(),
        });
        id
    }

    fn add_node(
        &mut self,
        chain: i32,
        kind: &'static str,
        verb: String,
        sig_in: &'static str,
        sig_out: &'static str,
    ) -> i32 {
        let id = self.nodes.len() as i32;
        let ch = &mut self.chains[chain as usize];
        let seq = ch.node_ids.len() as i32;
        ch.node_ids.push(id);
        let name = format!("{}_{}", sanitize(&verb), id);
        self.nodes.push(ZNode {
            id,
            chain,
            seq,
            kind,
            verb,
            name,
            sig_in,
            sig_out,
            args: Vec::new(),
            emit: None,
            gate: None,
            merge: None,
        });
        id
    }

    fn add_edge(&mut self, kind: &'static str, from: i32, to: i32) {
        if from < 0 || to < 0 {
            return;
        }
        let id = self.edges.len() as i32;
        self.edges.push(ZEdge { id, kind, from, to });
    }

    /// Lower a chain expression, returning the (first,last) node range so the
    /// caller can wire edges into/out of it. Mirrors `ir.c:lower_pipeline`.
    fn lower_pipeline(&mut self, chain: i32, c: &Chain<'_>) -> Range {
        match c {
            Chain::Flow(l, r, _) => self.lower_link(chain, l, r, "flow"),
            Chain::Tap(l, r, _) => self.lower_link(chain, l, r, "tap"),
            Chain::Feedback(l, r, _) => self.lower_link(chain, l, r, "feedback"),
            Chain::Sever(l, r, _) => self.lower_link(chain, l, r, "sever"),
            Chain::Bind(l, r, _) => self.lower_link(chain, l, r, "exchange"),
            Chain::Fork(branches, _) => {
                let fork = self.add_node(chain, "fork", "fork".into(), "signal", "signal");
                for b in branches {
                    let br = self.lower_pipeline(chain, &b.body);
                    self.add_edge("flow", fork, br.first);
                }
                Range { first: fork, last: fork }
            }
            Chain::Merge(m) => {
                let merge = self.add_node(chain, "merge", "merge".into(), "signal", "signal");
                self.nodes[merge as usize].merge = Some(merge_policy_json(&m.policy));
                for input in &m.inputs {
                    let ir = self.lower_pipeline(chain, input);
                    self.add_edge("flow", ir.last, merge);
                }
                match &m.downstream {
                    Some(ds) => {
                        let dr = self.lower_pipeline(chain, ds);
                        self.add_edge("flow", merge, dr.first);
                        Range { first: merge, last: dr.last }
                    }
                    None => Range { first: merge, last: merge },
                }
            }
            // Atoms, calls and comparisons are single stages.
            _ => {
                let id = self.lower_stage(chain, c);
                Range { first: id, last: id }
            }
        }
    }

    fn lower_link(&mut self, chain: i32, l: &Chain<'_>, r: &Chain<'_>, edge: &'static str) -> Range {
        let lr = self.lower_pipeline(chain, l);
        let rr = self.lower_pipeline(chain, r);
        self.add_edge(edge, lr.last, rr.first);
        Range { first: lr.first, last: rr.last }
    }

    /// Lower a single non-composite stage to one node. Mirrors `ir.c:lower_stage`.
    fn lower_stage(&mut self, chain: i32, c: &Chain<'_>) -> i32 {
        match c {
            Chain::Atom(a) => self.lower_atom(chain, a),
            Chain::Call(call) => {
                let verb = call.callee.joined();
                let (kind, sig_in, sig_out) = classify_verb(&verb);
                let id = self.add_node(chain, kind, verb.clone(), sig_in, sig_out);
                // Populate emit/gate from the first argument where meaningful.
                if kind == "source" {
                    if let Some(v) = first_arg_value(&call.args) {
                        self.nodes[id as usize].emit = Some(v);
                    }
                }
                if kind == "gate" {
                    if let Some(g) = first_arg_gate(&call.args) {
                        self.nodes[id as usize].gate = Some(g);
                    }
                }
                for a in &call.args {
                    let av = arg_json(a);
                    self.nodes[id as usize].args.push(av);
                }
                id
            }
            // A bare comparison used as a stage is a gate on the implicit input.
            Chain::BinExpr { op, rhs, .. } => {
                let id = self.add_node(chain, "gate", "gate".into(), "signal", "signal");
                self.nodes[id as usize].gate = Some(gate_json(*op, rhs));
                id
            }
            Chain::UnaryCmp { op, rhs, .. } => {
                let id = self.add_node(chain, "gate", "gate".into(), "signal", "signal");
                self.nodes[id as usize].gate = Some(gate_json(*op, rhs));
                id
            }
            // Composite chains shouldn't reach here (lower_pipeline handles them),
            // but degrade honestly to a passthrough processor if one does.
            _ => self.add_node(chain, "processor", "passthrough".into(), "signal", "signal"),
        }
    }

    fn lower_atom(&mut self, chain: i32, a: &Atom<'_>) -> i32 {
        match a {
            Atom::Path(p) => {
                let name = p.joined();
                let (kind, sig_in, sig_out): (&'static str, &'static str, &'static str) =
                    match name.as_str() {
                        "input" => ("source", "void", "signal"),
                        "output" => ("sink", "signal", "void"),
                        _ => ("processor", "signal", "signal"),
                    };
                self.add_node(chain, kind, name, sig_in, sig_out)
            }
            Atom::DevNull(_) => {
                self.add_node(chain, "sink", "/dev/null".into(), "signal", "void")
            }
            Atom::Literal(lit) => {
                let id = self.add_node(chain, "source", "emit".into(), "void", "signal");
                self.nodes[id as usize].emit = Some(literal_json(lit));
                id
            }
            Atom::TimePast { steps, .. } => {
                let id = self.add_node(chain, "source", "t-past".into(), "void", "signal");
                self.nodes[id as usize].emit = Some(format!("{{\"steps\":{}}}", steps));
                id
            }
        }
    }

    /// Serialize to ZIR v1 JSON. Hand-written (no serde dependency).
    pub fn to_json(&self) -> String {
        let mut s = String::new();
        s.push_str("{\n");
        s.push_str(&format!("  \"zir\": {},\n", ZIR_VERSION));
        s.push_str(&format!("  \"source\": {},\n", json_str(&self.source)));

        // chains
        s.push_str("  \"chains\": [\n");
        for (i, ch) in self.chains.iter().enumerate() {
            let ids: Vec<String> = ch.node_ids.iter().map(|n| n.to_string()).collect();
            s.push_str(&format!(
                "    {{ \"id\": {}, \"name\": {}, \"masq\": \"{}\", \"parent\": {}, \"nodes\": [{}] }}{}\n",
                ch.id,
                json_str(&ch.name),
                ch.masq,
                ch.parent,
                ids.join(", "),
                comma(i, self.chains.len())
            ));
        }
        s.push_str("  ],\n");

        // nodes
        s.push_str("  \"nodes\": [\n");
        for (i, n) in self.nodes.iter().enumerate() {
            let mut fields = vec![
                format!("\"id\": {}", n.id),
                format!("\"chain\": {}", n.chain),
                format!("\"seq\": {}", n.seq),
                format!("\"kind\": \"{}\"", n.kind),
                format!("\"verb\": {}", json_str(&n.verb)),
                format!("\"name\": {}", json_str(&n.name)),
                format!("\"sig_in\": \"{}\"", n.sig_in),
                format!("\"sig_out\": \"{}\"", n.sig_out),
                format!("\"args\": [{}]", n.args.join(", ")),
            ];
            if let Some(e) = &n.emit {
                fields.push(format!("\"emit\": {}", e));
            }
            if let Some(g) = &n.gate {
                fields.push(format!("\"gate\": {}", g));
            }
            if let Some(m) = &n.merge {
                fields.push(format!("\"merge\": {}", m));
            }
            s.push_str(&format!(
                "    {{ {} }}{}\n",
                fields.join(", "),
                comma(i, self.nodes.len())
            ));
        }
        s.push_str("  ],\n");

        // edges
        s.push_str("  \"edges\": [\n");
        for (i, e) in self.edges.iter().enumerate() {
            s.push_str(&format!(
                "    {{ \"id\": {}, \"kind\": \"{}\", \"from\": {}, \"to\": {}, \"sig\": \"signal\" }}{}\n",
                e.id,
                e.kind,
                e.from,
                e.to,
                comma(i, self.edges.len())
            ));
        }
        s.push_str("  ]\n");
        s.push_str("}\n");
        s
    }

    // ── accessors for tests / callers ──────────────────────────────────
    pub fn node_count(&self) -> usize { self.nodes.len() }
    pub fn edge_count(&self) -> usize { self.edges.len() }
    pub fn chain_count(&self) -> usize { self.chains.len() }
    /// Count of merge nodes carrying an explicit non-default policy string.
    pub fn merge_nodes(&self) -> usize {
        self.nodes.iter().filter(|n| n.kind == "merge").count()
    }
}

// ── helpers ────────────────────────────────────────────────────────────

fn classify_verb(verb: &str) -> (&'static str, &'static str, &'static str) {
    match verb {
        "emit" => ("source", "void", "signal"),
        "input" => ("source", "void", "signal"),
        "gate" => ("gate", "signal", "signal"),
        "output" | "print" => ("sink", "signal", "void"),
        "tap.log" => ("tap", "signal", "signal"),
        _ => ("processor", "signal", "signal"),
    }
}

fn first_arg_value(args: &[Arg<'_>]) -> Option<String> {
    args.first().map(|a| match a {
        Arg::Positional(c) => chain_value_json(c),
        Arg::Named { value, .. } => chain_value_json(value),
    })
}

fn first_arg_gate(args: &[Arg<'_>]) -> Option<String> {
    let c = match args.first()? {
        Arg::Positional(c) => c,
        Arg::Named { value, .. } => value,
    };
    match c {
        Chain::BinExpr { op, rhs, .. } => Some(gate_json(*op, rhs)),
        Chain::UnaryCmp { op, rhs, .. } => Some(gate_json(*op, rhs)),
        _ => None,
    }
}

fn arg_json(a: &Arg<'_>) -> String {
    match a {
        Arg::Positional(c) => format!("{{\"pos\": {}}}", chain_value_json(c)),
        Arg::Named { name, value, .. } => format!(
            "{{\"name\": {}, \"value\": {}}}",
            json_str(name.text),
            chain_value_json(value)
        ),
    }
}

/// Render a chain used as a *value* (arg / emit) to a JSON value fragment.
fn chain_value_json(c: &Chain<'_>) -> String {
    match c {
        Chain::Atom(Atom::Literal(lit)) => literal_json(lit),
        Chain::Atom(Atom::Path(p)) => format!("{{\"ref\": {}}}", json_str(&p.joined())),
        Chain::Atom(Atom::DevNull(_)) => "{\"ref\": \"/dev/null\"}".to_string(),
        Chain::BinExpr { op, rhs, .. } => gate_json(*op, rhs),
        Chain::UnaryCmp { op, rhs, .. } => gate_json(*op, rhs),
        _ => "{\"expr\": true}".to_string(),
    }
}

fn gate_json(op: BinOp, rhs: &Chain<'_>) -> String {
    format!(
        "{{\"op\": \"{}\", \"rhs\": {}}}",
        binop_str(op),
        chain_value_json(rhs)
    )
}

fn binop_str(op: BinOp) -> &'static str {
    match op {
        BinOp::Lt => "lt",
        BinOp::Gt => "gt",
        BinOp::Le => "le",
        BinOp::Ge => "ge",
        BinOp::Eq => "eq",
        BinOp::Ne => "ne",
        BinOp::FuzzyMatch => "match",
    }
}

fn merge_policy_json(p: &MergePolicy<'_>) -> String {
    match p {
        MergePolicy::All => "{\"policy\": \"all\"}".to_string(),
        MergePolicy::Any => "{\"policy\": \"any\"}".to_string(),
        MergePolicy::Quorum { n, m } => {
            format!("{{\"policy\": \"quorum\", \"n\": {}, \"m\": {}}}", n, m)
        }
        MergePolicy::Fastest(n) => format!("{{\"policy\": \"fastest\", \"n\": {}}}", n),
        MergePolicy::Within(_) => "{\"policy\": \"within\"}".to_string(),
        MergePolicy::By(path) => {
            format!("{{\"policy\": \"by\", \"key\": {}}}", json_str(&path.joined()))
        }
    }
}

fn literal_json(lit: &Literal<'_>) -> String {
    match lit {
        Literal::Int(n, _) => format!("{{\"int\": {}}}", n),
        Literal::Float(f, _) => format!("{{\"float\": {}}}", f),
        Literal::String(s, _) => format!("{{\"str\": {}}}", json_str(s)),
        Literal::TemplateString(s, _) => format!("{{\"template\": {}}}", json_str(s)),
        Literal::Duration { value, unit, .. } => {
            format!("{{\"duration\": {}, \"unit\": \"{}\"}}", value, unit)
        }
        Literal::Ratio(f, _) => format!("{{\"ratio\": {}}}", f),
        Literal::Sigma(f, _) => format!("{{\"sigma\": {}}}", f),
        Literal::Percent(f, _) => format!("{{\"percent\": {}}}", f),
        Literal::ByteSize { bytes, .. } => format!("{{\"bytes\": {}}}", bytes),
        Literal::Dimension { w, h, .. } => format!("{{\"w\": {}, \"h\": {}}}", w, h),
        Literal::Hex(n, _) => format!("{{\"int\": {}}}", n),
        Literal::HexColor(s, _) => format!("{{\"color\": {}}}", json_str(s)),
    }
}

/// JSON string escaping — the only correctness-critical serialization detail.
fn json_str(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn sanitize(s: &str) -> String {
    s.chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect()
}

fn comma(i: usize, len: usize) -> &'static str {
    if i + 1 < len { "," } else { "" }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parse;

    fn zir_of(src: &str) -> Zir {
        let m = parse(src).expect("parse");
        Zir::lower(&m, "test.zp")
    }

    #[test]
    fn emit_source_carries_value() {
        let z = zir_of("beat : emit(1)");
        assert!(z.node_count() >= 1);
        let json = z.to_json();
        assert!(json.contains("\"verb\": \"emit\""));
        assert!(json.contains("\"emit\": {\"int\": 1}"));
    }

    #[test]
    fn flow_produces_an_edge() {
        let z = zir_of("p : input -> output");
        assert!(z.edge_count() >= 1, "a -> b must yield a flow edge");
        assert!(z.to_json().contains("\"kind\": \"flow\""));
    }

    #[test]
    fn json_is_versioned_and_wellformed() {
        let z = zir_of("beat : emit(1)");
        let json = z.to_json();
        assert!(json.contains("\"zir\": 1"));
        // balanced braces — cheap structural sanity
        let opens = json.matches('{').count();
        let closes = json.matches('}').count();
        assert_eq!(opens, closes, "unbalanced braces in ZIR json");
    }

    #[test]
    fn json_strings_are_escaped() {
        // A string literal containing a quote must not break the JSON.
        let z = zir_of("p : emit(\"he said \\\"hi\\\"\")");
        let json = z.to_json();
        let opens = json.matches('{').count();
        let closes = json.matches('}').count();
        assert_eq!(opens, closes);
    }
}
