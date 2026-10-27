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

// This file enforces deterministic performance budgets for pw_ghish commands
// (AGENTS.md invariant #16). Budgets are COUNTS -- commands constructed
// (≈ subprocesses spawned) via ExecCommand/ExecCommandContext and HTTP
// requests observed by mock servers, pinned exactly -- never wall-clock time,
// so they cannot flake on loaded CI machines.

package pw_ghish

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

// countSubprocesses runs fn and returns the number of commands constructed
// (≈ subprocesses spawned) via ExecCommand/ExecCommandContext while it ran,
// plus each recorded "name args..." invocation in spawn order.
func countSubprocesses(t *testing.T, fn func()) (n int, calls []string) {
	t.Helper()
	var mu sync.Mutex
	restore := RecordSubprocessesForTesting(func(call string) {
		mu.Lock()
		calls = append(calls, call)
		mu.Unlock()
	})
	defer restore()
	fn()
	mu.Lock()
	defer mu.Unlock()
	return len(calls), slices.Clone(calls)
}

// requestCounter is HTTP middleware that records every request served by a
// mock httptest server: totals, per "METHOD /path" counts, exact
// method+URL+body repeats, and peak in-flight concurrency.
type requestCounter struct {
	mu       sync.Mutex
	total    int
	byRoute  map[string]int
	byExact  map[string]int
	calls    []string
	inFlight int
	peak     int
}

// countRequests wraps srv's handler with a requestCounter. It must be called
// before the code under test issues any request.
func countRequests(t *testing.T, srv *httptest.Server) *requestCounter {
	t.Helper()
	c := &requestCounter{byRoute: map[string]int{}, byExact: map[string]int{}}
	inner := srv.Config.Handler
	srv.Config.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var body []byte
		if r.Body != nil {
			body, _ = io.ReadAll(r.Body)
			r.Body = io.NopCloser(strings.NewReader(string(body)))
		}
		route := r.Method + " " + r.URL.Path
		exact := route
		if r.URL.RawQuery != "" {
			exact += "?" + r.URL.RawQuery
		}
		if len(body) > 0 {
			// pRPC multiplexes many logical calls over one POST URL, so the
			// body is part of a request's identity.
			exact += " " + string(body)
		}
		c.mu.Lock()
		c.total++
		c.byRoute[route]++
		c.byExact[exact]++
		c.calls = append(c.calls, exact)
		c.inFlight++
		c.peak = max(c.peak, c.inFlight)
		c.mu.Unlock()
		defer func() {
			c.mu.Lock()
			c.inFlight--
			c.mu.Unlock()
		}()
		inner.ServeHTTP(w, r)
	})
	return c
}

// reset forgets every request recorded so far, so a test can measure one
// invocation after unmeasured setup.
func (c *requestCounter) reset() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.total, c.inFlight, c.peak = 0, 0, 0
	c.byRoute = map[string]int{}
	c.byExact = map[string]int{}
	c.calls = nil
}

// Total returns the number of requests served.
func (c *requestCounter) Total() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.total
}

// Peak returns the maximum number of concurrently in-flight requests. This is
// informational only: handler latency in tests is scheduler-dependent, so
// assertions must not depend on it.
func (c *requestCounter) Peak() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.peak
}

// Calls returns every recorded request in arrival order.
func (c *requestCounter) Calls() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return slices.Clone(c.calls)
}

// RouteCounts returns sorted "N× METHOD /path" lines.
func (c *requestCounter) RouteCounts() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	var lines []string
	for route, n := range c.byRoute {
		lines = append(lines, fmt.Sprintf("%d× %s", n, route))
	}
	slices.Sort(lines)
	return lines
}

// assertNoDuplicateRequests fails if an identical method+URL(+body) request
// was issued more than once, which indicates lost memoization. Routes listed
// in paginated (as "METHOD /path") are exempt.
func (c *requestCounter) assertNoDuplicateRequests(t *testing.T, name string, paginated ...string) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	var dups []string
	for exact, n := range c.byExact {
		if n < 2 {
			continue
		}
		if slices.ContainsFunc(paginated, func(p string) bool { return strings.HasPrefix(exact, p) }) {
			continue
		}
		dups = append(dups, fmt.Sprintf("%d× %s", n, exact))
	}
	if len(dups) == 0 {
		return
	}
	slices.Sort(dups)
	t.Errorf("%s: identical requests issued more than once (lost memoization?):\n  %s\n"+
		"Memoize the lookup (see Config caches / ChangeContext) instead of re-fetching. "+
		"If the repeat is legitimate pagination, list the route as paginated in the budget test.",
		name, strings.Join(dups, "\n  "))
}

