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

// JSON output contract (AGENTS.md invariant #15, developing.rst "JSON output
// contract"): every runnable command with a --json flag runs once in the kind
// smoke fixture (reusing its kindSmokeArgs entry, with --json appended) and
// its stdout must be
//
//   - exactly one JSON value, with nothing (receipts, warnings, progress, ANSI
//     escapes) before or after it; stderr may carry anything;
//   - free of glyphs and other decoration in enum-like values (see
//     jsonEnumKeys); free-form text is never inspected; and
//   - free of unexpected null values (empty slices and maps marshal as [] and
//     {}).

package pw_ghish_test

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"path/filepath"
	"reflect"
	"regexp"
	"slices"
	"strings"
	"testing"

	"github.com/spf13/cobra"
	"github.com/spf13/pflag"
	"pigweed.dev/pw_ghish"
)

// jsonEnumKeys and jsonEnumKeySuffixes name the enum-like JSON keys whose
// string values (and the string elements of their array values) must match
// jsonEnumValue: machine-readable states that scripts and agents compare
// against (`"status": "READY_TO_LAND"`), never display text
// (`"● READY_TO_LAND"`). A key is enum-like if it equals one of jsonEnumKeys
// or ends with one of jsonEnumKeySuffixes. Every other key (summary, title,
// subject, message, details, description, body, ...) is free-form and never
// inspected, so text may legitimately carry glyphs or emoji. Glyphs belong in
// text output only.
//
// If a free-form value lands under an enum-like key, rename the key (e.g.
// status_text) rather than exempting it here. "verdict" is deliberately
// absent: `pr view` and `pr status` report their verdict as a human phrase
// ("IN REVIEW (waiting on Code-Review +2)"), the same one the text Verdict:
// line prints.
var (
	jsonEnumKeys = []string{
		"status", "state", "bucket", "kind", "residency", "mode", "modality",
		"type", "priority", "severity", "stateReason", "rebase_verdict",
	}
	jsonEnumKeySuffixes = []string{"_status", "_state", "Status", "State", "_badge", "Badge"}
	jsonEnumValue       = regexp.MustCompile(`^[A-Za-z0-9_.-]*$`)
)

// isJSONEnumKey reports whether key is enum-like (see jsonEnumKeys).
func isJSONEnumKey(key string) bool {
	if slices.Contains(jsonEnumKeys, key) {
		return true
	}
	return slices.ContainsFunc(jsonEnumKeySuffixes, func(s string) bool { return strings.HasSuffix(key, s) })
}

// jsonEnumViolations returns every string value of an enum-like key in v
// (at any depth), and every string element of an array under such a key,
// that does not match jsonEnumValue, as "path: value".
func jsonEnumViolations(v any) []string {
	var out []string
	check := func(path string, v any) {
		switch x := v.(type) {
		case string:
			if !jsonEnumValue.MatchString(x) {
				out = append(out, fmt.Sprintf("%s: %q", path, x))
			}
		case []any:
			for _, e := range x {
				if s, ok := e.(string); ok && !jsonEnumValue.MatchString(s) {
					out = append(out, fmt.Sprintf("%s[]: %q", path, s))
				}
			}
		}
	}
	var walk func(path string, v any)
	walk = func(path string, v any) {
		switch x := v.(type) {
		case map[string]any:
			for _, k := range mapKeys(x) {
				child := joinJSONPath(path, k)
				if isJSONEnumKey(k) {
					check(child, x[k])
				}
				walk(child, x[k])
			}
		case []any:
			for _, e := range x {
				walk(path+"[]", e)
			}
		}
	}
	walk("", v)
	return out
}

// jsonNullablePaths lists paths (see jsonNullPaths) that may legitimately
// be JSON null, with the reason. A null almost always means a nil Go slice or
// map that should marshal as [] or {}, which forces every caller to handle
// two shapes for "empty", so any other null fails. Keep it empty unless null
// carries meaning distinct from an empty value.
var jsonNullablePaths = map[string]string{}

// jsonNullPaths walks v (composed of map[string]any, []any, and scalar JSON
// values) and returns sorted, deduplicated paths where a value is nil and not
// allowed by jsonNullablePaths.
func jsonNullPaths(v any) []string {
	var out []string
	var walk func(path string, v any)
	walk = func(path string, v any) {
		label := path
		if label == "" {
			label = "(root)"
		}
		if v == nil {
			if _, allowed := jsonNullablePaths[label]; !allowed {
				out = append(out, label)
			}
			return
		}
		switch x := v.(type) {
		case map[string]any:
			for k, c := range x {
				walk(joinJSONPath(path, k), c)
			}
		case []any:
			for _, e := range x {
				walk(path+"[]", e)
			}
		}
	}
	walk("", v)
	slices.Sort(out)
	return slices.Compact(out)
}

