# GUI Comment Contract

The standard every comment in module is held to. Written because the codebase leans hard on
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