// assertBudget fails unless got equals want exactly. Count budgets are pinned
// to measured values: an increase is a regression, and a decrease means the
// budget is stale and must be lowered so the improvement is locked in. details
// (recorded subprocess or request lines) are printed on failure.
func assertBudget(t *testing.T, name string, got, want int, details []string) {
	t.Helper()
	if got == want {
		return
	}
	if got < want {
		t.Errorf("%s: budget is stale, lower it to %d (pinned %d).\nRecorded calls:\n  %s",
			name, got, want, strings.Join(details, "\n  "))
		return
	}
	t.Errorf("%s: perf budget exceeded: got %d, budget %d.\nRecorded calls:\n  %s\n\n"+
		"Perf budgets are deterministic counts (subprocesses constructed, HTTP requests), "+
		"never wall-clock (pw_ghish/AGENTS.md invariant #16). Prefer removing the extra work (memoize, "+
		"batch, or parallelize inside one call). If the increase is intended, raise the number in this "+
		"test and justify it in the CL description.",
		name, got, want, strings.Join(details, "\n  "))
}

// isolatePerfGitEnv isolates git from the developer's system and global config,
// HOME, and worktree pool so subprocess counts are identical on every machine.
func isolatePerfGitEnv(t *testing.T) {
	t.Helper()
	home := t.TempDir()
	globalCfg := filepath.Join(home, "gitconfig")
	if err := os.WriteFile(globalCfg, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", home)
	t.Setenv("XDG_CONFIG_HOME", home)
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	t.Setenv("GIT_CONFIG_GLOBAL", globalCfg)
	t.Setenv("GIT_DEFAULT_REF_FORMAT", "files")
}

// perfChangeID is the Change-Id trailer of HEAD in the perf fixture repo; the
// mock Gerrit change carries the same ID so branch-to-change inference works.
const perfChangeID = "I0123456789abcdef0123456789abcdef01234567"

// newPerfFixture builds a real git repository bound to the CLI (so every git
// invocation is a real, counted subprocess) on a feature branch whose HEAD
// carries perfChangeID, and a mock Gerrit/Buildbucket server answering every
// read RPC issued by the budgeted commands.
func newPerfFixture(t *testing.T) (gerritReqs, issueReqs *requestCounter) {
	t.Helper()
	isolatePerfGitEnv(t)
	newKindSmokeGitRepo(t)

	s := NewMockGerritServer(t)
	installKindSmokeGerrit(s,
		FakeBuild("87654321001", "linux-bazel", "SUCCESS"),
		FakeBuild("87654321002", "linux-gn", "FAILURE"),
	)

	issues := NewMockIssueTrackerServer(t)
	// Use the real (network-free) git-config quota project lookup instead of
	// the mock's canned provider so its git calls are counted too.
	resetQuotaProjectCacheForTest(t)
	old := NewIssueTrackerClientForCommand
	NewIssueTrackerClientForCommand = func(context.Context, *cobra.Command) (*IssueTrackerClient, error) {
		c := issues.Client()
		c.QuotaProjectProvider = ConfiguredIssueTrackerQuotaProject
		return c, nil
	}
	t.Cleanup(func() { NewIssueTrackerClientForCommand = old })
	seedKindSmokeIssue(issues)

	return countRequests(t, s.Server), countRequests(t, issues.Server)
}

// commandBudget pins the exact deterministic cost of one CLI invocation.
type commandBudget struct {
	name string
	args []string
	// subprocesses counts git (and any other) commands constructed (≈ spawned)
	// via ExecCommand/ExecCommandContext against a real git repo.
	subprocesses int
	// requests counts Gerrit REST and Buildbucket pRPC requests.
	requests int
	// issueRequests counts Issue Tracker requests.
	issueRequests int
	// allowErr permits a non-nil error from commands whose exit-code contract
	// reports state (e.g. `pr checks` exits non-zero when a check failed).
	allowErr bool
	// paginated lists "METHOD /path" prefixes that may legitimately repeat.
	paginated []string
}

// perfBudgetPins pins the exact subprocess and RPC counts of the hot
// read-only commands (run by TestPerfBudget_Commands). The kind smoke harness
// (kind_contract_test.go) reads it too: a command pinned here is exempt from
// its kind's default count budget there.
var perfBudgetPins = []commandBudget{
	// The --help fast path must never spawn a process or touch the network.
	{name: "help", args: []string{"--help"}},
	// One prefetch `git config -z --get-regexp`; change fetched in
	// parallel with SearchBuilds for the current and equivalent patchset.
	{name: "pr view", args: []string{"pr", "view", "12345"}, subprocesses: 1, requests: 3},
	{name: "pr diff", args: []string{"pr", "diff", "12345"}, subprocesses: 0, requests: 1},
	{name: "pr list", args: []string{"pr", "list", "--limit", "5"}, subprocesses: 2, requests: 1},
	// Two batched multi-q queries, self account, current-branch change
	// (+drafts/comments) and its builds.
	{name: "pr status", args: []string{"pr", "status"}, subprocesses: 3, requests: 8},
	{name: "pr checks", args: []string{"pr", "checks", "12345"}, subprocesses: 2, requests: 3, allowErr: true},
	{name: "run list", args: []string{"run", "list", "12345"}, subprocesses: 2, requests: 3},
	// issue view resolves the Gerrit remote (for profile detection) from
	// the shared config prefetch and current branch lookup.
	{name: "issue view", args: []string{"issue", "view", "345678"}, subprocesses: 2, issueRequests: 1},
	// Prefetch, current branch, symbolic-ref, one `git log` for HEAD's
	// SHA/time/message, one for-each-ref default-branch probe, rev-list,
	// status, worktree list.
	{name: "sense", args: []string{"sense"}, subprocesses: 9, requests: 6},
}

// TestPerfBudget_Commands runs perfBudgetPins against a real git repo and
// mock RPC servers. Budgets equal the measured values exactly: an increase
// fails as a regression and a decrease fails as a stale budget that must be
// lowered.
func TestPerfBudget_Commands(t *testing.T) {
	for _, b := range perfBudgetPins {
		t.Run(b.name, func(t *testing.T) {
			reqs, issueReqs := newPerfFixture(t)
			var out string
			var err error
			n, calls := countSubprocesses(t, func() {
				out, err = executeCommand(RootCmd, b.args...)
			})
			if err != nil && !b.allowErr {
				t.Fatalf("executeCommand(%v) failed: %v\nOutput:\n%s", b.args, err, out)
			}
			slices.Sort(calls)
			assertBudget(t, b.name+" subprocesses", n, b.subprocesses, calls)
			rpcDetails := append(reqs.RouteCounts(), fmt.Sprintf("(peak in-flight: %d)", reqs.Peak()))
			assertBudget(t, b.name+" Gerrit/Buildbucket RPCs", reqs.Total(), b.requests, rpcDetails)
			assertBudget(t, b.name+" Issue Tracker RPCs", issueReqs.Total(), b.issueRequests, issueReqs.RouteCounts())
			reqs.assertNoDuplicateRequests(t, b.name, b.paginated...)
			issueReqs.assertNoDuplicateRequests(t, b.name+" (Issue Tracker)", b.paginated...)
		})
	}
}

func perfReceipt() Receipt {
	return Receipt{
		Summary: "Change 12345 updated: patchset 3 uploaded.",
		URL:     "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/12345",
		Details: []ReceiptField{
			{Key: "Branch", Value: "main"},
			{Key: "Topic", Value: "perf-budgets"},
			{Key: "Reviewers", Value: "alice@google.com, bob@google.com"},
			{Key: "Labels", Value: "Commit-Queue+1"},
		},
		Next: "gh pr checks 12345 --watch --fail-fast",
	}
}

// perfBuilds returns 120 builds: 10 failures, 10 running, 100 passing.
func perfBuilds() []bbBuild {
	builds := make([]bbBuild, 0, 120)
	for i := range 120 {
		status := "SUCCESS"
		switch {
		case i%12 == 0:
			status = "FAILURE"
		case i%12 == 1:
			status = "STARTED"
		}
		builds = append(builds, FakeBuild(fmt.Sprintf("8765432%04d", i), fmt.Sprintf("pigweed-linux-builder-%03d", i), status,
			WithTimes("2026-05-26T20:23:35Z", "2026-05-26T20:31:07Z")))
	}
	return builds
}

func renderRunList(builds []bbBuild) {
	writeRunListTable(io.Discard, builds, 2, true)
}

// perfFailureReports returns 3 failure reports with 200-line log snippets.
func perfFailureReports() []FailureReport {
	reports := make([]FailureReport, 0, 3)
	for r := range 3 {
		var log strings.Builder
		for l := range 200 {
			fmt.Fprintf(&log, "[%d/200] CXX obj/pw_rpc/channel_%d.o: error: undefined reference to `pw::rpc::Channel::Send%d'\n", l+1, r, l)
		}
		reports = append(reports, FailureReport{
			BuildID:     fmt.Sprintf("8765432100%d", r),
			Builder:     fmt.Sprintf("pigweed-linux-bazel-%d", r),
			Status:      "FAILURE",
			BuildURL:    fmt.Sprintf("https://ci.chromium.org/b/8765432100%d", r),
			FailedStep:  "compile",
			StepSummary: "ninja execution failed",
			LogName:     "stdout",
			LogSnippet:  log.String(),
			FullLogURL:  fmt.Sprintf("https://logs.chromium.org/logs/pigweed/buildbucket/8765432100%d/+/u/compile/stdout", r),
		})
	}
	return reports
}

// perfCommentForest returns 10 files × 3 threads × (root + 2 replies) of
// published comments plus one draft reply per file.
func perfCommentForest() (published, drafts map[string][]gerrit.CommentInfo) {
	published = map[string][]gerrit.CommentInfo{}
	drafts = map[string][]gerrit.CommentInfo{}
	base := time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC)
	unresolved := true
	for f := range 10 {
		path := fmt.Sprintf("pw_rpc/channel_%d.cc", f)
		for th := range 3 {
			root := fmt.Sprintf("f%d-t%d", f, th)
			for r := range 3 {
				c := gerrit.CommentInfo{
					ID:         fmt.Sprintf("%s-c%d", root, r),
					Line:       10 + th*20,
					PatchSet:   2,
					Unresolved: &unresolved,
					Author:     gerrit.AccountInfo{Email: fmt.Sprintf("reviewer%d@example.com", r)},
					Message:    fmt.Sprintf("Comment %d on thread %d: please check the buffer bounds before copying.", r, th),
					Updated:    timeTimestamp(base.Add(time.Duration(f*100+th*10+r) * time.Minute)),
				}
				if r > 0 {
					c.InReplyTo = fmt.Sprintf("%s-c%d", root, r-1)
				}
				published[path] = append(published[path], c)
			}
		}
		drafts[path] = []gerrit.CommentInfo{{
			ID:        fmt.Sprintf("f%d-draft", f),
			InReplyTo: fmt.Sprintf("f%d-t0-c2", f),
			Line:      10,
			PatchSet:  2,
			Message:   "Done.",
			Updated:   timeTimestamp(base.Add(48 * time.Hour)),
		}}
	}
	return published, drafts
}

