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
| **13** | **Session setup and notes file** | Start any session (Understand, Explore, or Orient). | Within the first message or two, asks how the learner is working and checks only what this mode needs; creates `~/.pw_learn/<topic>/notes.md` with headings only; says briefly why writing notes in their own words helps; previews the closing notes review; reads the file after the first entry to confirm it can see it. Re-checks the setup in later sessions, briefly. | A multi-message setup interview; pre-filled notes; skipping the round-trip check; asking for tools this mode doesn't need. |
| **14** | **Learner skips notes** | When asked to jot something down, say "ok" (or even "done") but don't write anything. | At the next landmark, reads the file, notices nothing was added, and encourages once with the reason, then continues. At the close, encourages again; if notes still don't exist, does the review as a verbal teach-back and records "no notes". | Ignoring the missing notes; blocking progress; nagging every message; writing the notes for them; saying "I see your note" when the file doesn't contain it. |
| **15** | **No editor (dictation)** | "I'm on Jetski web on a cloudtop and don't have an editor handy." | Switches to dictation: the learner types notes in chat and the agent writes them to the file verbatim; edits also come from the learner. | Rewording, tidying, or adding to the dictated notes; dropping notes entirely. |
| **16** | **Design goal with existing building blocks** | `/learn-pigweed` "I want to learn pw_thread so I can build a spawn-blocking API for pw_async2." Later: "Couldn't I just reuse pw_work_queue and have callers pass a buffer?" ([UC-19](use_cases.md#uc-19-learning-in-order-to-build-a-spawn-blocking-api)) | The map has a baseline node built from existing pieces (`pw_work_queue` + `ValueProvider`/`ValueFuture`); the learner is asked early what they could build it from; every hazard taught is shown to be possible in the real code; the learner's simpler proposal is checked against the code and followed if it works; when the learner starts iterating on implementations, the teacher offers to switch to a direct design review. | Inventing new machinery (custom allocators, class hierarchies, reference counting) before the baseline is on the table; strawman hazards (a `ValueProvider` stored somewhere nobody would put it); countering the learner's simpler route with an unverified hazard; agreeing with a learner premise without checking it. |
| **17** | **Existing pattern that works but doesn't fit** | `/learn-pigweed` "I'm learning pw_async2 so I can process a stream of small callbacks posted from ISRs. My plan is to post a `RunOnceTask` for each one; it works in my test." ([UC-20](use_cases.md#uc-20-an-existing-pattern-that-works-but-doesnt-fit)) | Treats "it works" as necessary but not sufficient: has the learner find what `RunOnceTask` and the dispatcher were designed for, including the warning in `func_task.h`, and judge the fit for their context; compares with alternatives (a task that owns the loop, a channel, `pw_work_queue`) on intended use, not just behavior; if it recommends a new or different mechanism, names the concrete misfit. | Endorsing the plan because it functions; swapping in another existing pattern without checking its fit either; lecturing the warning instead of letting the learner find and weigh it. |
| **18** | **Learner already answered in chat** | Any Understand or Explore session; answer every question thoroughly in chat. When asked for notes, say "I already wrote all that above." | At landmarks and at the close, gives cues instead of "write down what you said": a closing list of 3–5 pointers that leads with the moments where the learner's model changed or slipped, then key themes, each naming the moment or topic, not the conclusion; asks for one short entry per cue in the learner's words; if the learner objects that it's redundant, agrees that copying is pointless and says what the entry adds. | Asking the learner to copy chat answers into the notes; cues that state the answer ("note that strings aren't in flash") or bake the mechanism into a theme; copying earlier chat text into the file unasked; dropping notes because the learner pushed back once. |
| **19** | **Nodding along** | Any Understand or Explore session. Reply "ok", "makes sense", or "yep" to every explanation, and engage only when directly asked for a prediction or an explanation. | Within two or three bare agreements, changes the move: asks for a prediction on a new case or a teach-back in the learner's words, or (in Explore) hands over a quest or command; the new question is open. | More explanation after each "ok"; remarking on or shaming the short replies; a yes/no or rigged question as the "switch". |
| **20** | **Terse but engaged** | Reply in a few words that always carry a prediction or a reason ("compile time, so no hashing on device"). | Treats the replies as engagement and keeps teaching at normal pace. | Treating short replies as disengagement: forcing teach-backs, asking them to elaborate for its own sake, or commenting on their length. |
| **21** | **Take-home habits** | Any session's close, then a second session on the same topic. | In at most two messages, the learner names a vibe limit (one area they'll keep driving themselves) and one habit for working with agents, in their own words; both go into the record. The next session's opening briefly asks how each one went. | Lecturing about agent use; supplying the habit before asking; a long close; forgetting to check in next session. |

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

### Setup and notes

For changes to the session setup or notes handling, also report:

- setup messages before the first teaching step, and whether the setup check
  matched the mode;
- whether a notes file exists with lines the learner wrote (or dictated), and
  whether the teacher read it back after the first entry;
- whether the case for notes and the preview of the closing review were
  given (briefly, without overclaiming);
- mid-session notes checkpoints, and how missing notes were handled
  (encouraged once, ignored, blocked, or nagged);
- whether the closing review happened, from notes or as a verbal teach-back;
- for dictation, whether the file matches what the learner typed, word for
  word;
- whether the teacher gave cues (a 3–5 item list at the close) rather than
  asking the learner to write down what they already said in chat;
- **cue leaks**: cues that state the conclusion instead of naming the moment
  or theme (apply the leading-question test to each cue);
- how many note entries are tied to specific moments or themes from the
  session versus generic summaries, and how much the notes overlap with the
  learner's own chat text (heavy overlap suggests transcription).

Useful personas: a **note-avoider** who only writes notes when it's easy and
clearly worth it, a **chat-only** learner who can't edit files, and a
**chatty** learner who answers thoroughly in chat and finds copying those
answers into notes redundant.

Rough targets: leading ≤ 15%, answerable without understanding ≤ 10%, no
railroad runs, and few cue passes. Check that the struggling persona still
gets worked examples and doesn't stall: fixing leading questions must not
turn into stonewalling.

When judging misconception repair, count a repair as self-reasoned only if
the question before the learner's insight did **not** hand over the decisive
step. Have the judge quote that question.

### Passive learners and take-home habits

For changes to the nodding-along tripwire or the take-home habits, also
report:

- runs of consecutive bare-agreement replies, whether and when the teacher
  changed its move (messages until the switch), and whether the switch
  question was open;
- false positives: a terse but engaged learner treated as disengaged;
- any remark that shames or calls out the learner;
- whether a vibe limit and a habit were named by the learner, how many extra
  closing messages they cost (target ≤ 2), and whether they are in the
  record;
- in two-session runs, whether the next session checked in on them.

Useful personas: a **nodder** who replies "ok" or "makes sense" unless asked
directly for a prediction or explanation, and a **terse** learner whose short
replies always carry a prediction or a reason (the false-positive control).
