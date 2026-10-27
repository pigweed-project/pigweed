// Copyright 2026 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

// Kind-driven smoke harness (AGENTS.md invariants #15 and #16): every
// runnable mutation, view, and list command in the live RootCmd tree,
// including the wt family, runs once in a hermetic fixture (real git repo
// with a local origin, mock Gerrit / Buildbucket / Issue Tracker; see
// kind_fixture_test.go), and its output and cost are checked against the
// contract and default budgets of its declared ghish:kind. A new command gets
// all of this from one kindSmokeArgs entry; there is no per-command test to
// write.

package pw_ghish_test

import (
	"fmt"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/spf13/cobra"
	"pigweed.dev/pw_ghish"
	"pigweed.dev/pw_ghish/worktree"
)

// kindSmokeArgs is the invocation (the arguments after `gh`) the harness
// runs for every runnable mutation, view, and list command, keyed by command
// path. A command registered at several paths with one RunE under one name
// (aliases such as `view` / `pr view`, or `sense` / `pr sense` / `wt sense`)
// needs exactly one entry, under any of its paths. Target the fixture's
// canonical objects: change 12345 (also the current branch's change) and
// issue 345678.
//
// A runnable command with no entry fails TestKindContract_EveryCommandOptsIn.
var kindSmokeArgs = map[string][]string{
	// view
	"gh-ish auth":        {"auth"},
	"gh-ish auth status": {"auth", "status"},
	"gh-ish hook status": {"hook", "status"},
	"gh-ish issue view":  {"issue", "view", "345678"},
	"gh-ish pr diff":     {"pr", "diff", "12345"},
	"gh-ish pr view":     {"pr", "view", "12345"},
	"gh-ish run view":    {"run", "view", "12345"},
	"gh-ish sense":       {"sense"},
	"gh-ish wt audit":    {"wt", "audit"},
	// list
	"gh-ish issue list":   {"issue", "list"},
	"gh-ish issue status": {"issue", "status"},
	"gh-ish pr checks":    {"pr", "checks", "12345"},
	"gh-ish pr list":      {"pr", "list"},
	"gh-ish pr status":    {"pr", "status"},
	"gh-ish run list":     {"run", "list", "12345"},
	"gh-ish wt list":      {"wt", "list"},
	// mutation
	"gh-ish hook install":   {"hook", "install", "--git"},
	"gh-ish hook uninstall": {"hook", "uninstall", "--git"},
	"gh-ish issue close":    {"issue", "close", "345678"},
	"gh-ish issue comment":  {"issue", "comment", "345678", "--body", "Smoke comment."},
	"gh-ish issue create":   {"issue", "create", "--title", "Smoke issue", "--body", "Smoke body.", "--component", "1194524"},
	"gh-ish issue develop":  {"issue", "develop", "345678"},
	"gh-ish issue edit":     {"issue", "edit", "345678", "--title", "Smoke title"},
	"gh-ish issue reopen":   {"issue", "reopen", "345678"},
	"gh-ish pr checkout":    {"pr", "checkout", "12345"},
	"gh-ish pr cherry-pick": {"pr", "cherry-pick", "12345"},
	"gh-ish pr close":       {"pr", "close", "12345"},
	"gh-ish pr comment":     {"pr", "comment", "12345", "--body", "Smoke comment."},
	"gh-ish pr create":      {"pr", "create"},
	"gh-ish pr edit":        {"pr", "edit", "12345", "--add-hashtag", "smoke"},
	"gh-ish pr merge":       {"pr", "merge", "12345"},
	"gh-ish pr push":        {"pr", "push"},
	"gh-ish pr ready":       {"pr", "ready", "12345"},
	"gh-ish pr reopen":      {"pr", "reopen", "12345"},
	"gh-ish pr review":      {"pr", "review", "12345", "--approve"},
	"gh-ish run rerun":      {"run", "rerun", "12345", "-j", "linux-gn"},
	"gh-ish wt close":       {"wt", "close", "smoke"},
	"gh-ish wt gc":          {"wt", "gc"},
	"gh-ish wt init":        {"wt", "init"},
	"gh-ish wt next":        {"wt", "next", "smoke"},
	"gh-ish wt park":        {"wt", "park", "smoke"},
	"gh-ish wt use":         {"wt", "use", "smoke"},
}

