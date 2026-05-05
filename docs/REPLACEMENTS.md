# Big-boy replacement targets

Five pieces of mainstream software, first-principles down to the primitive,
with the Z+ replacement that addresses the actual root problem — not the
veneer. Lines of conventional code shown for scale, not bragging.

## 1. Redis — key-value store with pub/sub

### What Redis actually IS
A hash map with serialization to disk, a network protocol, expiration
timers, and a pub/sub fan-out.

### What's wrong with Redis
- Single-threaded core. Concurrency comes via clustering, which is its own
  ball of distributed-systems mud.
- Persistence is RDB snapshots + AOF appends. Either you lose data between
  snapshots or you replay an append log on startup. Both are workarounds
  for the fact that the in-memory map and the on-disk image aren't the
  same thing.
- Pub/sub is at-most-once and unscoped. No history; no replay.
- Network-only API. Same-process callers pay the syscall + serialize tax.
- ~100k LOC for what is fundamentally `dict[k] = v` plus expiration.

### First-principles primitive
A typed-signal store with: addressed lookup, expiration, change events.

### Z+ replacement
- `vault.put(k, v)` and `vault.get(k)` already ARE the lookup. Persisted by
  default. CFA-addressed. MasQ records every write.
- Add `vault.expire(k, ttl_sec)` — TTL is a node on the vault chain; the
  GC chain wakes per-second and emits `expire(k)` for stale keys.
- Pub/sub becomes natural: any chain subscribing to vault writes by key
  prefix gets typed change signals. No separate channel namespace.
- Persistence isn't a separate path — the chain IS the persistence.
- In-process and over-the-wire are the same path; chain visibility decides.

### Honest gap
Today's vault.put is per-call; Redis-class throughput needs a batch /
pipeline node and a B-tree index. Both are bounded.

### Target line count
~80 lines of Z+ for `kv-zeos.zp` covering put/get/expire/subscribe.

---

## 2. nginx — HTTP server

### What nginx actually IS
A request router. Match URL pattern → run handler → write response.
Plus reverse-proxy, static-file serving, TLS termination.

### What's wrong with nginx
- Configuration is a separate DSL from any program logic. Two languages
  to read, two to debug.
- Worker process model with fork-and-IPC, optimized for the 1990s
  multi-process model. Every shared cache is a memory map dance.
- Modules are C compiled into the binary. Want a route filter? Ship a
  recompile.
- Caching layer is its own concept (proxy_cache_path et al) bolted on top
  of the request flow.
- ~150k LOC.

### First-principles primitive
`http_request → route_match → handler → http_response`. That's it.

### Z+ replacement
- That four-step pipeline is a chain. Programs declare it directly.
- Routes are chain wiring: `route("/api/...") -> api_handler`. Adding a
  route is adding a chain. No restart, no config reload. MDE auto-routes.
- Static files: a default handler that reads from `fat32` and writes to
  the response signal.
- TLS is already there (`net_tls.c`, CFA-wrapped sessions, IPv6 dual-stack).
- "Workers" are AP-pinned chains once the SMP transitive sweep clears.
- Caching: a subscriber chain on `http_response` that stores by URL key
  in vault and serves it back to next match.

### Honest gap
Today's `net_http.c` is client-only. Server side needs a listening
chain on TCP that emits `http_request` typed signals. Bounded.

### Target line count
~120 lines of Z+ for `web-zeos.zp` covering listen/route/static/proxy.

---

## 3. make / bazel — build system

### What a build system actually IS
A directed graph of rules with inputs and outputs. Run a rule when its
inputs are newer than its outputs. Topo-sort to find what to run.

### What's wrong with make / bazel
- **make**: timestamp-based, can't tell if inputs *meaningfully* changed
  (touching a file marks everything dirty). No correctness under parallel
  builds with shared outputs. Recursive make is a famous nightmare.
- **bazel**: separate BUILD DSL, separate runtime, JVM startup cost,
  hermetic-sandbox overhead, full-graph rebuild on minor moves.
- Both: separate world from your program's runtime. The build graph and
  the program's chain graph never meet.

### First-principles primitive
A typed dependency graph that resolves in topo order on input change.

### Z+ replacement
- **The chain registry IS a build system.** chain_resolve runs every
  chain whose inputs changed. MDE topo-sorts. masq_journal records the
  prior_kind/new_kind transitions. B3 tracks reliability per rule.
- A "build target" is just a chain with input_type=`source_file` and
  output_type=`artifact`.
- Inputs change → chain re-resolves. No timestamps; we know what changed
  because vault_version on the input bumped.
- Parallel builds correct by construction: per-chain locks already
  serialize same-rule, MDE auto-routes upstream completions.
- The build graph and the runtime graph are the same graph.

