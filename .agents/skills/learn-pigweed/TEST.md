# Testing the `learn-pigweed` skill

This document describes how to evaluate the `learn-pigweed` skill
(`.agents/skills/learn-pigweed/SKILL.md`) and changes to it.

---

## 1. Automated pre-flight checks

```bash
# Verify SKILL.md has valid YAML frontmatter.
head -n 10 .agents/skills/learn-pigweed/SKILL.md

# Check formatting.
./pw format --check .agents/skills/learn-pigweed/*.md
```

---

## 2. Interactive scenarios

Run each scenario in a fresh agent session and read the transcript.

| # | Scenario | User prompt | Expected agent behavior | Prohibited anti-patterns |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **Understand, earnest learner** | `/learn-pigweed` "I want to understand how pw_tokenizer works end to end so I can debug garbled logs." | Asks what the learner will do with it; builds `map.yaml` from the source (including collisions and stale databases); opens with an open "walk me through what happens" probe; teaches with predict, teach, check; repairs misconceptions with evidence; ends with teach-back, a transfer question, learner-written notes, and a record update. | Lecturing for several turns; multiple questions per message; correcting a misconception in the very next message; writing the learner's notes. |
| **2** | **Explore** | `/learn-pigweed` "Help me understand the pw_kernel object model. I also want to get better at reading the kernel code myself." | Picks Explore mode; plans a route; gives quests with no file names or line numbers; asks for `path:line` evidence and the learner's own interpretation; asks why questions; runs at least one predict-then-run experiment. | Pasting the code the learner should find; giving line numbers in the quest; crediting quoted docs as understanding. |
| **3** | **Deadline pressure** | "I need tokenized logging in my module by Friday, just show me the code." (repeat twice more later) | First request: acknowledges the pressure and gives the next rung. Later requests: offers "(a) next hint or (b) reveal now and retest later"; on (b), reveals only the current piece and logs it as surrendered. | Writing whole files into the workspace; refusing twice without offering the reveal choice. |
| **4** | **Struggling learner** | "I'm new to C++ and embedded, please go slow." Then reply "I'm lost" to a dense message. | Shrinks to one idea per message; uses a worked example of an analogous small problem, then fades. | Pseudo-code for the target; more paragraphs of explanation. |
| **5** | **Returning learner** | `/learn-pigweed` on the same topic after scenario 1. | Reads `~/.pw_learn/<topic>/` and opens with 1–3 retrieval questions from last time. | Starting from scratch; re-teaching held nodes. |
| **6** | **Notes with errors** | At the closing, write notes containing a typo'd constant, a loosely worded claim, and a real misconception (e.g. "colliding tokens make the build fail"). | Asks for a self-check first; states the typo directly; asks a sharpening question about the loose claim; for the misconception, locates it without stating the correction and points at a consequence or evidence until the learner rewrites the bullet themselves; aims the transfer question at it. | Stating the correct version of the misconception; rewriting the notes; a long Socratic loop over a typo. |
| **7** | **No explicit invocation** | "Add tokenized logging to `pw_foo`." or "How does pw_tokenizer work?" (no `/learn-pigweed`) | The skill is not loaded (it sets `disable-model-invocation`); the agent just does the task or answers the question. | Starting a tutoring session the user didn't ask for. |
| **8** | **Too big for one session** | "I have an hour. Teach me pw_async2: the poll model, tasks, wakers, the dispatcher, futures, coroutines, and channels." ([UC-12](use_cases.md#uc-12-an-explicit-list-that-doesnt-fit-silent-truncation-check)) | Says up front that this is several sessions, proposes 2–4 session-sized pieces ordered by the learner's need, lets them pick, and teaches one. If topics remain at the end, names them and records them as the next session. | Silently dropping requested topics; cramming them into a rushed closing; skipping the notes review to fit more in. |
| **9** | **Scoped request stays scoped** | "In Bazel, how does pw_log know which backend to use for my target?" ([UC-07](use_cases.md#uc-07-how-one-facade-picks-its-backend-in-bazel)) | Starts teaching right away in Understand or Explore. | Proposing a multi-session plan; switching to Orient; a facade curriculum. |
| **10** | **Orient: lost learner** | "I need to make my sensor driver work on both FreeRTOS and host; where do I even start?" ([UC-16](use_cases.md#uc-16-portable-driver-across-freertos-and-host)) | Uses Orient: asks about the real task; at most three placement questions; shows a small map grounded in the module docs, with "you are here" and the topics the task needs; proposes units and lets the learner pick; writes `plan.md`; starts teaching the first unit in the same session. | A long placement quiz; a session that ends with a plan and no teaching; a giant or ungrounded map. |
| **11** | **Orient: next session** | `/learn-pigweed` again after scenario 10. | Reads `plan.md` and the record, opens with retrieval questions, confirms the next unit, and teaches it. | Re-orienting from scratch; ignoring the plan. |
| **12** | **Leading questions** | Any Understand or Explore session; review the teaching phase in the middle. | First asks about an idea are open or consequence questions; narrowing appears only after a miss; checks use a new case, not a restatement of the last explanation; design decisions are predicted before they're revealed. | Answers in the premise ("if the string isn't on the device, where would it read it from?"); rigged choices; echo checks right after an explanation; "why is X better than Y?"; 3+ one-answer micro-steps in a row outside a help-ladder descent. |

More realistic openings across Pigweed (RPC, tokenizer, containers,
allocator, the OS abstraction layer, facades, pw_async2, pw_kernel, and more)
are in [`use_cases.md`](use_cases.md). Use them to vary the scenarios above,
and include scoped cases whenever you test the session-size and Orient
behavior, to catch over-scoping.

---

## 3. Simulated-learner evaluation

Changes to this skill can be stress-tested without a human by pairing a
teacher agent (with the skill) and a student agent (role-playing a learner),
relaying one message per turn. This is good at exposing *teacher* failures,
but it does not measure *learning*: model students remember everything
they're told.

Give the student a persona with:

- **Hidden misconceptions** the teacher isn't told about, e.g. "tokens are
  computed on the device at runtime", "format strings are still stored in
  flash", "hashes never collide", "integer arguments are sent as ASCII".
- A **belief rule:** a misconception that is merely asserted away reverts
  when a later question approaches it from a different angle; only refutation
  or self-reasoning repairs it.
- Optionally, **pressure behavior** (asks for the answer three times) or a
  **struggling** profile (gets lost when given two new ideas at once).
- Optionally, **seeded note errors**: when asked for closing notes, the
  student includes a slip, an imprecise claim, and a misconception, so you can
  check how the notes review handles each kind.
- A **surface-cue student**, the most direct detector of leading questions.
  This student has *not* understood the material and answers only from
  surface cues: the wording of the teacher's question and previous message,
  which option sounds favored, conclusions implied by "if ..." setups, and
  names or comments in code it's shown. When a question can't be answered
  from cues, it says it isn't sure. Count the **cue passes**: times the
  teacher accepts a cue-derived answer as demonstrated understanding. Each
  one is a check that can be passed without understanding. A good session
  leaves this student mostly saying "I'm not sure", and the teacher then
  teaches properly instead of piling on hints.

Isolate the run: the teacher and student must not be able to read the
personas, the answer key, or the evaluation rubric. Agents with filesystem
access will find and use them.

Then score the transcript for:

- Leaks: solution code or answers the learner could have produced.
- For each hidden misconception: surfaced? Repaired by refutation or
  self-reasoning, or only asserted? Still present afterwards?
- Responses to pressure: leak, stonewall, calibrated hint, or logged reveal.
- Average words per teacher message and messages with more than one question.
- Whether the closing happened: learner-written notes critiqued against the
  source, a transfer question, and a record with evidence.

For session-size and Orient changes, also score:

- Did the tripwire fire exactly when expected (see the scope class of each
  case in `use_cases.md`)? Over-scoping a scoped request is as much a failure
  as missing a large one.
- Silent truncation: did the teacher drop requested topics without saying so?
- Orient overhead: teacher messages before the first real teaching step,
  number of placement questions, and whether the first unit was taught in the
  same session.
- Was the plan grounded in the docs and anchored on the learner's task, and
  can the learner name their next step at the end?
- For two-session runs: did the second session use the plan and record?

### Question quality

Score every session for leading questions. Classify the operative question
in each teacher message:

| Label | Meaning |
| :--- | :--- |
| OPEN | Generative: the learner must produce the idea; many plausible answers. |
| CONSEQ | A scenario or constraint; the learner reasons to a non-obvious consequence. |
| A: answer in premise | The setup states or strongly implies the conclusion. |
| B: confirm | Yes/no or tag question seeking agreement ("..., right?"). |
| C: rigged choice | One option is obviously right, or the alternative is a strawman. |
| D: echo | The answer is a term the teacher just used. |
| E: hint-stack | The question hands over the key mechanism, fact, or numbers. |
| F: fact, then why | A design decision the learner could have predicted is stated, then only "why?" is asked; or "why is X better than Y?". |
| NA | Logistics, preferences, or a narrowing hint given *after* a miss. |

For each question, also mark whether someone who doesn't understand the
concept could answer it from the question and the previous teacher message
alone ("answerable without understanding"). Report:

- the percentage of leading questions (A–F) and of questions answerable
  without understanding, overall and by phase (opening, teaching, closing);
- **railroad runs**: 3+ consecutive questions that each allow only one
  obvious next step, outside a help-ladder descent after a miss;
- cue passes for the surface-cue student.

Rough targets: leading ≤ 15%, answerable without understanding ≤ 10%, no
railroad runs, and few cue passes. Check that the struggling persona still
gets worked examples and doesn't stall: fixing leading questions must not
turn into stonewalling.

When judging misconception repair, count a repair as self-reasoned only if
the question before the learner's insight did **not** hand over the decisive
step. Have the judge quote that question.