// decodeSingleJSON decodes stdout as exactly one JSON value (numbers kept as
// json.Number), failing if anything but whitespace precedes or follows it.
func decodeSingleJSON(stdout string) (any, error) {
	dec := json.NewDecoder(strings.NewReader(stdout))
	dec.UseNumber()
	var v any
	if err := dec.Decode(&v); err != nil {
		return nil, fmt.Errorf("stdout is not a JSON value: %w", err)
	}
	var extra any
	if err := dec.Decode(&extra); !errors.Is(err, io.EOF) {
		rest := stdout[dec.InputOffset():]
		return nil, fmt.Errorf("stdout carries more than one JSON value (text after the first value: %q)", truncate(strings.TrimSpace(rest), 200))
	}
	return v, nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "…"
}

func joinJSONPath(path, key string) string {
	if path == "" {
		return key
	}
	return path + "." + key
}

// jsonCommand is one --json-capable alias group with its kind smoke entry.
type jsonCommand struct {
	kc *kindSmokeCommand
	// entry is kc's kindSmokeArgs key.
	entry string
	// fieldList is true for gh-compatible `--json <fields>` flags (string
	// valued); false for ghish-only boolean `--json` flags.
	fieldList bool
}

// jsonFlag returns c's --json flag, local or inherited, or nil.
func jsonFlag(c *cobra.Command) *pflag.Flag {
	if f := c.Flags().Lookup("json"); f != nil {
		return f
	}
	return c.InheritedFlags().Lookup("json")
}

// jsonCommandsIn returns every runnable command under root with a --json
// flag, one per alias group, in tree order, and the problems that keep a
// command out of the contract (no kind smoke entry, or a kind the harness
// does not run). Stream commands are exempt: they emit a sequence, not one
// value.
func jsonCommandsIn(root *cobra.Command) ([]jsonCommand, []string) {
	groups := map[aliasKey]*kindSmokeCommand{}
	for _, kc := range kindSmokeCommandsIn(root) {
		groups[aliasKey{runE: reflect.ValueOf(kc.cmd.RunE).Pointer(), name: kc.cmd.Name()}] = kc
	}
	var cmds []jsonCommand
	var problems []string
	seen := map[*kindSmokeCommand]bool{}
	walkCommands(root, func(c *cobra.Command) {
		if !declaresOwnRun(c) || c.RunE == nil {
			return
		}
		f := jsonFlag(c)
		if f == nil {
			return
		}
		kind := pw_ghish.CommandKind(c)
		if kind == pw_ghish.KindStream {
			return
		}
		kc, ok := groups[aliasKey{runE: reflect.ValueOf(c.RunE).Pointer(), name: c.Name()}]
		if !ok {
			problems = append(problems, fmt.Sprintf("%q has a --json flag but ghish:kind %q, which the kind smoke harness does not run; "+
				"the JSON output contract (json_contract_test.go) cannot check it. Make it a mutation/view/list command with a kindSmokeArgs entry, "+
				"or extend the JSON contract harness", c.CommandPath(), kind))
			return
		}
		if seen[kc] {
			return
		}
		seen[kc] = true
		entry := kc.entry(kindSmokeArgs)
		if entry == "" {
			problems = append(problems, fmt.Sprintf("%q has a --json flag but no kindSmokeArgs entry: add kindSmokeArgs[%q] in pw_ghish/kind_contract_test.go; "+
				"TestJSONContract_Commands reuses it with --json appended to check the JSON output contract (%s)", kc.paths[0], kc.paths[0], newCommandChecklistHint))
			return
		}
		cmds = append(cmds, jsonCommand{kc: kc, entry: entry, fieldList: f.Value.Type() != "bool"})
	})
	return cmds, problems
}

// TestJSONContract_EveryJSONCommandOptsIn is the forcing function: a new
// --json command fails here until the harness can run it.
func TestJSONContract_EveryJSONCommandOptsIn(t *testing.T) {
	cmds, problems := jsonCommandsIn(runtimeRoot())
	if len(cmds) == 0 {
		t.Fatal("found no --json commands under RootCmd; the tree walk is broken")
	}
	for _, p := range problems {
		t.Error(p)
	}
}

// jsonFieldProbe is a field name no command offers; requesting it makes a
// `--json <fields>` command report its full field set.
const jsonFieldProbe = "ghish_json_contract_probe"