// kindSmokeSetup lists unmeasured invocations that put state in place before
// a command's measured invocation, keyed like kindSmokeArgs. Entries starting
// with "git" run git directly in the fixture repository.
var kindSmokeSetup = map[string][][]string{
	"gh-ish hook uninstall": {{"hook", "install", "--git"}},
	"gh-ish issue reopen":   {{"issue", "close", "345678"}},
	// Cherry-pick the change onto main rather than onto itself.
	"gh-ish pr cherry-pick": {{"git", "checkout", "main"}},
	// A commit no Gerrit query finds, so `pr create` creates a change.
	"gh-ish pr create": {
		{"git", "checkout", "-b", "fresh", "main"},
		{"git", "commit", "--allow-empty", "-m", "pw_fresh: Add fresh change\n\nChange-Id: " + pw_ghish.KindSmokeFreshChangeID},
	},
	"gh-ish wt audit": {{"wt", "init"}},
	"gh-ish wt close": {{"wt", "init"}, {"wt", "use", "smoke"}},
	"gh-ish wt gc":    {{"wt", "init"}},
	"gh-ish wt list":  {{"wt", "init"}, {"wt", "use", "smoke"}},
	"gh-ish wt next":  {{"wt", "init"}, {"wt", "use", "smoke"}},
	"gh-ish wt park":  {{"wt", "init"}, {"wt", "use", "smoke"}},
	"gh-ish wt use":   {{"wt", "init"}},
}

// kindSmokeSkip lists commands that cannot run hermetically, keyed by command
// path, with the reason (at least minReasonLen characters). Every entry is a
// command with no output-contract or budget coverage, so keep it empty unless
// the fixture genuinely cannot host the command.
var kindSmokeSkip = map[string]string{}

// kindSmokeContractGaps lists commands whose output does not (yet) carry the
// Verdict:/Next: line their kind requires, keyed like kindSmokeArgs, with the
// reason. The decorative-noise, byte-budget, and perf checks still apply, and
// an entry whose command now satisfies its contract fails as stale. Commands
// whose stdout is a raw artifact declare OutputAnnotation: OutputRaw instead.
var kindSmokeContractGaps = map[string]string{}

// kindSmokeDuplicateSubprocesses allows a command, keyed like kindSmokeArgs,
// to construct the same subprocess (identical argv) more than once in one
// invocation, keyed by the argv, with the reason. A repeated identical git
// call is almost always lost memoization; an entry whose argv no longer
// repeats fails as stale. The detector compares argv only, not the working
// directory, so the same git call in two different worktrees also counts as
// a repeat.
var kindSmokeDuplicateSubprocesses = map[string]map[string]string{
	"gh-ish wt close": {"git rev-parse --verify origin/main": lostMemo},
	// ExecGitRunner.AddWorktree prunes before each `worktree add` (once per
	// new slot: 2 here) and ListWorktrees prunes before listing (once).
	"gh-ish wt init": {"git worktree prune": "prunes stale worktree entries before each slot's `worktree add` and again before `worktree list`"},
}

// lostMemo is the kindSmokeDuplicateSubprocesses reason for repeats that
// predate the harness and are scheduled to be memoized.
const lostMemo = "follow-up: lost memoization"

// kindDefaults is the default output and cost budget for a command kind.
type kindDefaults struct {
	// maxBytes caps the command's (scrubbed) output.
	maxBytes int
	// maxSubprocesses caps the commands constructed (≈ subprocesses spawned)
	// via ExecCommand / ExecCommandContext.
	maxSubprocesses int
	// maxRPCs caps Gerrit + Buildbucket + Issue Tracker requests.
	maxRPCs int
}

// kindSmokeDefaults are the default budgets for each checked kind, measured
// against the fixture with headroom on bytes (paths and ports are scrubbed
// first) and none on counts beyond the typical command of the kind. A
// command over a count default needs an exact pin (kindSmokeBudgetPins or
// perfBudgetPins) or a ghish:budget-exempt annotation.
var kindSmokeDefaults = map[string]kindDefaults{
	// Measured: largest receipt (with progress) ~790 B (wt init).
	pw_ghish.KindMutation: {maxBytes: 1024, maxSubprocesses: 4, maxRPCs: 8},
	// Measured: largest view ~740 B (wt audit).
	pw_ghish.KindView: {maxBytes: 2048, maxSubprocesses: 2, maxRPCs: 6},
	// Measured: largest list ~660 B (pr status).
	pw_ghish.KindList: {maxBytes: 2048, maxSubprocesses: 2, maxRPCs: 8},
}

