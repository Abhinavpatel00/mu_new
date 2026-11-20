# AGENTS.md — coding rules for this repository

The C style rules live in `desktop/AGENTS.md` and apply to both targets
(desktop/laptop and mobile) unchanged. Read them. The two rules repeated there
because they are broken most often:

1. **Do not break code into unnecessary helper functions.** Longer functions
   are often better. Extract only when a helper is called from two or more
   places, is needed as a callback, or is a separate transform.

2. **Few `if` blocks, few null checks.** A failure during setup is fatal where
   it happens. Check once where a value enters the program and can really be
   wrong. State invariants with `MU_ASSERT` (debug builds), not run-time `if`s.

When in doubt, follow `desktop/AGENTS.md` — it is the house style for `vk.c`,
`main.c`, `mg.c`, `platform.c` and every file in this repo.
