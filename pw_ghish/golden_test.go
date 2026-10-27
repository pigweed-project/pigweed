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

package pw_ghish

// Golden snapshot tests: run real commands against the mock Gerrit / LUCI /
// Issue Tracker servers and compare their full output against
// testdata/golden/<case>.expected, so any change to user-visible output shows
// up as a reviewable diff. Every case also runs the design-language contract
// assertions from design_contract_test.go and a byte budget.
//
// Snapshot files use the .expected extension, which Pigweed's copyright-notice
// presubmit already exempts.
//
// To regenerate after an intentional output change (see goldenUpdateHint):
//
//   go test ./pw_ghish -run TestGolden -update
//   bazelisk run //pw_ghish:pw_ghish_test -- -test.run TestGolden -update

import (
	"context"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
	"time"
)

var updateGolden = flag.Bool("update", false, "rewrite pw_ghish/testdata/golden/*.expected from current command output")

const goldenUpdateHint = "go test ./pw_ghish -run TestGolden -update   (from the repository root; or: bazelisk run //pw_ghish:pw_ghish_test -- -test.run TestGolden -update)"

// goldenDir returns the directory holding golden files. Tests start with the
// package directory as their working directory under both `go test` and
// Bazel; it is captured at init (goldenStartDir) so fixtures that t.Chdir
// elsewhere still find the snapshots. Under `bazelisk run` the working
// directory is the runfiles tree, so updates are written to the source
// workspace instead.
func goldenDir() string {
	if ws := os.Getenv("BUILD_WORKSPACE_DIRECTORY"); ws != "" && *updateGolden {
		return filepath.Join(ws, "pw_ghish", "testdata", "golden")
	}
	return filepath.Join(goldenStartDir, "testdata", "golden")
}

// goldenStartDir is the test binary's starting working directory (the
// package directory), or "" (relative paths) if it cannot be determined.
var goldenStartDir, _ = os.Getwd()

// pinGoldenEnv makes command output independent of the caller's terminal and
// agent environment: color is forced off and every coding-agent variable is
// cleared, so human and agent runs take the same code path, and HOME points at
// an empty directory.
func pinGoldenEnv(t *testing.T) {
	t.Helper()
	t.Setenv("GHISH_COLOR", "never")
	t.Setenv("NO_COLOR", "")
	t.Setenv("TERM", "xterm")
	for _, k := range agentEnvVars {
		t.Setenv(k, "")
	}
	t.Setenv("HOME", t.TempDir())
}

var goldenLoopbackRe = regexp.MustCompile(`http://(127\.0\.0\.1|\[::1\]):\d+`)

// scrubGolden removes residual nondeterminism from command output. Fixtures
// use fixed timestamps, so this only normalizes the mock servers' random
// loopback ports and temporary directory prefixes.
func scrubGolden(out string) string {
	out = goldenLoopbackRe.ReplaceAllString(out, "http://mock-server")
	var tmpDirs []string
	for _, d := range []string{os.Getenv("TEST_TMPDIR"), os.TempDir()} {
		if d = strings.TrimRight(d, "/"); len(d) > 1 {
			tmpDirs = append(tmpDirs, d)
		}
	}
	// Longest first, so a nested TEST_TMPDIR wins over its parent.
	sort.Slice(tmpDirs, func(i, j int) bool { return len(tmpDirs[i]) > len(tmpDirs[j]) })
	for _, d := range tmpDirs {
		out = strings.ReplaceAll(out, d, "<TMP>")
	}
	return out
}

// assertGolden compares got (after scrubbing) with
// testdata/golden/<name>.expected, or rewrites the file when -update is set.
func assertGolden(t *testing.T, name, got string) {
	t.Helper()
	assertGoldenWithHint(t, name, got, goldenUpdateHint)
}