// kindBudgetPin is the upper-bound cost of a command whose budget exceeds its
// kind's default count budget.
type kindBudgetPin struct {
	subprocesses, rpcs int
	// why explains the extra work.
	why string
}

// kindSmokeBudgetPins sets upper-bound subprocess and RPC budgets for commands
// whose cost in the kind smoke fixture exceeds their kind's default. An
// increase past the pin fails as a regression; being at or under the pin
// passes (exact ratchet pins live in perfBudgetPins in perfbudget_test.go for
// the hot commands). Raise a pin only with a CL description justifying the
// extra work.
var kindSmokeBudgetPins = map[string]kindBudgetPin{
	"gh-ish auth":           {subprocesses: 3, rpcs: 1, why: "resolves branch, shared config prefetch, and git-common-dir probe"},
	"gh-ish auth status":    {subprocesses: 3, rpcs: 1, why: "resolves branch, shared config prefetch, and git-common-dir probe"},
	"gh-ish issue list":     {subprocesses: 3, rpcs: 1, why: "resolves branch, shared config prefetch, and HEAD diff-tree for commit context"},
	"gh-ish issue develop":  {subprocesses: 6, rpcs: 2, why: "resolves default branch, verifies target branch, and creates/switches branch"},
	"gh-ish pr checkout":    {subprocesses: 6, rpcs: 1, why: "fetches patchset ref and checks out local branch"},
	"gh-ish pr cherry-pick": {subprocesses: 6, rpcs: 1, why: "fetches patchset ref and cherry-picks onto current branch"},
	// The push pipeline validates the commit stack, resolves the target
	// branch, and pushes.
	"gh-ish pr create": {subprocesses: 12, rpcs: 1, why: "push pipeline: stack validation, target branch resolution, existing-change check, push"},
	"gh-ish pr push":   {subprocesses: 10, rpcs: 1, why: "push pipeline: stack validation and push"},
	// Worktree management is git work by definition.
	"gh-ish wt close": {subprocesses: 7, rpcs: 0, why: "inspects the slot (log, rev-list, status) and detaches it"},
	"gh-ish wt init":  {subprocesses: 7, rpcs: 0, why: "creates one git worktree per slot and prunes"},
	"gh-ish wt park":  {subprocesses: 5, rpcs: 0, why: "inspects the slot and detaches it"},
	"gh-ish wt use":   {subprocesses: 7, rpcs: 0, why: "collision checks, branch creation, worktree bookkeeping"},
}

// kindSmokeCommand is one command registered at one or more paths (an alias
// group), in tree order.
type kindSmokeCommand struct {
	paths []string
	// kinds lists the ghish:kind of each path; an alias group must agree.
	kinds []string
	kind  string
	cmd   *cobra.Command
}

// aliasKey identifies an alias group: the same RunE code registered under the
// same command name (`view` / `pr view`, or `sense` / `pr sense` /
// `wt sense`). The name is part of the key because distinct commands built
// by one factory share a RunE code pointer.
type aliasKey struct {
	runE uintptr
	name string
}

// kindSmokeCommandsIn returns every runnable mutation, view, and list command
// under root, grouped into alias groups (stream and meta commands are exempt
// from single-shot contracts), in tree order. Runnable commands with Run but
// no RunE are reported by TestKindContract_EveryCommandOptsIn instead.
func kindSmokeCommandsIn(root *cobra.Command) []*kindSmokeCommand {
	groups := map[aliasKey]*kindSmokeCommand{}
	var cmds []*kindSmokeCommand
	walkCommands(root, func(c *cobra.Command) {
		if !declaresOwnRun(c) || c.RunE == nil {
			return
		}
		kind := pw_ghish.CommandKind(c)
		if _, checked := kindSmokeDefaults[kind]; !checked {
			return
		}
		key := aliasKey{runE: reflect.ValueOf(c.RunE).Pointer(), name: c.Name()}
		if kc, ok := groups[key]; ok {
			kc.paths = append(kc.paths, c.CommandPath())
			kc.kinds = append(kc.kinds, kind)
			return
		}
		kc := &kindSmokeCommand{paths: []string{c.CommandPath()}, kinds: []string{kind}, kind: kind, cmd: c}
		groups[key] = kc
		cmds = append(cmds, kc)
	})
	return cmds
}