// jsonCase is one invocation the JSON contract runs and snapshots.
type jsonCase struct {
	// name names the subtest and the snapshot
	// (testdata/golden/json_shape/<name>.expected).
	name string
	// entry is the kindSmokeArgs key; its kindSmokeSetup runs first.
	entry string
	// args is the invocation without --json.
	args []string
	// fieldList marks a gh-compatible `--json <fields>` flag.
	fieldList bool
	// setup lists extra unmeasured invocations run after kindSmokeSetup.
	setup [][]string
	// workspace registers the `wt` workspace integration (as a real build
	// does), so commands such as sense and issue develop see the pool.
	workspace bool
	// project, if set, runs the command from that `wt` project's symlink
	// (implies workspace).
	project string
	// prepare adjusts the fixture (e.g. failing CI) before the run.
	prepare func(*testing.T, *pw_ghish.KindSmokeFixture)
	// wantExitCode, if nonzero, is the exit code the command must fail with
	// (e.g. a failing CI verdict); stdout is still held to the contract.
	wantExitCode int
	// wantStderr, if set, must appear on stderr (e.g. failure reports that
	// --json moves off stdout).
	wantStderr string
}

// jsonSmokeTweaks adjusts the default case of a --json command, keyed like
// kindSmokeArgs, mostly to populate optional (omitempty or conditionally
// empty) fields so the shape snapshot covers them. Only setup, args,
// workspace, and project are read. Fields a fixture cannot populate are
// absent from the snapshot: a shape reflects the fixture, not every possible
// payload.
var jsonSmokeTweaks = map[string]jsonCase{
	// A parked project, and a mounted one linked to an issue and holding a
	// lease: parked_projects[], issue_id, and active_leases.
	"gh-ish wt list": {setup: [][]string{
		{"wt", "use", "shelved"},
		{"wt", "park", "shelved"},
		{"wt", "use", "--issue", "345678", "--agent", "json-contract"},
	}},
	// Inside a pool-managed project, with the fleet and a target tracked in
	// another slot: worktree.current_status from the pool, the fleet lists,
	// and target.existing_worktree.
	"gh-ish sense": {
		args:    []string{"sense", "b/345678", "--fleet"},
		setup:   [][]string{{"wt", "init"}, {"wt", "use", "smoke"}, {"wt", "use", "--issue", "345678"}},
		project: "smoke",
	},
}

// jsonExtraCases are invocations beyond each command's default case.
var jsonExtraCases = []jsonCase{
	{
		// Failure reports must go to stderr under --json.
		name:         "pr_checks_failing",
		entry:        "gh-ish pr checks",
		args:         []string{"pr", "checks", "12345", "--log-failed"},
		fieldList:    true,
		prepare:      func(t *testing.T, f *pw_ghish.KindSmokeFixture) { f.ServeFailingBuild(t) },
		wantExitCode: pw_ghish.ExitCodeFailure,
		wantStderr:   "ninja: build stopped",
	},
	{
		name:      "issue_develop_worktree",
		entry:     "gh-ish issue develop",
		args:      []string{"issue", "develop", "345678", "--worktree"},
		setup:     [][]string{{"wt", "init"}},
		workspace: true,
	},
}

// jsonCases returns the default case of every --json command (with its
// jsonSmokeTweaks) followed by jsonExtraCases.
func jsonCases(cmds []jsonCommand) []jsonCase {
	var cases []jsonCase
	for _, jc := range cmds {
		c := jsonCase{
			name:      strings.ReplaceAll(strings.TrimPrefix(jc.entry, "gh-ish "), " ", "_"),
			entry:     jc.entry,
			args:      kindSmokeArgs[jc.entry],
			fieldList: jc.fieldList,
		}
		if tw, ok := jsonSmokeTweaks[jc.entry]; ok {
			if tw.args != nil {
				c.args = tw.args
			}
			c.setup, c.workspace, c.project = tw.setup, tw.workspace, tw.project
		}
		cases = append(cases, c)
	}
	return append(cases, jsonExtraCases...)
}

// jsonCaseFixture builds a fresh kind smoke fixture prepared for c and
// returns it with the directory to run c from ("" for the repository).
func jsonCaseFixture(t *testing.T, c jsonCase) (*pw_ghish.KindSmokeFixture, string) {
	t.Helper()
	f := pw_ghish.NewKindSmokeFixture(t)
	projects := installHermeticWT(t, f, c.workspace || c.project != "")
	for _, s := range kindSmokeSetup[c.entry] {
		f.Setup(t, s)
	}
	for _, s := range c.setup {
		f.Setup(t, s)
	}
	if c.prepare != nil {
		c.prepare(t, f)
	}
	if c.project == "" {
		return f, ""
	}
	return f, filepath.Join(projects, c.project)
}