// assertGoldenWithHint is assertGolden for snapshot suites outside
// TestGolden, whose failure messages name updateHint (the command that
// regenerates that suite's snapshots) instead of goldenUpdateHint. name may
// contain a subdirectory of testdata/golden.
func assertGoldenWithHint(t *testing.T, name, got, updateHint string) {
	t.Helper()
	if *updateGolden && os.Getenv("TEST_SRCDIR") != "" && os.Getenv("BUILD_WORKSPACE_DIRECTORY") == "" {
		t.Fatalf("-update under `bazelisk test` would only rewrite the sandbox copy; run instead:\n  %s", updateHint)
	}
	got = scrubGolden(got)
	path := filepath.Join(goldenDir(), name+".expected")
	if *updateGolden {
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatalf("creating golden dir: %v", err)
		}
		if err := os.WriteFile(path, []byte(got), 0o644); err != nil {
			t.Fatalf("writing golden file %s: %v", path, err)
		}
		return
	}
	wantBytes, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("reading golden file %s: %v\nTo create it, run:\n  %s", path, err, updateHint)
	}
	want := string(wantBytes)
	if got == want {
		return
	}
	t.Errorf("output does not match %s (-want +got):\n%s\nIf the change is intentional, regenerate with:\n  %s",
		path, goldenLineDiff(want, got), updateHint)
}

// goldenLineDiff returns a minimal line-based diff of want and got, marking
// removed lines with "-" and added lines with "+", with one line of context.
func goldenLineDiff(want, got string) string {
	a := strings.Split(want, "\n")
	b := strings.Split(got, "\n")
	// Longest-common-subsequence table; golden files are small.
	lcs := make([][]int, len(a)+1)
	for i := range lcs {
		lcs[i] = make([]int, len(b)+1)
	}
	for i := len(a) - 1; i >= 0; i-- {
		for j := len(b) - 1; j >= 0; j-- {
			if a[i] == b[j] {
				lcs[i][j] = lcs[i+1][j+1] + 1
			} else {
				lcs[i][j] = max(lcs[i+1][j], lcs[i][j+1])
			}
		}
	}
	type op struct {
		kind byte
		line string
	}
	var ops []op
	i, j := 0, 0
	for i < len(a) || j < len(b) {
		switch {
		case i < len(a) && j < len(b) && a[i] == b[j]:
			ops = append(ops, op{' ', a[i]})
			i++
			j++
		case i < len(a) && (j == len(b) || lcs[i+1][j] >= lcs[i][j+1]):
			ops = append(ops, op{'-', a[i]})
			i++
		default:
			ops = append(ops, op{'+', b[j]})
			j++
		}
	}
	var sb strings.Builder
	lastPrinted := -1
	for k, o := range ops {
		if o.kind == ' ' {
			near := (k > 0 && ops[k-1].kind != ' ') || (k+1 < len(ops) && ops[k+1].kind != ' ')
			if !near {
				continue
			}
		}
		if lastPrinted >= 0 && k > lastPrinted+1 {
			sb.WriteString("  ...\n")
		}
		fmt.Fprintf(&sb, "%c %s\n", o.kind, o.line)
		lastPrinted = k
	}
	return sb.String()
}

// goldenFixedTime is the timestamp used by every golden fixture.
var goldenFixedTime = time.Date(2026, 9, 15, 12, 0, 0, 0, time.UTC)

// --- pr ---------------------------------------------------------------------

