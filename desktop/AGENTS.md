# AGENTS.md — rules for writing code in this repository

The C style for this repository is the one in
[`external/mu_lib/AGENTS.md`](external/mu_lib/AGENTS.md). Read it; it applies
here unchanged. The two rules below are repeated here because the demos are
where they are broken most often.

## 1. Do not break code into unnecessary helper functions

Longer functions are often better. A function that reads top to bottom, with
its state in view, is easier to write and to reason about than the same code
spread over five helpers that are each called once.

- Do not extract a helper that is called from one place. Leave the code where
  it runs and give the block a one-line comment as its heading.
- Do not split a function because it is long. Length is not a reason.
- Do not write wrappers that only rename or forward a call.
- Extract a function when it is called from two or more places, when an API
  needs it as a callback (GLFW, `PassDesc.record` in the old demos), or when it is a
  separate transform with its own inputs and outputs.
- When a helper exists only to hide a ladder of checks, the fix is rule 2,
  not the helper.

## 2. Few `if` blocks, few null checks

Checks that cannot fail in a working build make code uglier to read, harder
to write and harder to reason about. Setup code should be straight lines.

- **A failure during setup is fatal where it happens.** The function that
  can fail (shader load, target or buffer creation, pipeline creation)
  prints what failed and exits. Its callers do not test the result. If a
  helper does not do that yet, change the helper, not the call site.
- No `if (!x) return false;` ladders, no `ok = ok && …` chains, no testing
  the same handle again in every function it passes through.
- Do not check what cannot happen: a pointer your own caller passed, a count
  you just computed, `malloc` in a demo or a tool.
- Check where a value enters the program and can really be wrong: command
  line arguments, files, results that vary at run time (a swapchain that is
  out of date). Check once, there.
- Prefer data that removes the branch: a loop over zero items instead of
  `if (count)`, a valid dummy resource instead of an optional one, a table
  instead of an `if` chain.
- State an invariant with `MU_ASSERT` (debug builds), not with a run-time
  `if`.
- The test: if deleting the check changes nothing unless there is a bug or a
  broken install, delete it or make it an assert.