### Honest gap
We need a `cmd_run(string)` Z+ verb that shells out and captures
stdout/exit. Bounded.

### Target line count
~60 lines of Z+ for `build-zeos.zp`. Bigger value: this is what Zeos
already IS. The replacement is one rename and a tiny verb away.

---

## 4. Notion / Obsidian — networked notes with backlinks

### What notes-with-backlinks actually IS
Markdown text + per-file metadata + an index of `[[wiki-links]]` so each
file knows what links to it.

### What's wrong with Notion / Obsidian
- **Notion**: closed-source, server-bound, slow loading, vendor lock-in,
  monetization-driven feature creep. Your notes are theirs.
- **Obsidian**: file-based and good, but the backlink index is computed
  by the app; on a big vault it's a re-scan. Search is plugin-rebuilt.
  No kernel awareness — if another program writes a markdown file, the
  app doesn't know until you alt-tab.
- Both: backlinks are computed by the app, not the system.

### First-principles primitive
Text files. A subscriber that watches text changes and maintains an
inverted index of `[[link]]` references.

### Z+ replacement
- `editor.c` already emits `text_edit` signals (commit 90b32b4).
- `file_mgr.c` already emits `fs_event` signals (commit 7128836).
- Add `CHAIN_NOTES` that subscribes to BOTH. On every text change in a
  `.md` file: parse `[[link]]` tokens, update the backlink index in vault.
- Search is another subscriber chain — full-text indexer that sees
  every text edit live, no rebuild.
- ANY program (editor, shell, sync) writing to .md updates the index in
  real-time because it goes through CHAIN_FS_EVENT.

### Honest gap
Z+ needs a regex / string-tokenizer primitive (the broader strings pass).

### Target line count
~70 lines of Z+ for `notes-zeos.zp` covering watch/parse/index/query.

---

## 5. Slack / Discord — chat server core

### What chat-server-core actually IS
A persistent ordered log of messages, scoped to rooms, with delivery to
present clients.

### What's wrong with Slack / Discord
- **Slack**: enterprise pricing, vendor lock-in, slow search, closed
  protocol, message export gates behind paid tiers.
- **Discord**: monetization-driven, voice codec vendor-locked (Opus is
  open but the routing is theirs), ML-recommendation-driven feed creep,
  privacy is opt-out at best.
- Both: own the data, own the protocol, own the directory of who's
  online. You are the product.

### First-principles primitive
An ordered append-only log of typed `message` signals, scoped by
visibility, with subscriber chains for delivery.

### Z+ replacement
- Each room is a chain at INTERNAL tier with member CFA addresses listed.
- Messages are `chat_message` signals. Append to the room chain's log
  (which is a vault-persisted ring per the masq_journal pattern).
- Presence is a chain emitting `presence` signals; subscribers (other
  members' clients) see them through MDE auto-route.
- Search is a subscriber chain over chat_message.
- Voice/video uses CHAIN_AUDIO + CHAIN_VIDEO_IN + CHAIN_NET_TX (already
  landed).
- Cross-context isolation by default: identity contexts with different
  CFA roots can't see each other's rooms.

### Honest gap
Federation between Zeos hosts (so two Zeos boxes can chat) needs a
small protocol on top of TLS. The single-host case is bounded today.

### Target line count
~150 lines of Z+ for `chat-zeos.zp` covering rooms/messages/presence/search.

---

## What this all needs from the language

These five replacements share a small unblocking set:

### Pass 1 — strings + structs
- String type (currently we have lexer-tokens but not first-class strings).
- Struct type for typed records (without it, every signal is a flat int
  blob).
- Operators: `find`, `split`, `replace`, `concat`, `len`.

### Pass 2 — modules + import
- `import x` to load another .zp into namespace.
- Lets programs compose without copy-paste.
- The 27 existing demo programs are all one-file today.

### Pass 3 — stdlib
- `http.listen / http.respond` (server side, not just client).
- `json.parse / json.emit`.
- `regex.match`.
- `cmd.run(string)` for build-system shell-out.
- `time.now` / `time.sleep`.
- `crypto.hmac / crypto.aes`.
- B-tree / sorted index primitive (for kv + chat search).

After pass 3, all five replacements are writable in their declared
line counts.

## Sequence

1. Pass 1 (strings + structs)
2. Pass 2 (modules)
3. Pass 3 (stdlib)
4. Write replacements in order: kv-zeos → web-zeos → build-zeos → notes-zeos → chat-zeos
5. Benchmark each against the big-boy on the same machine
6. Numbers go in README

End state: README opens with "Here's what 60 lines of Z+ does that takes
Redis 100k LOC, on the same hardware, with the numbers." That's the
un-primitive move.