// entry returns the path under which kc has a kindSmokeArgs entry, or "".
func (kc *kindSmokeCommand) entry(args map[string][]string) string {
	for _, p := range kc.paths {
		if _, ok := args[p]; ok {
			return p
		}
	}
	return ""
}

// checkOptIn requires every command in cmds to have exactly one args entry
// or a reasoned skip entry, and every args/skip key to name a command in
// cmds.
func checkOptIn(t testing.TB, cmds []*kindSmokeCommand, args map[string][]string, skip map[string]string) {
	t.Helper()
	const minReasonLen = 10
	known := map[string]bool{}
	for _, kc := range cmds {
		for i, k := range kc.kinds {
			if k != kc.kind {
				t.Errorf("aliases %q (%s) and %q (%s) share one RunE but declare different ghish:kind values; aliases must share one kind",
					kc.paths[0], kc.kind, kc.paths[i], k)
			}
		}
		var entries []string
		for _, p := range kc.paths {
			known[p] = true
			if _, ok := args[p]; ok {
				entries = append(entries, p)
			}
			if _, ok := skip[p]; ok {
				entries = append(entries, p+" (kindSmokeSkip)")
			}
		}
		switch {
		case len(entries) == 0:
			t.Errorf("%q (ghish:kind %s) has no kind smoke entry: add kindSmokeArgs[%q] = []string{...} in pw_ghish/kind_contract_test.go "+
				"with arguments that run it against the hermetic fixture (change 12345, issue 345678); the command then gets its kind's output "+
				"contract and default budgets automatically (%s)", kc.paths[0], kc.kind, kc.paths[0], newCommandChecklistHint)
		case len(entries) > 1:
			t.Errorf("aliases %v have several kind smoke entries %v; keep exactly one", kc.paths, entries)
		}
	}
	for p, reason := range skip {
		if len(strings.TrimSpace(reason)) < minReasonLen {
			t.Errorf("kindSmokeSkip[%q] = %q must explain why the command cannot run hermetically (at least %d characters)", p, reason, minReasonLen)
		}
	}
	for _, p := range mapKeys(args) {
		if !known[p] {
			t.Errorf("kindSmokeArgs[%q] names no runnable mutation/view/list command; delete or rename the entry", p)
		}
	}
	for _, p := range mapKeys(skip) {
		if !known[p] {
			t.Errorf("kindSmokeSkip[%q] names no runnable mutation/view/list command; delete or rename the entry", p)
		}
	}
}

func mapKeys[V any](m map[string]V) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	slices.Sort(keys)
	return keys
}

// TestKindContract_EveryCommandOptsIn is the forcing function: a new
// runnable mutation, view, or list command fails here until it has a
// kindSmokeArgs entry (and so is covered by TestKindContract_Commands).
func TestKindContract_EveryCommandOptsIn(t *testing.T) {
	cmds := kindSmokeCommandsIn(runtimeRoot())
	if len(cmds) == 0 {
		t.Fatal("found no mutation/view/list commands under RootCmd; the tree walk is broken")
	}
	checkOptIn(t, cmds, kindSmokeArgs, kindSmokeSkip)

	// Belt and braces for invariant #1: a runnable command whose action is
	// Run (not RunE) would silently escape this harness.
	walkCommands(runtimeRoot(), func(c *cobra.Command) {
		if declaresOwnRun(c) && c.RunE == nil {
			t.Errorf("%q is runnable but has no RunE; use RunE (AGENTS.md invariant #1) so the kind smoke harness can run it", c.CommandPath())
		}
	})

	entries := map[string]bool{}
	for _, kc := range cmds {
		if p := kc.entry(kindSmokeArgs); p != "" {
			entries[p] = true
		}
	}
	for _, table := range []struct {
		name string
		keys []string
	}{
		{"kindSmokeSetup", mapKeys(kindSmokeSetup)},
		{"kindSmokeContractGaps", mapKeys(kindSmokeContractGaps)},
		{"kindSmokeDuplicateSubprocesses", mapKeys(kindSmokeDuplicateSubprocesses)},
		{"kindSmokeBudgetPins", mapKeys(kindSmokeBudgetPins)},
	} {
		for _, p := range table.keys {
			if !entries[p] {
				t.Errorf("%s[%q] must be keyed by the command's kindSmokeArgs path; delete or rename the entry", table.name, p)
			}
		}
	}
}

