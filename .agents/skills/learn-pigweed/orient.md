# Orient mode

Follow [`teaching_contract.md`](teaching_contract.md) throughout.

Use this mode when the request is too big or too vague to plan one session
from ("teach me the kernel", "what's the deal with facades?"), or when the
learner says they don't know where to start. Don't use it for scoped requests.

Learners new to a large area often don't know what they don't know, so they
can't choose what to learn. The goal of Orient is to fix that quickly and then
get out of the way. Within about 4–6 messages the learner should have:

1. a picture of the territory at the level of topics,
2. a sense of where they already are on it,
3. a short plan of session-sized units anchored on their real task,

and then **start the first unit in the same session**. A session that ends
with a plan and nothing learned is a failure.

## Phase 1: Anchor on the task (1–2 questions)

Ask what they need to *do* in the next few weeks, not what they want to learn
about: "write a service that talks to another process", "make my driver build
for both FreeRTOS and host", "review CLs in pw_async2". A junior can't scope
"the kernel", but the task scopes it for them. If there's no task (pure
curiosity), ask what would make them feel they "get it". Fold in the session
setup and notes file from the contract ("Starting every session").

## Phase 2: Build a coarse map (silently)

- If the area has an owner-written `learn_map.md`, use it; it beats anything
  you generate.
- Otherwise build one from the module docs (the `docs.rst` table of contents,
  plus overview, tour, guide, and design pages) and public headers.
- The map has 6–12 **topics**, not individual facts. Each topic is roughly one
  session (one page of notes), cites the doc or header that covers it, and
  lists its prerequisite topics.
- Mark the topics the learner's task actually needs. Often that's a small
  fraction of the area. If the task is to build something, include the
  existing modules that already solve part of it; they're often the first
  unit.
- Save it to `~/.pw_learn/<area>/plan.md`.

## Phase 3: Place the learner (at most 3 questions)

Ask open "what do you think happens when ..." questions on topics the task
needs, starting high. This is not a quiz: you only need a starting point.
Note which topics look held, fragile, or unknown, and any misconception that
surfaces (it goes in the ledger).

## Phase 4: Show the map and choose

- Show the map compactly (≤ 10 lines: a list or small ASCII graph). Mark
  "you are here" and "your task needs these".
- Propose 2–4 units in order and recommend one, with a one-line reason tied
  to the task. The learner picks; respect a different choice.
- Alternative for learners who want Explore-style practice: have them build
  the map themselves ("open the module's `docs.rst`, list the main pieces and
  how they connect"), then compare it with yours. That turns orientation into
  learning.

## Phase 5: Start the first unit

Switch to Understand or Explore for the chosen unit (ask if it's not obvious)
and teach it in the same session, following that mode. Close as the contract
describes; the learner record's "next session" pointer is the next unit in
`plan.md`.

## Later sessions

Read `plan.md` and the record, open with retrieval questions, and confirm the
next unit is still the right one. Plans are suggestions: renegotiate when the
task or the learner's curiosity changes.

## Anti-patterns

- A placement quiz of more than three questions.
- Ending the session with a plan and no teaching.
- A giant map, or one not grounded in the docs.
- Teaching the whole map at once instead of one unit.
- Forcing the plan on a learner who wants to go elsewhere.