func TestGolden_PrView(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	change := map[string]any{
		"_number": 12345,
		"project": "pigweed/pigweed",
		"branch":  "main",
		"subject": "pw_rpc: Fix channel deadlock",
		"status":  "NEW",
		"created": "2026-09-14 10:00:00.000000000",
		"updated": "2026-09-15 12:00:00.000000000",
		"owner": map[string]any{
			"_account_id": 1, "name": "Test User", "email": "test@google.com",
		},
		"reviewers": map[string]any{
			"REVIEWER": []any{map[string]any{"_account_id": 2, "name": "Alice"}},
			"CC":       []any{map[string]any{"_account_id": 3, "name": "helper"}},
		},
		"attention_set": map[string]any{
			"2": map[string]any{"account": map[string]any{"_account_id": 2, "name": "Alice"}},
		},
		"topic":            "deadlock-fix",
		"hashtags":         []any{"rpc"},
		"current_revision": "rev3",
		"revisions": map[string]any{
			"rev3": map[string]any{
				"_number": 3,
				"commit": map[string]any{
					"subject": "pw_rpc: Fix channel deadlock",
					"message": "pw_rpc: Fix channel deadlock\n\nRelease the channel lock before invoking callbacks.\n\nBug: b/345678\nChange-Id: I12345\n",
				},
			},
		},
		"labels": map[string]any{
			"Code-Review": map[string]any{},
		},
	}
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, change)
	server.OnSearchBuilds(
		FakeBuild("101", "pigweed-linux", "SUCCESS", WithBucket("pigweed.try")),
		FakeBuild("102", "pigweed-mac", "SUCCESS", WithBucket("pigweed.try")),
		FakeBuild("103", "pigweed-lint", "FAILURE", WithBucket("pigweed.try")),
	)

	out, err := executeCommand(RootCmd, "pr", "view", "12345", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "pr_view", out)
}