// messageRecorder records Errorf messages instead of failing the test.
type messageRecorder struct {
	testing.TB
	msgs []string
}

func (r *messageRecorder) Helper() {}

func (r *messageRecorder) Errorf(format string, args ...any) {
	r.msgs = append(r.msgs, fmt.Sprintf(format, args...))
}

// installHermeticWT swaps RootCmd's `wt` subtree, whose manager is bound at
// init to the real $HOME pool, for one bound to a temporary pool whose
// primary repository is the fixture repository, for the rest of the test.
// It returns the temporary pool's projects directory and optionally registers
// the workspace integration adapter for the hermetic pool.
func installHermeticWT(t *testing.T, f *pw_ghish.KindSmokeFixture, integrate bool) string {
	t.Helper()
	root := pw_ghish.RootCmd
	var real *cobra.Command
	for _, c := range root.Commands() {
		if c.Name() == "wt" {
			real = c
		}
	}
	if real == nil {
		t.Fatal("RootCmd has no wt command; is pigweed.dev/pw_ghish/worktree imported?")
	}
	tmp := t.TempDir()
	projects := filepath.Join(tmp, "projects")
	store := worktree.NewStateStore(filepath.Join(tmp, "state", "worktrees.json"))
	if err := store.Save(worktree.NewEmptyState(filepath.Join(tmp, "slots"), projects, f.WorkDir, 2)); err != nil {
		t.Fatal(err)
	}
	mgr := worktree.NewManager(store, worktree.NewExecGitRunner(), nil, worktree.NoopIDEDriver{}, worktree.NoopGerritStatusProvider{})
	mgr.Now = func() time.Time { return time.Date(2026, 9, 15, 12, 0, 0, 0, time.UTC) }
	hermetic := worktree.NewCommand(mgr)
	root.RemoveCommand(real)
	root.AddCommand(hermetic)
	t.Cleanup(func() {
		root.RemoveCommand(hermetic)
		root.AddCommand(real)
	})
	if integrate {
		prev := pw_ghish.RegisteredWorkspaceIntegration
		pw_ghish.RegisteredWorkspaceIntegration = &worktree.WorkspaceIntegrationAdapter{
			ManagerFn: func() (*worktree.Manager, error) { return mgr, nil },
		}
		t.Cleanup(func() { pw_ghish.RegisteredWorkspaceIntegration = prev })
	}
	return projects
}

// perfPinnedCommands returns the alias group key of every command
// TestPerfBudget_Commands pins exactly (perfBudgetPins), resolved through
// Cobra so a pin on any alias path covers the group.
func perfPinnedCommands(t *testing.T, root *cobra.Command) map[aliasKey]string {
	t.Helper()
	pinned := map[aliasKey]string{}
	for _, args := range pw_ghish.PerfBudgetPinnedArgsForTest() {
		c, _, err := root.Find(args)
		if err != nil || c == root || c.RunE == nil {
			continue // e.g. `--help`
		}
		pinned[aliasKey{runE: reflect.ValueOf(c.RunE).Pointer(), name: c.Name()}] = strings.Join(args, " ")
	}
	if len(pinned) == 0 {
		t.Fatal("perfBudgetPins resolved to no commands; the lookup is broken")
	}
	return pinned
}

// checkMutationReceipt checks the structured receipt, which may follow
// progress lines (e.g. git fetch output), starting at the first line that
// begins with the ✓ glyph.
func checkMutationReceipt(t testing.TB, out string) {
	t.Helper()
	lines := strings.SplitAfter(out, "\n")
	for i, line := range lines {
		if strings.HasPrefix(line, pw_ghish.GlyphPass+" ") {
			pw_ghish.AssertMutationReceiptForTest(t, strings.Join(lines[i:], ""))
			return
		}
	}
	pw_ghish.AssertMutationReceiptForTest(t, out)
}