// jsonFieldSet asks a `--json <fields>` command for its full field set by
// requesting jsonFieldProbe, which it must reject with
// pw_ghish.UnknownJSONFieldsError.
func jsonFieldSet(t *testing.T, c jsonCase) []string {
	t.Helper()
	f, dir := jsonCaseFixture(t, c)
	args := append(slices.Clone(c.args), "--json="+jsonFieldProbe)
	res := f.RunSplitIn(t, dir, args)
	var unknown *pw_ghish.UnknownJSONFieldsError
	if !errors.As(res.Err, &unknown) || len(unknown.Available) == 0 {
		t.Fatalf("`gh %s` must reject the unknown field with a pw_ghish.UnknownJSONFieldsError listing its fields (as Renderer, "+
			"ValidateIssueJSONFields, and ValidateAuthStatusJSONFields do) so the JSON contract can request every field; got error %v\nstdout:\n%s",
			strings.Join(args, " "), res.Err, res.Stdout)
	}
	if !slices.IsSorted(unknown.Available) {
		t.Errorf("`gh %s`: UnknownJSONFieldsError.Available = %v is not sorted", strings.Join(args, " "), unknown.Available)
	}
	return unknown.Available
}

// TestJSONContract_Commands runs every --json case in a fresh kind smoke
// fixture, requesting every field of `--json <fields>` commands, and checks
// that stdout is exactly one JSON value without ANSI escapes, that enum-like
// values are plain ASCII identifiers, and that no field is null.
func TestJSONContract_Commands(t *testing.T) {
	cmds, _ := jsonCommandsIn(runtimeRoot()) // Problems: TestJSONContract_EveryJSONCommandOptsIn.
	for _, c := range jsonCases(cmds) {
		t.Run(c.name, func(t *testing.T) {
			args := slices.Clone(c.args)
			if c.fieldList {
				args = append(args, "--json="+strings.Join(jsonFieldSet(t, c), ","))
			} else {
				args = append(args, "--json")
			}
			invocation := "gh " + strings.Join(args, " ")
			f, dir := jsonCaseFixture(t, c)
			res := f.RunSplitIn(t, dir, args)
			switch got := pw_ghish.ExitCodeFor(res.Err); {
			case c.wantExitCode == 0 && res.Err != nil:
				t.Fatalf("`%s` failed in the kind smoke fixture: %v\nstdout:\n%s\nstderr:\n%s", invocation, res.Err, res.Stdout, res.Stderr)
			case c.wantExitCode != 0 && got != c.wantExitCode:
				t.Fatalf("`%s` exited %d (error %v), want %d\nstdout:\n%s\nstderr:\n%s", invocation, got, res.Err, c.wantExitCode, res.Stdout, res.Stderr)
			}
			if c.wantStderr != "" && !strings.Contains(res.Stderr, c.wantStderr) {
				t.Errorf("`%s`: stderr does not contain %q:\n%s", invocation, c.wantStderr, res.Stderr)
			}
			if strings.Contains(res.Stdout, "\x1b[") {
				t.Errorf("`%s`: JSON stdout contains ANSI escape codes:\n%s", invocation, res.Stdout)
			}
			v, err := decodeSingleJSON(res.Stdout)
			if err != nil {
				t.Fatalf("`%s`: %v\nUnder --json, stdout must be exactly one JSON value: send receipts, progress, and warnings to stderr "+
					"(cmd.ErrOrStderr()) or into the payload.\nstdout:\n%s", invocation, err, res.Stdout)
			}
			bad := jsonEnumViolations(v)
			slices.Sort(bad)
			if bad = slices.Compact(bad); len(bad) > 0 {
				t.Errorf("`%s`: enum-like values must be plain identifiers (%s), got:\n  %s\nRender glyphs and other display text in text "+
					"output only (see jsonEnumKeys in json_contract_test.go)", invocation, jsonEnumValue, strings.Join(bad, "\n  "))
			}
			if nulls := jsonNullPaths(v); len(nulls) > 0 {
				t.Errorf("`%s`: null values at %s; marshal empty slices and maps as [] and {} (initialize them, not nil) so callers see one "+
					"shape for \"empty\" (see jsonNullablePaths in json_contract_test.go)", invocation, strings.Join(nulls, ", "))
			}
		})
	}
}
