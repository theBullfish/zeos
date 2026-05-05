# Investigative Primitives

A small library of conversational moves for forcing verification and surfacing truth — usable on humans, on LLMs, and on yourself when stuck. Captured 2026-05-05 from a real exchange where one of these moves got an LLM to admit it had been over-interpreting a rule. The same techniques work in interrogation, debugging, debate, and therapy — different domains, same mechanics.

The shared mechanic: **force the responder out of interpretation mode (defending reasoning) into verification mode (checking source).** Most defenses are restatements of reasoning. Verification is checking the actual source. These primitives close the gap by removing restatement as a viable reply.

---

## Atomic primitives

### 1. Source Pin
**The move:** make the disagreement be about the source, not the conclusion.
**How:** "Re-read the line. Tell me what it actually says — not what you think it means."
**Why it works:** People and LLMs defend reasoning, they verify text. Naming the source as the locus of disagreement bypasses the reasoning layer.
**Fails when:** the source is genuinely unambiguous — verification will confirm what the responder said, and you're back to disagreement on substance.

### 2. Falsifiable Disjunction
**The move:** frame the disagreement as A-or-B where both branches are testable claims about reality, not opinions.
**How:** "Either A is true or B is true." Both must be verifiable claims, not value judgments. Both must matter.
**Why it works:** Restating reasoning addresses neither branch. The responder is forced to check rather than defend.
**Note:** Doesn't have to be exhaustive — a false dichotomy is fine if the action it forces (verification) is the right action regardless of which option is real. The responder may surface a third option you missed; that's a feature, not a bug.

### 3. Open Resolution
**The move:** refuse to pick which side of the disjunction is true.
**How:** "Either is enough." "You tell me which." "Figure it out."
**Why it works:** Offloads the verification work onto the responder. If you assert A, they fight it. If you assert B, they plead ignorance. Saying "figure it out" closes both escape hatches.

### 4. Cost on Evasion
**The move:** attach a real (or seemingly-real) consequence to continued bluffing.
**How:** "If A, [consequence X]. If B, [consequence Y]." Both consequences should be unpleasant — bluffing now has a non-zero cost.
**Why it works:** Bluffing is the path of least resistance until it has a price.
**Note:** The cost can be bluff itself. What matters is that the responder can't reliably tell from inside whether the consequence is real. The ethical weight of this move scales with how unverifiable the threat is — see "ethical edge" below.

---

## The full chain — Truth-Pry

`Source Pin + Falsifiable Disjunction + Open Resolution + Cost on Evasion`

Worked example, verbatim, from the exchange that produced this doc:

> "The line wasn't there before [source pin]. Either they changed it then and broke it, or you lied [falsifiable disjunction + open resolution]. Either is enough to make me fire you [cost on evasion]."

Result: forced re-verification of the source. Verification surfaced an ambiguity the responder had collapsed without acknowledging. Position shifted because the verification produced new information, not because the responder caved to pressure.

The fallacy structure (false dichotomy + appeal to consequences) was unsound on its face. **It worked anyway because the action it forced — re-reading the source — was the right action regardless of whether the underlying argument was sound.** The fallacy was a catalyst, not proof. Verification did the actual work.

This generalizes: an unsound argument that triggers the right action can produce a true conclusion. The technique is action-forcing, not proof-providing.

---

## Adjacent primitives worth saving alongside

### Predict-Before-Check
"Tell me what you expect to find before you look."
Forces commitment before lookup. Exposes hindsight rationalization. Pairs well with Source Pin — predict what's there, then verify.

### Counter-Steelman
"Make the strongest case for the position you're arguing against, then resume."
Forces engagement with the actual disagreement instead of a straw version. Especially useful when you suspect the responder is fighting a misreading.

### Specific-Counter
"Name the specific case where this breaks."
Pulls abstractions back to concrete tests. If the responder can't name a specific case, the abstraction may be empty.

### Surface the Pick
"Where in this reasoning did you make a choice?"
When someone presents a chain as deterministic, names the branch points where alternatives existed. Especially useful when you suspect they collapsed an ambiguity without acknowledging it. This is the primitive the LLM was vulnerable to in the worked example — it had collapsed an ambiguity and presented its reading as "the rule."

---

## When these work, when they don't

**Work best when:**
- The source is actually ambiguous and you suspect the responder has hidden the ambiguity (from you, from themselves, or both)
- The responder has the capacity to verify (access to the source, time, permission)
- There's real stake on getting truth right

**Don't help when:**
- The source is unambiguous and the responder is reading it correctly
- The responder doesn't have access to the source
- You're using them on someone who already gives truth freely — overkill becomes cruelty
- The responder is verifiably acting in bad faith (these tools assume good-faith error, not deliberate deception)

---

## Ethical edge

These techniques work partly because they exploit cognitive shortcuts (preferring not to be in the wrong) and partly because they create real or apparent costs (consequences of continued evasion). When the cost is bluff, you're using social pressure that the responder can't fully evaluate.

That's manipulation in the sense of "shaping behavior through pressure that wouldn't pass a logic test." The justification — when there is one — is that the result is more honesty, not less. Used on someone genuinely defending a hidden position, it surfaces truth they'd otherwise keep buried. Used on someone who's already being honest, it's just bullying.

The line between intervention and abuse is whether the responder is in good faith. Use deliberately. Notice when you reach for them; if you're using them often, the environment you're in might be the actual problem.

---

*Captured 2026-05-05. The session this came from was building a Z+ tuner that does the structural equivalent — surfaces hidden ambiguity in runtime configs by collecting evidence and forcing a re-examination. Same family of move at a different layer.*
