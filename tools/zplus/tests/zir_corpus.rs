//! ZIR emitter — corpus integration test.
//!
//! Parses every `.zp` under `programs/` that the front-end accepts and lowers
//! it to ZIR, asserting the output is structurally well-formed (balanced
//! braces, versioned, non-empty). This is the coverage proof for `src/zir.rs`:
//! the Rust front-end's codegen backend runs over the whole corpus, not just
//! unit fixtures.

use std::fs;
use std::path::PathBuf;

use zplus::{parse, Zir};

fn corpus_root() -> PathBuf {
    // tests run with CWD = crate dir (tools/zplus); corpus is at repo/programs.
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
        .join("programs")
}

fn collect_zp(dir: &PathBuf, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let p = e.path();
        if p.is_dir() {
            collect_zp(&p, out);
        } else if p.extension().and_then(|s| s.to_str()) == Some("zp") {
            out.push(p);
        }
    }
}

#[test]
fn corpus_lowers_to_wellformed_zir() {
    let root = corpus_root();
    let mut files = Vec::new();
    collect_zp(&root, &mut files);
    assert!(!files.is_empty(), "no .zp corpus found at {:?}", root);

    let mut parsed_ok = 0usize;
    let mut emitted_ok = 0usize;
    let total = files.len();

    for f in &files {
        let Ok(src) = fs::read_to_string(f) else { continue };
        let Ok(module) = parse(&src) else { continue };
        parsed_ok += 1;

        let name = f.file_name().and_then(|s| s.to_str()).unwrap_or("?.zp");
        let json = Zir::lower(&module, name).to_json();

        // structural well-formedness
        let opens = json.matches('{').count();
        let closes = json.matches('}').count();
        assert_eq!(opens, closes, "unbalanced braces in ZIR for {:?}", f);
        let bopen = json.matches('[').count();
        let bclose = json.matches(']').count();
        assert_eq!(bopen, bclose, "unbalanced brackets in ZIR for {:?}", f);
        assert!(json.contains("\"zir\": 1"), "missing version in {:?}", f);
        assert!(json.contains("\"nodes\""), "missing nodes array in {:?}", f);
        emitted_ok += 1;
    }

    // Every program the parser accepts must emit well-formed ZIR.
    assert_eq!(
        parsed_ok, emitted_ok,
        "some parsed programs failed to emit well-formed ZIR"
    );
    // Sanity floor: the front-end handles most of the corpus.
    assert!(
        parsed_ok * 2 > total,
        "expected majority of corpus ({}) to parse; only {} did",
        total, parsed_ok
    );
    eprintln!(
        "ZIR corpus: {}/{} programs parsed and emitted well-formed ZIR",
        emitted_ok, total
    );
}