func TestGolden_PrList(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	var changes []map[string]any
	for i, subj := range []string{
		"pw_rpc: Fix channel deadlock",
		"pw_log: Add tokenized backend",
		"pw_ghish: Add golden snapshots",
		"docs: Fix typo in style guide",
	} {
		changes = append(changes, map[string]any{
			"_number": 12340 + i,
			"project": "pigweed/pigweed",
			"branch":  "main",
			"subject": subj,
			"status":  "NEW",
			"updated": "2026-09-15 12:00:00.000000000",
			"owner": map[string]any{
				"name": "Dev " + string(rune('A'+i)), "email": fmt.Sprintf("dev%d@google.com", i), "username": fmt.Sprintf("dev%d", i),
			},
		})
	}
	server.OnJSON("GET", "/changes/", http.StatusOK, changes)

	out, err := executeCommand(RootCmd, "pr", "list")
	if err != nil {
		t.Fatalf("pr list failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "pr_list", out)
}

func TestGolden_PrStatus(t *testing.T) {
	pinGoldenEnv(t)
	server := newStatusMockServer(t)
	active := map[string]any{
		"change_id": "Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e",
		"_number":   472267,
		"project":   "pigweed/pigweed",
		"subject":   "Active Feature Subject",
		"status":    "NEW",
		"branch":    "main",
		"labels": map[string]any{
			"Code-Review": narrowLabel(-2, 2, 2),
			"Lint":        narrowLabel(-2, 2, 1),
		},
	}
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/Ic7c9a23e0aeef1af97a936896bfbf8a17cd5f96e*", http.StatusOK, active)
	server.On("GET", "/changes/*", func(w http.ResponseWriter, r *http.Request) {
		// pr status issues recent (-age:30d) section queries and then
		// counts older changes (age:30d); only the recent queries match.
		qs := r.URL.Query()["q"]
		batches := make([][]map[string]any, len(qs))
		for i, q := range qs {
			batches[i] = []map[string]any{}
			if !strings.Contains(q, "-age:30d") {
				continue
			}
			batches[i] = []map[string]any{{
				"_number": 88880 + i,
				"project": "pigweed/pigweed",
				"subject": fmt.Sprintf("Status section %d change", i+1),
				"status":  "NEW",
				"owner":   map[string]any{"name": "Alice"},
			}}
		}
		server.RespondJSON(w, http.StatusOK, batches)
	})
	mockGitForFeatureBranch(t)

	out, err := executeCommand(RootCmd, "pr", "status")
	if err != nil {
		t.Fatalf("pr status failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "pr_status", out)
}

func TestGolden_PrChecksAllPass(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Many passing checks"), WithCurrentPatchset(2))
	var builds []bbBuild
	for i := 1; i <= 72; i++ {
		builds = append(builds, FakeBuild(fmt.Sprintf("%d", 1000+i), fmt.Sprintf("pass-builder-%02d", i), "SUCCESS", WithBucket("pigweed.try")))
	}
	server.OnSearchBuilds(builds...)

	out, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if err != nil {
		t.Fatalf("pr checks failed: %v\nOutput: %s", err, out)
	}
	if strings.Contains(out, "pass-builder-01") {
		t.Errorf("expected passing checks to be folded, got:\n%s", out)
	}
	assertGolden(t, "pr_checks_all_pass", out)
}

func TestGolden_PrChecksFailingMix(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Failing checks mix"), WithCurrentPatchset(2))
	var builds []bbBuild
	for i := 1; i <= 12; i++ {
		builds = append(builds, FakeBuild(fmt.Sprintf("%d", 1000+i), fmt.Sprintf("pass-builder-%02d", i), "SUCCESS", WithBucket("pigweed.try")))
	}
	builds = append(builds,
		FakeBuild("2001", "pigweed-lint", "FAILURE", WithBucket("pigweed.try"), WithSummary("clang-format diff detected")),
		FakeBuild("2002", "pigweed-infra-1", "INFRA_FAILURE", WithBucket("pigweed.try"), WithSummary("bot died unexpectedly")),
		FakeBuild("2003", "pigweed-infra-2", "INFRA_FAILURE", WithBucket("pigweed.try"), WithSummary("bot died unexpectedly")),
	)
	server.OnSearchBuilds(builds...)

	out, err := executeCommand(RootCmd, "pr", "checks", "12345")
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Fatalf("ExitCodeFor(%v) = %d, want %d\nOutput: %s", err, got, ExitCodeFailure, out)
	}
	assertGolden(t, "pr_checks_failing_mix", out)
}

func TestGolden_PrCloseReceipt(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	server.OnJSON("POST", "/changes/12345/abandon", http.StatusOK, map[string]any{})

	out, err := executeCommand(RootCmd, "pr", "close", "12345")
	if err != nil {
		t.Fatalf("pr close failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "pr_close_receipt", out)
}

func TestGolden_PrCommentDraftReceipt(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/123/comments", http.StatusOK, map[string]any{})
	server.OnJSON("GET", "/changes/123/drafts", http.StatusOK, map[string]any{})
	server.OnJSON("PUT", "/changes/123/revisions/current/drafts", http.StatusOK, map[string]any{
		"id": "draft1", "path": "pw_rpc/channel.cc", "line": 42, "patch_set": 3,
	})

	out, err := executeCommand(RootCmd, "pr", "comment", "123", "--path", "pw_rpc/channel.cc", "--line", "42", "--message", "Release the lock first.", "--draft")
	if err != nil {
		t.Fatalf("pr comment --draft failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "pr_comment_draft_receipt", out)
}

// --- run --------------------------------------------------------------------

func TestGolden_RunList(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Run list flood"), WithCurrentPatchset(5))
	var builds []bbBuild
	for i := 1; i <= 118; i++ {
		builds = append(builds, FakeBuild(fmt.Sprintf("%d", 1000+i), fmt.Sprintf("pass-builder-%03d", i), "SUCCESS"))
	}
	builds = append(builds,
		FakeBuild("2001", "pigweed-linux", "FAILURE"),
		FakeBuild("2002", "pigweed-mac", "FAILURE"),
	)
	server.OnSearchBuilds(builds...)

	out, err := executeCommand(RootCmd, "run", "list", "12345")
	if err != nil {
		t.Fatalf("run list failed: %v\nOutput: %s", err, out)
	}
	if strings.Contains(out, "pass-builder-001") {
		t.Errorf("expected passing builds to fold to one line, got:\n%s", out)
	}
	assertGolden(t, "run_list", out)
}

func TestGolden_RunViewLogFailed(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Three failing builders"), WithCurrentPatchset(2))
	server.OnSearchBuilds(
		FakeBuild("101", "pigweed-linux", "SUCCESS"),
		FakeBuild("201", "pigweed-linux-clang", "FAILURE"),
		FakeBuild("202", "pigweed-linux-gcc", "FAILURE"),
		FakeBuild("203", "pigweed-mac", "FAILURE"),
	)

	logBody := func(prefix string) string {
		var lines []string
		for i := 1; i <= 200; i++ {
			lines = append(lines, fmt.Sprintf("[%03d/200] %s: compiling pw_rpc/channel_%03d.cc", i, prefix, i))
		}
		return strings.Join(lines, "\n") + "\n"
	}
	// Two builders share an identical compile log; the third differs.
	logs := map[string]string{
		"/logs/201": logBody("ninja"),
		"/logs/202": logBody("ninja"),
		"/logs/203": logBody("xcode"),
	}
	for p, body := range logs {
		server.OnString("GET", p, http.StatusOK, "text/plain", body)
	}
	details := map[string]*LUCIBuildDetails{}
	for id, builder := range map[string]string{"201": "pigweed-linux-clang", "202": "pigweed-linux-gcc", "203": "pigweed-mac"} {
		details[id] = FakeBuildDetails(id, builder, "FAILURE",
			WithSteps(
				FakeStep("setup", "SUCCESS"),
				FakeStep("compile", "FAILURE",
					WithStepSummary("ninja: build stopped: subcommand failed."),
					WithStepLogs(FakeLog("stdout", server.URL+"/logs/"+id)),
				),
			),
		)
	}
	server.OnGetBuild(details)

	out, err := executeCommand(RootCmd, "run", "view", "12345", "--log-failed")
	if err != nil {
		t.Fatalf("run view --log-failed failed: %v\nOutput: %s", err, out)
	}
	if !strings.Contains(out, "Same failure output as") {
		t.Errorf("expected identical logs to be deduplicated, got:\n%s", out)
	}
	assertGolden(t, "run_view_log_failed", out)
}

func TestGolden_RunRerunReceipt(t *testing.T) {
	pinGoldenEnv(t)
	server := NewMockGerritServer(t)
	server.OnDefaultChange(12345, WithSubject("Rerun Receipt"), WithCurrentPatchset(4))
	server.OnSearchBuilds(
		FakeBuild("111", "pigweed-linux", "FAILURE", WithBucket("pigweed.try")),
		FakeBuild("112", "pigweed-mac", "FAILURE", WithBucket("pigweed.try")),
		FakeBuild("113", "pigweed-lint", "SUCCESS", WithBucket("pigweed.try")),
	)
	restore := SetRerunExecForTesting(
		func(string) (string, error) { return "/usr/bin/bb", nil },
		func(context.Context, string, []string, io.Writer, io.Writer) error { return nil },
	)
	t.Cleanup(restore)

	out, err := executeCommand(RootCmd, "run", "rerun", "12345", "--failed")
	if err != nil {
		t.Fatalf("run rerun --failed failed: %v\nOutput: %s", err, out)
	}
	// The receipt follows per-builder "Rerunning check:" progress lines.
	assertGolden(t, "run_rerun_receipt", out)
}

// --- issue ------------------------------------------------------------------

// goldenIssue describes an issue seeded into the mock Issue Tracker with
// fixed timestamps.
type goldenIssue struct {
	ID       int64
	Status   string
	Priority string
	Title    string
	Assignee string // email; empty for unassigned
	Reporter string // email; empty for none
}

func seedGoldenIssue(srv *MockIssueTrackerServer, gi goldenIssue) {
	st := BuganizerState{
		ComponentID: 1194524,
		Type:        "BUG",
		Status:      gi.Status,
		Priority:    gi.Priority,
		Severity:    "S2",
		Title:       gi.Title,
	}
	if gi.Assignee != "" {
		st.Assignee = &BuganizerUser{EmailAddress: gi.Assignee}
	}
	if gi.Reporter != "" {
		st.Reporter = &BuganizerUser{EmailAddress: gi.Reporter}
	}
	srv.SeedIssue(&BuganizerIssue{
		IssueID:      FlexInt64(gi.ID),
		State:        st,
		CreatedTime:  goldenFixedTime,
		ModifiedTime: goldenFixedTime,
	}, "Detailed report for "+gi.Title+".")
}

func setupGoldenIssueEnv(t *testing.T) *MockIssueTrackerServer {
	t.Helper()
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	gitRunner := &MockGitRunner{}
	gitRunner.OnCommand("config --get user.email", "dev@google.com\n")
	SetupMockConfig(t, gitRunner)
	SetTestProfile(t, "pigweed")
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = nil
	t.Cleanup(func() { RegisteredWorkspaceIntegration = prevWT })
	return srv
}

func TestGolden_IssueView(t *testing.T) {
	pinGoldenEnv(t)
	srv := setupGoldenIssueEnv(t)
	seedGoldenIssue(srv, goldenIssue{ID: 345678, Status: "ASSIGNED", Priority: "P1", Title: "pw_rpc: Fix channel deadlock", Assignee: "owner@google.com", Reporter: "author@google.com"})

	out, err := executeCommand(RootCmd, "issue", "view", "345678")
	if err != nil {
		t.Fatalf("issue view failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "issue_view", out)
}

func TestGolden_IssueStatus(t *testing.T) {
	pinGoldenEnv(t)
	srv := setupGoldenIssueEnv(t)
	for i := int64(1); i <= 32; i++ {
		seedGoldenIssue(srv, goldenIssue{ID: 1000 + i, Status: "ASSIGNED", Priority: "P2", Title: fmt.Sprintf("Assigned bulk issue %02d", i), Assignee: "dev@google.com"})
	}
	seedGoldenIssue(srv, goldenIssue{ID: 2001, Status: "NEW", Priority: "P3", Title: "Reported by me", Reporter: "dev@google.com"})

	out, err := executeCommand(RootCmd, "issue", "status")
	if err != nil {
		t.Fatalf("issue status failed: %v\nOutput: %s", err, out)
	}
	if strings.Contains(out, "b/1011") {
		t.Errorf("expected issues beyond the first 10 to be folded, got:\n%s", out)
	}
	assertGolden(t, "issue_status", out)
}

func TestGolden_IssueList(t *testing.T) {
	pinGoldenEnv(t)
	srv := setupGoldenIssueEnv(t)
	seedGoldenIssue(srv, goldenIssue{ID: 101, Status: "NEW", Priority: "P2", Title: "First open issue"})
	seedGoldenIssue(srv, goldenIssue{ID: 102, Status: "ASSIGNED", Priority: "P1", Title: "Second open issue assigned", Assignee: "alice@google.com"})
	seedGoldenIssue(srv, goldenIssue{ID: 103, Status: "ACCEPTED", Priority: "P3", Title: "Third open issue accepted", Assignee: "bob@google.com"})
	seedGoldenIssue(srv, goldenIssue{ID: 104, Status: "FIXED", Priority: "P2", Title: "Closed issue (hidden by default)"})

	out, err := executeCommand(RootCmd, "issue", "list")
	if err != nil {
		t.Fatalf("issue list failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "issue_list", out)
}

func TestGolden_IssueCloseReceipt(t *testing.T) {
	pinGoldenEnv(t)
	srv := setupGoldenIssueEnv(t)
	seedGoldenIssue(srv, goldenIssue{ID: 300001, Status: "ASSIGNED", Priority: "P1", Title: "pw_foo: Receipt test", Assignee: "dev@google.com"})

	out, err := executeCommand(RootCmd, "issue", "close", "300001")
	if err != nil {
		t.Fatalf("issue close failed: %v\nOutput: %s", err, out)
	}
	assertGolden(t, "issue_close_receipt", out)
}