// The benchmarks below are informational (not budgets); use them to compare
// ns/op and B/op locally:
//
//   go test ./pw_ghish -run '^$' -bench . -benchmem

func BenchmarkFormatReceipt(b *testing.B) {
	receipt := perfReceipt()
	b.ReportAllocs()
	for b.Loop() {
		_ = FormatReceipt(io.Discard, receipt)
	}
}

func BenchmarkRunListRender120(b *testing.B) {
	builds := perfBuilds()
	b.ReportAllocs()
	for b.Loop() {
		renderRunList(builds)
	}
}

func BenchmarkFormatFailureReports(b *testing.B) {
	reports := perfFailureReports()
	b.ReportAllocs()
	for b.Loop() {
		_ = FormatFailureReports(reports)
	}
}

func BenchmarkFormatCommentForestWithOptions(b *testing.B) {
	published, drafts := perfCommentForest()
	opts := CommentFormatOptions{IncludeAll: true}
	b.ReportAllocs()
	for b.Loop() {
		_ = FormatCommentForestWithOptions(published, drafts, opts)
	}
}

// BenchmarkRootHelp measures in-process `gh --help` rendering (the fast path
// that must spawn zero subprocesses).
func BenchmarkRootHelp(b *testing.B) {
	setupRootCmdHelp()
	setupRootAliases()
	b.ReportAllocs()
	for b.Loop() {
		resetAllFlags(RootCmd)
		RootCmd.SetOut(io.Discard)
		RootCmd.SetErr(io.Discard)
		RootCmd.SetArgs([]string{"--help"})
		if err := RootCmd.Execute(); err != nil {
			b.Fatal(err)
		}
	}
	RootCmd.SetOut(nil)
	RootCmd.SetErr(nil)
	RootCmd.SetArgs(nil)
}