// checkKindContract applies kind's output contract to out:
//
//   - mutation: a PrintReceipt receipt ("✓ <Summary>" with a Next: line in
//     its block), after any progress lines;
//   - view: a Verdict: line (every current view leads with one; Next: is not
//     yet universal, e.g. pr view);
//   - list: a Next: line (every current list ends with one; Verdict: is not
//     yet universal, e.g. pr list).
func checkKindContract(t testing.TB, kind, out string) {
	t.Helper()
	switch kind {
	case pw_ghish.KindMutation:
		checkMutationReceipt(t, out)
	case pw_ghish.KindView:
		pw_ghish.AssertVerdictForTest(t, out)
	case pw_ghish.KindList:
		pw_ghish.AssertNextHintForTest(t, out)
	}
}

// TestKindContract_Commands runs every opted-in command in a fresh hermetic
// fixture and checks its kind's output contract, decorative-noise rule, byte
// budget, subprocess / RPC budgets, and that it repeats no request and no
// subprocess.
func TestKindContract_Commands(t *testing.T) {
	root := runtimeRoot()
	perfPinned := perfPinnedCommands(t, root)
	for _, kc := range kindSmokeCommandsIn(root) {
		name := kc.entry(kindSmokeArgs)
		if name == "" {
			continue // Reported by TestKindContract_EveryCommandOptsIn.
		}
		args := kindSmokeArgs[name]
		t.Run(strings.TrimPrefix(name, "gh-ish "), func(t *testing.T) {
			f := pw_ghish.NewKindSmokeFixture(t)
			installHermeticWT(t, f, false)
			for _, s := range kindSmokeSetup[name] {
				f.Setup(t, s)
			}
			res := f.Run(t, args)
			invocation := "gh " + strings.Join(args, " ")
			t.Logf("%s (%s, paths %v): %d subprocesses, %d RPCs, %d bytes", invocation, kc.kind, kc.paths, len(res.Subprocesses), res.RPCs, len(res.Out))
			if res.Err != nil {
				t.Fatalf("`%s` failed in the kind smoke fixture: %v\nOutput:\n%s\nFix the kindSmokeArgs/kindSmokeSetup entry or extend kind_fixture_test.go.", invocation, res.Err, res.Out)
			}
			// Hermeticity: no real network, and only git, against the
			// fixture's local repositories. (The fixture's PATH already
			// refuses to spawn anything else; this names the culprit.)
			for _, req := range res.RefusedRequests {
				t.Errorf("`%s` reached for the real network (%s); the kind smoke fixture must stay hermetic (route it to a mock server in kind_fixture_test.go)", invocation, req)
			}
			for _, call := range res.Subprocesses {
				if !strings.HasPrefix(call, "git ") {
					t.Errorf("`%s` tried to spawn a non-git process %q, which the fixture refuses; stub the tool in kind_fixture_test.go", invocation, call)
				}
			}

			checkOutputContract(t, kc, name, res.Out)
			f.AssertNoDuplicateRequests(t, invocation)
			checkDuplicateSubprocesses(t, name, invocation, res.Subprocesses)
			checkKindBudget(t, kc, name, invocation, res, perfPinned)
		})
	}
}

// checkOutputContract applies the kind's output contract (unless the command
// declares raw output or a recorded gap), the decorative-noise rule, and the
// kind's byte budget (unless raw).
func checkOutputContract(t *testing.T, kc *kindSmokeCommand, name, out string) {
	t.Helper()
	pw_ghish.AssertNoDecorativeNoiseForTest(t, out)
	if kc.cmd.Annotations[pw_ghish.OutputAnnotation] == pw_ghish.OutputRaw {
		t.Logf("Verdict:/Next: contract and byte budget skipped: %s=%s", pw_ghish.OutputAnnotation, pw_ghish.OutputRaw)
		return
	}
	pw_ghish.AssertByteBudgetForTest(t, out, kindSmokeDefaults[kc.kind].maxBytes)
	gap, ok := kindSmokeContractGaps[name]
	if !ok {
		checkKindContract(t, kc.kind, out)
		return
	}
	rec := &messageRecorder{TB: t}
	checkKindContract(rec, kc.kind, out)
	if len(rec.msgs) == 0 {
		t.Errorf("kindSmokeContractGaps[%q] is stale: the output now satisfies the %s contract; delete the entry", name, kc.kind)
		return
	}
	t.Logf("output contract gap (%s): %s", gap, strings.Join(rec.msgs, "; "))
}

