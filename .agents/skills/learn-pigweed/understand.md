# Understand mode

Follow [`teaching_contract.md`](teaching_contract.md) throughout.

Understanding is a graph of claims that depend on each other. Find the
learner's **frontier** (what they already hold vs. what they don't), then
extend it one reasoning step at a time, checking each step before building on
it.

## Phase 0: Make the goal concrete (1–2 questions)

"Understand tokenization" can mean ten different things. Ask what the learner
will *do* with it: debug garbled logs, add it to a module, review a change.
The answer decides which parts of the map matter. Fold in the session setup
and notes file from the contract ("Starting every session").

## Phase 1: Build the concept map (silently)

Read the real source and docs. Write `~/.pw_learn/<topic>/map.yaml`:

```yaml
nodes:
  - id: token-is-hash
    claim: >-
      A token is a 32-bit hash of the string literal, computed at compile
      time.
    source: pw_tokenizer/public/pw_tokenizer/hash.h:40
    prereqs: [constexpr-basics]
    check: >-
      If you change one character in a log string, what happens to its token,
      and why?
    misconceptions:
      - The device computes the token at runtime.
    state: unknown  # unknown | covered | fragile | held | misconception
    evidence: ""    # learner quote or result that justifies the state
```

- Use 8–20 nodes, and only what the goal needs; coverage is not the goal.
- **Include the subsystem's failure modes and limits** (collisions, stale
  databases, size limits, what can't be done) whenever the goal involves
  debugging or production use. That is where real bugs and misconceptions
  live.
- Make each node a single claim small enough to teach in one step.
- When you teach a node, use its `check` and `misconceptions` as written;
  don't swap in an easier check.
- If `map.yaml` already exists from an earlier session, merge into it and keep
  the existing states.

## Phase 2: Probe (about 3–6 questions)

- One question at a time. **Start high:** open near the hardest thing the goal
  needs and let misses walk you down.
- Prefer **open "walk me through what you think happens" prompts** to
  quizzes; they surface several misconceptions at once. A single prompt like
  "When `PW_LOG_INFO("Voltage: %d", v)` runs, what bytes leave the device, and
  how does the host turn them back into text?" often reveals two or three.
- Correct and confident: jump up. Correct but hesitant: mark fragile. Wrong:
  probe once more to tell a slip from a misconception.
- Update node states (with evidence). Stop probing when you know where to
  start teaching.
- Show the learner a short plan: 3–6 landmarks from where they are to the
  goal. Don't show them the YAML.

## Phase 3: Teach the frontier

For each frontier node (all prerequisites held):

1. **Predict:** pose the motivating problem as a constraint, not a conclusion
   ("strings take 40 bytes of flash and you only want to send 4 bytes per
   log; what could you send instead?"). Let them attempt the discovery.
2. **Teach one step:** resolve their prediction. Be Socratic if they can
   reason their way there, and expository (short and clear) if they can't.
   Socratic means *they* make the reasoning steps, not that you split the
   reasoning into a chain of one-answer questions. Show at most 15 lines of
   real code as evidence, with its path. Introduce one new idea only.
3. **Check:** ask one question that requires *using* the idea in a case you
   haven't just walked through (apply, predict, debug), never "what is X?"
   and never a restatement of your last message. A check can wait a step and
   be folded into the next prediction. On a miss, diagnose and repair before
   moving on.
4. **Misconception nodes:** follow the refutation steps in the contract. Use
   an experiment when the claim is checkable.
5. Update the node's state and evidence.

Every 3–4 steps, ask one quick question about an earlier node in a new setting
(retrieval practice).

## Phase 4: Consolidate

1. **Teach-back:** "Explain the whole flow end to end, as if to a new
   teammate." Compare against the map and repair the top one or two gaps.
2. **Transfer:** a new scenario (e.g. "a customer sends a crash log from a
   release six months old; what do you need to decode it?").
3. **Notes and record:** follow "Closing every session" in the contract. Save
   `map.yaml` states and a short `record.md` with the misconception ledger and
   retrieval questions for next time (prefer fragile nodes).

## Anti-patterns

- Lecturing three facts, then asking one check.
- Checks that can be answered by pattern-matching your last sentence.
- Teaching nodes the probe showed they already hold.
- Narrating the plan ("Now, node 3...") or showing the YAML.
- Marking a node "held" because you explained it.
