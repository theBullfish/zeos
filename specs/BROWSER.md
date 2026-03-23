# Zeos Browser — Signal Chain Web Engine

> A web browser where every stage is a signal chain node.
> No monolith. No 35 million lines of C++.
> Request → Parse → Layout → Paint. Four signal chains.
>
> **Date**: March 23, 2026
> **Status**: Architecture specification
> **Codename**: Surf

---

## Why Build a Browser

Z-OS needs web access to be a real OS. But Chromium is 35M LOC.
Firefox is 25M. Even a "simple" browser (NetSurf) is 200K LOC.

The signal chain architecture makes a browser structurally different:

1. **HTTP is already a signal** — request → response is a chain
2. **HTML is a tree** — trees ARE directed graphs (signal chains)
3. **CSS is a transform** — style application is a pipeline
4. **Layout is constraint resolution** — same as signal chain resolution
5. **Paint is a signal sink** — pixels are the output

The entire browser is: `url -> fetch -> parse -> style -> layout -> paint`

---

## Architecture

### Stage 1: Network (Fetch)

```
url -> dns_resolve -> tcp_connect -> tls_handshake -> http_request
http_request -> response_headers -> {
    gate(content_type: "text/html")  -> html_parser,
    gate(content_type: "text/css")   -> css_parser,
    gate(content_type: "image/*")    -> image_decoder,
    gate(content_type: "text/javascript") -> js_engine,
    gate(content_type: "application/json") -> json_parser
}
```

Signal chain. Each content type routes to the right parser.
No giant switch statement. Just gates.

### Stage 2: Parse (DOM Construction)

HTML parser produces a DOM tree. The DOM tree IS a signal graph:

```
html_parser -> tokenizer -> tree_builder -> dom_tree

// Each DOM node is a signal node:
// <div> contains <p> contains "hello"
// = div_node -> p_node -> text_node("hello")

// Attributes are node properties
// <a href="..."> = a_node {href: "..."}

// Events are signals
// onclick = user_click -> a_node -> navigate(href)
```

### Stage 3: Style (CSS Application)

CSS is a pipeline of transforms applied to DOM nodes:

```
{dom_tree, css_rules} -> selector_match -> cascade -> computed_style

// Selector matching: for each DOM node, find matching CSS rules
// Cascade: resolve conflicts by specificity
// Computed style: final values for each property

// This is a TRISA pipeline:
// detect (which rules match) → classify (specificity) → act (apply)
```

### Stage 4: Layout (Box Model)

Layout resolves constraints — given computed styles, where does
each box go? This IS signal chain resolution:

```
computed_style -> box_generation -> {
    gate(display: block)  -> block_layout,
    gate(display: inline) -> inline_layout,
    gate(display: flex)   -> flex_layout,
    gate(display: grid)   -> grid_layout,
    gate(display: none)   -> drop
}

block_layout -> resolve_widths -> resolve_heights -> position
```

### Stage 5: Paint (Rasterization)

Paint walks the layout tree and draws to the framebuffer:

```
layout_tree -> paint_order -> {
    gate(type: background) -> fb_rect,
    gate(type: border)     -> fb_rect,
    gate(type: text)       -> fb_text,
    gate(type: image)      -> fb_blit,
    gate(type: shadow)     -> fb_blur
}
```

This uses the framebuffer primitives from SIGNAL_VISUALIZER.md.
Same `fb_rect`, `fb_text`, etc.

---

## Implementation Phases

### Phase 1: Text Browser (MVP)
- HTTP/1.1 GET requests (no TLS initially)
- HTML tokenizer (subset: `<html>`, `<body>`, `<p>`, `<a>`, `<h1>`-`<h6>`, `<br>`, `<pre>`, `<ul>`, `<li>`, `<img alt>`)
- No CSS (hardcoded styles)
- Text rendering to framebuffer (already have `fb_text` via kprint)
- Link navigation (highlight, select, follow)
- **This is a usable browser.** Like lynx, but signal-chain native.

### Phase 2: Styled Browser
- CSS parser (subset: color, background, font-size, margin, padding, display, width, height)
- Box model layout
- Basic colors and backgrounds
- Inline images (PNG decode → framebuffer blit)

### Phase 3: Modern Browser
- TLS 1.3 (need crypto in kernel)
- JavaScript engine (or Z+ as the scripting language)
- Flexbox layout
- Full CSS cascade
- Fonts (TrueType rasterization)
- Scrolling, input fields, forms

### Phase 4: Z-OS Native
- Replace JavaScript with Z+ scripting
- VAULT integration (history IS temporal state)
- MasQ for page identity (is this page what it claims to be?)
- Zixel timing for performance analysis
- Signal chain devtools (trace the render pipeline)

---

## Kernel Requirements

### Networking Stack (does not exist yet)
1. Ethernet/WiFi driver (or virtio-net for QEMU)
2. IP stack (ARP, IPv4, TCP)
3. DNS resolver
4. HTTP/1.1 client
5. TLS 1.3 (phase 3)

### Graphics (partially exists)
1. `fb_rect`, `fb_text` — need to add from SIGNAL_VISUALIZER.md
2. `fb_blit` — copy pixel buffer to framebuffer
3. Scrolling — viewport offset into a larger render buffer
4. Mouse input — click on links (keyboard nav first)

### Memory
- DOM trees need dynamic allocation (heap exists)
- Render buffers need large contiguous allocations

---

## What Makes This Different

Every other browser is a monolith. Layout, style, paint are deeply
intertwined. Changing one breaks the others.

In Zeos, each stage is an independent signal chain. You can:
- **Replace the layout engine** without touching the parser
- **Tap any stage** for debugging (`dom_tree ~> inspector`)
- **See timing per stage** via Zixel (already built into signal nodes)
- **Hot-swap the renderer** (software → GPU → Goya)
- **Fork the pipeline** (render to screen AND save to VAULT simultaneously)

The browser is not a special application. It's just signal chains.
The same signal chains that run robots and games.

---

## For Students (DereZ)

Building a browser is the ultimate DereZ project:
1. Start with `fetch url -> print(response)` — that's curl
2. Add HTML tokenizing — now you see tags
3. Add rendering — now you see a page
4. Add links — now you can navigate
5. Add CSS — now it's pretty

Each step is a new node in the signal chain.
The browser grows by wiring, not by rewriting.

**Codex Labs LLC — 2026**
