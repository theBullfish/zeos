# Zeos Working Notes

Append-only log of non-obvious things discovered during work — constraints,
workarounds, gotchas, decisions made and their rationale. Date every entry.
Future-self forgets these.

Newest at the bottom. Never rewrite previous entries — append a dated
correction if needed.

---

## 2026-05-04 — bootstrap

- `tools/zplus/` is empty as of this commit. Z+ compiler is the chosen
  P0 BUILD entry point because zeos-build, runtime libraries, and signal
  chain definitions all eventually compile through Z+.
- Z+ work to date lives elsewhere on Z13 (Brad's notes mention 13+
  programs and a `FINDINGS.md`). Confirm whether to import that work or
  start fresh before writing the lexer.
- Doctrine docs (`docs/SIGNAL_LOGIC.md`, `docs/CHAIN_CONTRACT.md`,
  `docs/COMPONENT_AS_MODULE.md`, `docs/TRISAVERSE_STACK.md`) are
  append-only — never rewrite sections, append dated revisions.
