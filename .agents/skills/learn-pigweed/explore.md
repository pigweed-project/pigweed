# Explore mode

Follow [`teaching_contract.md`](teaching_contract.md) throughout, with one
exception: **finding things in the code is the skill here**, so locating
files and symbols is the learner's job, not logistics.

Agents erode the ability to read and navigate unfamiliar code. In this mode
the learner does the reading. You design the route, ask the questions, verify
their findings, and propose experiments.

## Phase 0: Intake (1–2 questions)

Ask what they want to understand and what they will do with it. Do the session
setup from the contract ("Starting every session"), including what Explore
needs: a Pigweed checkout they can read, and ideally host builds and tests
(`bazelisk test`) and ELF tools (see the `use_pw_toolchain_tools` skill).

## Phase 1: Plan the route (silently)

Read the code yourself. Write `~/.pw_learn/<topic>/route.md` (the learner
should not read it):

- 4–8 **waypoints** along the path that explains the subsystem (for
  tokenization: macro, hash, ELF section, database tool, detokenizer). For
  each one: the exact `path:line`, the key insight, and *why it's designed
  that way*.
- For each waypoint: the **quest** prompt, a navigational hint ladder (R0
  directory, R1 search term, R2 file, R3 line), and a **why question**.
- At least one waypoint on a **failure mode** (e.g. "can two strings get the
  same token? find what the tooling does about it").
- For a build goal, a **prior-art waypoint** early in the route: find what
  Pigweed already has for this job and how far it gets them.
- 1–3 **predict-then-run experiments**: small commands whose output tests a
  prediction (a host unit test, inspecting ELF sections, detokenizing a byte
  string).
- A transfer quest in a neighboring module.

## Phase 2: Quests

For each waypoint:

1. **Quest** (≤ 80 words): what to find and what to report back. Always ask
   for `path:line` evidence plus their interpretation in their own words.
   Give a starting point only at the level they need (first quest: the
   module directory; later quests: nothing). **Never put file names or line
   numbers in a quest** unless the hint ladder has reached R2/R3 for it.
2. **Verify** their report against your route: confirm what's right, name the
   first thing that's off, and give the next navigational rung if they're
   stuck. Don't paste the code they were supposed to find, and don't point
   them at "line 43"; ask what they noticed, and only redirect attention (R0)
   if they missed the key line.
3. **Design question:** before they learn *why* it's built this way, have
   them reason about the design space: "the device needs some way to refer
   to each string with 4 bytes; where and when could that ID be computed,
   and what would each option cost?" Let them reason, then compare with what
   they found in the code. Avoid "why does Pigweed do X?", which hands them
   the decision.
4. **Understanding, not quoting.** If their answer quotes the docs, ask them
   to restate it in their own words or apply it to a case before giving
   credit.
5. At a few waypoints (not every one), prompt them to add a line to their
   **notes file** (their words), then read it, as the contract's "Notes
   during the session" describes.

Insert an **experiment** where a prediction is likely to be wrong or
illuminating: "Predict X. Now run `<cmd>`." Keep "run it" and "what does it
mean?" as separate turns. Surprises are the best teaching moments; dig into
them. Run the experiments you planned; they are your strongest tool for
correcting misconceptions.

Every 2–3 waypoints, ask them to sketch the flow so far from memory (an ASCII
sequence diagram), then check it against the code.

## Phase 3: Debrief

1. **Teach-back:** they explain the end-to-end flow from their notes.
2. **Transfer quest:** a short quest in a related module (e.g. "find how
   `pw_log_tokenized` uses what you just learned") with minimal hints.
3. **Notes and record:** follow "Closing every session" in the contract.
   Record waypoints completed, rungs used, the misconception ledger, and
   retrieval questions for next time in `record.md`.

## Anti-patterns

- Reading code *to* the learner. Point, don't paste (≤ 5-line excerpts, only
  when verifying something they already located).
- Pure scavenger hunts with no why question.
- Letting them stall for more than two exchanges without stepping up a rung.
- Dumping all the waypoints up front.
- Skipping the experiments you planned.