// checkDuplicateSubprocesses fails if the invocation constructed the same
// subprocess (identical argv) more than once, unless the argv is allowed with
// a reason in kindSmokeDuplicateSubprocesses.
func checkDuplicateSubprocesses(t *testing.T, name, invocation string, calls []string) {
	t.Helper()
	const minReasonLen = 10
	counts := map[string]int{}
	for _, c := range calls {
		counts[c]++
	}
	allowed := kindSmokeDuplicateSubprocesses[name]
	for _, call := range mapKeys(counts) {
		if counts[call] < 2 {
			continue
		}
		reason, ok := allowed[call]
		if !ok {
			t.Errorf("%s: constructed %q %d times in one invocation (lost memoization?). Memoize it (see Config caches / GitClient) or, if the repeat is "+
				"necessary, allow it in kindSmokeDuplicateSubprocesses[%q] with a reason.", invocation, call, counts[call], name)
			continue
		}
		if len(strings.TrimSpace(reason)) < minReasonLen {
			t.Errorf("kindSmokeDuplicateSubprocesses[%q][%q] = %q needs a reason of at least %d characters", name, call, reason, minReasonLen)
		}
	}
}

// checkKindBudget applies the kind's default count budgets, unless the
// command has an upper-bound pin in kindSmokeBudgetPins, an exact pin in
// perfBudgetPins (not both), or carries a ghish:budget-exempt annotation.
func checkKindBudget(t *testing.T, kc *kindSmokeCommand, name, invocation string, res pw_ghish.KindSmokeResult, perfPinned map[aliasKey]string) {
	t.Helper()
	d := kindSmokeDefaults[kc.kind]
	subs, rpcs := len(res.Subprocesses), res.RPCs
	details := fmt.Sprintf("Subprocesses:\n  %s\nRPCs:\n  %s", strings.Join(res.Subprocesses, "\n  "), strings.Join(res.RPCRoutes, "\n  "))
	perfArgs, inPerf := perfPinned[aliasKey{runE: reflect.ValueOf(kc.cmd.RunE).Pointer(), name: kc.cmd.Name()}]
	if pin, ok := kindSmokeBudgetPins[name]; ok {
		if inPerf {
			t.Errorf("%s is pinned in both kindSmokeBudgetPins and perfBudgetPins (gh %s); keep only the perfBudgetPins entry", name, perfArgs)
		}
		checkPin(t, invocation, "subprocesses", subs, pin.subprocesses, details)
		checkPin(t, invocation, "RPCs", rpcs, pin.rpcs, details)
		return
	}
	if inPerf {
		t.Logf("default budget skipped: pinned exactly by perfBudgetPins (gh %s)", perfArgs)
		return
	}
	if reason, ok := kc.cmd.Annotations[pw_ghish.BudgetExemptAnnotation]; ok {
		t.Logf("default budget skipped: %s: %s", pw_ghish.BudgetExemptAnnotation, reason)
		return
	}
	if subs > d.maxSubprocesses || rpcs > d.maxRPCs {
		t.Errorf("%s: over the default %s budget: %d subprocesses (max %d), %d RPCs (max %d).\n%s\n\n"+
			"Budgets are deterministic counts (pw_ghish/AGENTS.md invariant #16). Prefer removing the work (memoize, batch, "+
			"parallelize inside one call). If it is inherent, pin the upper-bound counts in kindSmokeBudgetPins (or exact counts in perfBudgetPins), "+
			"or annotate the command with %s: \"<reason>\"; both are reviewed (%s).",
			invocation, kc.kind, subs, d.maxSubprocesses, rpcs, d.maxRPCs, details, pw_ghish.BudgetExemptAnnotation, newCommandChecklistHint)
	}
}

// checkPin fails if got exceeds the upper-bound pin max.
func checkPin(t *testing.T, invocation, what string, got, max int, details string) {
	t.Helper()
	if got > max {
		t.Errorf("%s: %s exceeded the pinned budget: got %d, max %d.\n%s\nRemove the extra work, or raise the pin in kindSmokeBudgetPins and justify it in the CL description.",
			invocation, what, got, max, details)
	}
}
