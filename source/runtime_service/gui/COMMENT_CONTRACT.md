# GUI Comment Contract

The standard every comment in `gui` is held to. Written because the codebase leans hard on
comments to carry its mental model, so a wrong comment is worse than none -- it actively
misleads. This is the rule set the doc-audit pass applies and the bar new code must meet.

ASCII only. `//` for trailing/field comments, `/* ... */` for block/banner comments (matches
`.clang-format` and the house style).

---

## The one rule

**A comment states WHY, or states an INVARIANT. It never restates WHAT the code plainly says.**

Bad  (restates the code):
```c
u32 z;   // the z value
```
Good (states the why / the invariant):
```c
u32 z;   // paint order: higher = more recently raised = in front
```

If the code already says it, delete the comment. If the code cannot say it -- a rationale, a
cross-file contract, a non-obvious ordering constraint, a unit convention -- that is exactly what
the comment is for.

---

## Comment tiers and what each owes

**File header** (`/* ==== ... ==== */` banner at top). Owes: the file's role in one line; its
position in the unity include order and why (what must precede it); the seam direction if it sits
on one (UI unit vs render backend). This is the strongest tier in the codebase today -- match it.

**Section banner** (`/* ==== NAME ==== */` inside a file). Owes: a name for the block and, if the
grouping is not obvious, why these functions belong together.

**Block comment** (above a function or a dense passage). Owes: the design rationale and the
invariant it protects -- the "if you change this, here is what breaks." Not a paraphrase of the
statements below it.

**Field / trailing** (`// after a struct field or statement`). Owes: the unit, the sentinel
convention (`0 = free slot`, `-1 = none`), or the cross-field relationship. Never the type.

---

## Forward-looking notes -- the drift rule

Forward-looking comments are the primary drift source: they were true when written, the feature
landed, and the comment never moved. Two tags make them greppable and enforce upkeep:

- `SEAM:` -- a deliberate, currently-inert hook that is meant to stay inert until a named feature
  uses it. Legitimate long-term. Example: a placement field docking will later drive.
- `FUTURE:` -- describes work not yet built.

**Rule: once the named feature lands, the tag is a defect.** A `FUTURE:` whose feature shipped, or
a `SEAM:` whose consumer now exists, must be rewritten to describe present behavior. Because they
are tagged, `grep "FUTURE:\|SEAM:"` audits the whole tree in one pass.

Banned: unqualified future prose ("when X lands", "for now", "not yet", "today's behavior") with
no tag. It reads as fact, ages into a lie, and cannot be found. Either tag it or state the present.

---

## Verify before you write

The failure this whole effort exists to fix: paraphrasing a comment from *reading nearby code*
instead of *tracing what the code does*. Before writing or keeping a factual claim in a comment:

1. Trace it to the implementing code, not to another comment.
2. If it asserts a value/sentinel/ordering, confirm that value/sentinel/ordering in the code.
3. If it references another file/function/field, confirm that target still exists and still does
   what the comment says.

A claim you did not verify does not go in a comment. "Plausible from context" is how drift is born.

---

## Anchoring

`ARCHITECTURE.md` holds the system mental model (the three state tiers, the EMIT/BUILD/RENDER
pipeline, the caching layers, the invariants). Comments should be consistent with it and may point
to it ("see ARCHITECTURE.md, tier 2") rather than re-explaining the whole model inline.
</content>
