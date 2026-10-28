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

// This file builds the hermetic environment the kind-driven smoke harness
// (kind_contract_test.go, external package) runs every command in: a real git
// repository with a local bare origin, and mock Gerrit / Buildbucket / Issue
// Tracker servers answering the common read and write endpoints for change
// 12345 and issue 345678. It lives in the internal package so it can reuse the
// golden and perf-budget fixtures; the exported identifiers are visible to
// kind_contract_test.go because both packages link into one test binary.

package pw_ghish

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

// KindSmokeChange and KindSmokeIssue are the canonical objects the kind smoke
// fixture serves; kindSmokeArgs entries should target them.
const (
	KindSmokeChange = 12345
	KindSmokeIssue  = 345678
	// KindSmokeFreshChangeID is a Change-Id no Gerrit query finds, for
	// commands that create a new change.
	KindSmokeFreshChangeID = "Ifeedfacefeedfacefeedfacefeedfacefeedface"
)

// KindSmokeFixture is one hermetic environment for one command invocation.
type KindSmokeFixture struct {
	// WorkDir is the fixture repository's working tree (bound as the CLI's
	// current directory). It is on branch "feature", one commit ahead of
	// origin/main, and that commit carries the Change-Id of change 12345.
	WorkDir string

	repo                  *testGitRepo
	gerrit                *MockGerritServer
	gerritReqs, issueReqs *requestCounter
	network               *loopbackOnlyTransport
}

// loopbackOnlyTransport is installed as http.DefaultTransport for the
// fixture's lifetime (and, by TestMain, for the whole unit test binary): it
// forwards requests to loopback hosts (the mock servers) and refuses and
// records every other request, so a command that reaches for the real network
// fails instead of silently escaping the fixture.
type loopbackOnlyTransport struct {
	inner http.RoundTripper
	// logRefusals also reports each refused request on stderr, so a refusal
	// is visible even when a best-effort caller swallows the error.
	logRefusals bool
	mu          sync.Mutex
	refused     []string
}

func (l *loopbackOnlyTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	if isLoopbackHost(r.URL.Host) {
		return l.inner.RoundTrip(r)
	}
	if r.Body != nil {
		_ = r.Body.Close()
	}
	l.mu.Lock()
	l.refused = append(l.refused, r.Method+" "+r.URL.String())
	l.mu.Unlock()
	if l.logRefusals {
		fmt.Fprintf(os.Stderr, "hermetic test: refusing non-loopback request to %s\n", r.URL)
	}
	return nil, fmt.Errorf("hermetic test: refusing non-loopback request to %s", r.URL)
}

// takeRefused returns and forgets the refused requests.
func (l *loopbackOnlyTransport) takeRefused() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	refused := l.refused
	l.refused = nil
	return refused
}

// KindSmokeResult is the captured output and deterministic cost of one
// measured invocation.
type KindSmokeResult struct {
	// Out is the combined stdout and stderr, with mock-server ports and
	// temporary directories scrubbed (as in golden files) so its size does
	// not depend on the machine.
	Out string
	Err error
	// Subprocesses lists every command constructed via ExecCommand /
	// ExecCommandContext, sorted.
	Subprocesses []string
	// RPCs is the total number of Gerrit, Buildbucket, and Issue Tracker
	// requests; RPCRoutes details them as sorted "N× METHOD /path" lines.
	RPCs      int
	RPCRoutes []string
	// RefusedRequests lists requests to non-loopback hosts, which the
	// fixture refuses; a hermetic command issues none.
	RefusedRequests []string
}

// NewKindSmokeFixture pins the environment (no color, no agent variables,
// isolated HOME and git config, files ref backend), builds the repository,
// and starts the mock servers. Everything is restored on test cleanup.
func NewKindSmokeFixture(t *testing.T) *KindSmokeFixture {
	t.Helper()
	// Fail closed on subprocesses: PATH holds only git, so any other tool
	// (gcloud, sso_client, luci-auth, a browser) fails to spawn, and the
	// working directory is empty, so a git call without an explicit Dir
	// cannot reach the developer's real repository.
	gitPath, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git binary not found in PATH")
	}
	binDir := t.TempDir()
	if err := os.Symlink(gitPath, filepath.Join(binDir, "git")); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", binDir)
	t.Setenv("BROWSER", "false")
	t.Chdir(t.TempDir())

	pinGoldenEnv(t)
	isolatePerfGitEnv(t)
	t.Setenv("GH_ISH_IDE_SYNC", "0")
	t.Setenv("CONVERSATION_ID", "")
	// Commits made by the fixture and by commands get fixed dates, and git's
	// version-dependent detached-HEAD advice stays out of the output.
	date := goldenFixedTime.Format(time.RFC3339)
	t.Setenv("GIT_AUTHOR_DATE", date)
	t.Setenv("GIT_COMMITTER_DATE", date)
	if err := os.WriteFile(os.Getenv("GIT_CONFIG_GLOBAL"), []byte("[advice]\n\tdetachedHead = false\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	// executeCommand's flag reset preserves --profile, and --verbose is a
	// package global, so values left by earlier tests would change which
	// code paths (and how many git calls) a command takes. Pin the defaults.
	SetTestProfile(t, "")
	prevVerbose := VerboseFlag
	VerboseFlag = false
	t.Cleanup(func() { VerboseFlag = prevVerbose })

	repo := newKindSmokeGitRepo(t)

	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = nil
	t.Cleanup(func() { RegisteredWorkspaceIntegration = prevWT })
	t.Cleanup(SetRerunExecForTesting(
		func(string) (string, error) { return "/usr/bin/bb", nil },
		func(context.Context, string, []string, io.Writer, io.Writer) error { return nil },
	))
	network := &loopbackOnlyTransport{inner: http.DefaultTransport}
	http.DefaultTransport = network
	t.Cleanup(func() { http.DefaultTransport = network.inner })

	s := NewMockGerritServer(t)
	WithHermeticAuth(t, AuthResolvers{
		LUCIToken:                func(context.Context) (string, string, error) { return "smoke-luci-token", "kind smoke fixture", nil },
		IssueTrackerToken:        func(context.Context) (string, string, error) { return "smoke-it-token", "kind smoke fixture", nil },
		IssueTrackerQuotaProject: func(context.Context, string) string { return "smoke-quota-project" },
	})
	installKindSmokeGerrit(s)

	issues := NewMockIssueTrackerServer(t)
	issues.Install(t)
	seedKindSmokeIssue(issues)

	return &KindSmokeFixture{
		repo:       repo,
		WorkDir:    repo.WorkDir,
		gerrit:     s,
		gerritReqs: countRequests(t, s.Server),
		issueReqs:  countRequests(t, issues.Server),
		network:    network,
	}
}

// newKindSmokeGitRepo creates a test git repo on branch "feature" carrying
// perfChangeID, pushed to origin as patchset 2 of change 12345, with
// DefaultGitRunner bound to a RealGitRunner for repo.WorkDir.
func newKindSmokeGitRepo(t *testing.T) *testGitRepo {
	t.Helper()
	repo := newTestGitRepo(t)
	repo.GitOutput("checkout", "-b", "feature")
	repo.StageFile("feature.txt", "feature\n")
	repo.GitOutput("commit", "-m", "pw_feature: Add feature\n\nBug: b/345678\nChange-Id: "+perfChangeID)
	// Publish the commit as patchset 2 of change 12345 so `pr checkout`
	// fetches from the local origin rather than a real Gerrit host.
	repo.GitOutput("push", "origin", "HEAD:refs/changes/45/12345/2")
	repo.BindToCLI()
	prevGit := DefaultGitRunner
	DefaultGitRunner = &RealGitRunner{Dir: repo.WorkDir}
	t.Cleanup(func() { DefaultGitRunner = prevGit })
	return repo
}

// seedKindSmokeIssue adds the canonical issue 345678 to issues.
func seedKindSmokeIssue(issues *MockIssueTrackerServer) *BuganizerIssue {
	return issues.SeedIssue(&BuganizerIssue{
		IssueID:      KindSmokeIssue,
		CreatedTime:  goldenFixedTime,
		ModifiedTime: goldenFixedTime,
		State: BuganizerState{
			ComponentID: 1194524,
			Type:        "BUG",
			Status:      "ASSIGNED",
			Priority:    "P1",
			Severity:    "S2",
			Title:       "pw_feature: Add feature",
			Reporter:    &BuganizerUser{EmailAddress: "author@google.com"},
			Assignee:    &BuganizerUser{EmailAddress: "test@example.com"},
		},
	}, "Detailed bug report body.")
}

// installKindSmokeGerrit registers the Gerrit REST and Buildbucket routes the
// smoke harness's commands use. Specific routes come before the
// OnDefaultChange wildcards, which answer every other GET for the change.
func installKindSmokeGerrit(s *MockGerritServer, builds ...bbBuild) {
	if len(builds) == 0 {
		builds = []bbBuild{
			FakeBuild("87654321001", "linux-bazel", "SUCCESS", WithBucket("try")),
			FakeBuild("87654321002", "linux-gn", "SUCCESS", WithBucket("try")),
		}
	}
	ok := map[string]any{}
	changeOpts := []func(map[string]any){
		WithSubject("pw_feature: Add feature"),
		WithLabels("Code-Review", "Presubmit-Verified"),
		WithChangeID(perfChangeID),
		func(ch map[string]any) {
			ch["current_revision"] = "rev2"
			ch["revisions"] = map[string]any{
				"rev1": map[string]any{"_number": 1, "kind": "REWORK", "ref": "refs/changes/45/12345/1"},
				"rev2": map[string]any{"_number": 2, "kind": "TRIVIAL_REBASE", "ref": "refs/changes/45/12345/2"},
			}
		},
	}
	change := DefaultMockChange(KindSmokeChange, changeOpts...)
	// Queries naming KindSmokeFreshChangeID find nothing, so `pr create`
	// from a commit carrying it creates a new change; every other query
	// finds change 12345, as OnDefaultChange's handler (registered after
	// this one) would answer.
	s.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
		qs := r.URL.Query()["q"]
		found := []map[string]any{change}
		if slices.ContainsFunc(qs, func(q string) bool { return strings.Contains(q, KindSmokeFreshChangeID) }) {
			found = []map[string]any{}
		}
		if len(qs) > 1 {
			batches := make([][]map[string]any, len(qs))
			for i := range batches {
				batches[i] = []map[string]any{}
			}
			batches[0] = found
			s.RespondJSON(w, http.StatusOK, batches)
			return
		}
		s.RespondJSON(w, http.StatusOK, found)
	})
	s.OnAccountSelf(1000000).
		OnSearchBuilds(builds...).
		OnJSON("GET", "/changes/12345/revisions/current/patch", http.StatusOK, `"`+base64.StdEncoding.EncodeToString([]byte(kindSmokePatch))+`"`).
		OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{"/COMMIT_MSG": ok}).
		OnJSON("GET", "/changes/*/comments", http.StatusOK, map[string][]gerrit.CommentInfo{}).
		OnJSON("GET", "/changes/*/drafts", http.StatusOK, map[string][]gerrit.CommentInfo{}).
		OnJSON("POST", "/changes/12345/abandon", http.StatusOK, DefaultMockChange(KindSmokeChange, append(changeOpts, WithStatus("ABANDONED"))...)).
		OnJSON("POST", "/changes/12345/restore", http.StatusOK, DefaultMockChange(KindSmokeChange, changeOpts...)).
		OnJSON("POST", "/changes/12345/ready", http.StatusOK, ok).
		OnJSON("POST", "/changes/12345/submit", http.StatusOK, DefaultMockChange(KindSmokeChange, append(changeOpts, WithStatus("MERGED"))...)).
		OnJSON("POST", "/changes/12345/hashtags", http.StatusOK, []string{"smoke"}).
		OnJSON("PUT", "/changes/12345/topic", http.StatusOK, "smoke").
		OnJSON("POST", "/changes/12345/revisions/current/cherrypick", http.StatusOK, DefaultMockChange(12346, WithBranch("release"))).
		OnJSON("POST", "/changes/12345/revisions/*/cherrypick", http.StatusOK, DefaultMockChange(12346, WithBranch("release"))).
		OnReview(KindSmokeChange).
		OnJSON("POST", "/changes/12345/revisions/*/review", http.StatusOK, ok).
		OnDefaultChange(KindSmokeChange, changeOpts...)
}

// kindSmokePatch is the unified diff the mock serves for change 12345: two
// files, so `pr diff` exercises its per-file rendering.
const kindSmokePatch = `From 2dc3d04 Mon Sep 17 00:00:00 2001
From: Test User <test@example.com>
Date: Tue, 15 Sep 2026 12:00:00 +0000
Subject: [PATCH] pw_feature: Add feature

Bug: b/345678
Change-Id: I0123456789abcdef0123456789abcdef01234567
---
 pw_feature/feature.cc | 12 ++++++++++--
 pw_feature/feature.h  |  3 +++
 2 files changed, 13 insertions(+), 2 deletions(-)

diff --git a/pw_feature/feature.cc b/pw_feature/feature.cc
index 1111111..2222222 100644
--- a/pw_feature/feature.cc
+++ b/pw_feature/feature.cc
@@ -10,8 +10,16 @@ namespace pw::feature {
 Status Feature::Enable() {
-  enabled_ = true;
-  return OkStatus();
+  if (enabled_) {
+    return Status::FailedPrecondition();
+  }
+  enabled_ = true;
+  PW_LOG_INFO("Feature enabled");
+  return OkStatus();
+}
+
+bool Feature::enabled() const {
+  return enabled_;
 }

 }  // namespace pw::feature
diff --git a/pw_feature/feature.h b/pw_feature/feature.h
index 3333333..4444444 100644
--- a/pw_feature/feature.h
+++ b/pw_feature/feature.h
@@ -20,6 +20,9 @@ class Feature {
  public:
   Status Enable();

+  // Returns whether Enable() has succeeded.
+  bool enabled() const;
+
  private:
   bool enabled_ = false;
 };
`

// Run executes args against RootCmd and measures its subprocesses and RPCs.
// Requests issued before Run (e.g. by Setup) are not counted.
func (f *KindSmokeFixture) Run(t *testing.T, args []string) KindSmokeResult {
	t.Helper()
	f.gerritReqs.reset()
	f.issueReqs.reset()
	if refused := f.network.takeRefused(); len(refused) > 0 {
		t.Errorf("setup reached for the real network: %v", refused)
	}
	// Drop process-wide auth, token, and quota-project memos so the
	// measured invocation starts as cold as a fresh process.
	ResetAuthTokenCaches()
	var res KindSmokeResult
	_, calls := countSubprocesses(t, func() {
		res.Out, res.Err = executeCommand(RootCmd, args...)
	})
	res.Out = scrubGolden(res.Out)
	slices.Sort(calls)
	res.Subprocesses = calls
	res.RPCs = f.gerritReqs.Total() + f.issueReqs.Total()
	res.RPCRoutes = append(f.gerritReqs.RouteCounts(), f.issueReqs.RouteCounts()...)
	res.RefusedRequests = f.network.takeRefused()
	return res
}

// Setup executes args without measuring them, failing the test if the
// command fails. Use it to put state in place (e.g. a mounted worktree
// project) before the measured invocation. args starting with "git" run git
// directly in WorkDir; anything else runs against RootCmd.
func (f *KindSmokeFixture) Setup(t *testing.T, args []string) {
	t.Helper()
	if len(args) > 0 && args[0] == "git" {
		f.repo.GitOutput(args[1:]...)
		return
	}
	if out, err := executeCommand(RootCmd, args...); err != nil {
		t.Fatalf("setup `gh %s` failed: %v\nOutput:\n%s", strings.Join(args, " "), err, out)
	}
	if refused := f.network.takeRefused(); len(refused) > 0 {
		t.Errorf("setup `gh %s` reached for the real network: %v", strings.Join(args, " "), refused)
	}
}

// AssertNoDuplicateRequests fails if the measured invocation issued an
// identical request twice (lost memoization), as TestPerfBudget_Commands
// does.
func (f *KindSmokeFixture) AssertNoDuplicateRequests(t *testing.T, name string) {
	t.Helper()
	f.gerritReqs.assertNoDuplicateRequests(t, name)
	f.issueReqs.assertNoDuplicateRequests(t, name+" (Issue Tracker)")
}

// KindSmokeSplitResult is the output of one invocation with stdout and
// stderr kept apart.
type KindSmokeSplitResult struct {
	// Stdout holds everything written to the command's out writer and to
	// os.Stdout; Stderr holds the command's err writer.
	Stdout, Stderr string
	Err            error
}

// RunSplit executes args against RootCmd like Run, but keeps stdout and
// stderr apart (Run combines them), so callers can hold stdout to a
// machine-readable contract while stderr carries diagnostics. It does not
// measure cost; TestKindContract_Commands does that.
func (f *KindSmokeFixture) RunSplit(t *testing.T, args []string) KindSmokeSplitResult {
	t.Helper()
	return f.RunSplitIn(t, "", args)
}

// RunSplitIn is RunSplit with the CLI's working directory (and default git
// directory) set to dir instead of the fixture repository, for commands that
// behave differently inside a `wt` project. An empty dir means WorkDir.
func (f *KindSmokeFixture) RunSplitIn(t *testing.T, dir string, args []string) KindSmokeSplitResult {
	t.Helper()
	if refused := f.network.takeRefused(); len(refused) > 0 {
		t.Errorf("setup reached for the real network: %v", refused)
	}
	if dir != "" {
		prevCWD, prevGit, prevPreRunE := MockCWD, DefaultGitRunner, RootCmd.PersistentPreRunE
		MockCWD = dir
		DefaultGitRunner = &RealGitRunner{Dir: dir}
		// BindToCLI's PersistentPreRunE pins the config's CWD to WorkDir;
		// re-pin it to dir after that runs.
		RootCmd.PersistentPreRunE = func(cmd *cobra.Command, args []string) error {
			if prevPreRunE != nil {
				if err := prevPreRunE(cmd, args); err != nil {
					return err
				}
			}
			if cfg := GetConfig(cmd); cfg != nil {
				cfg.CWD = dir
				cfg.Git = DefaultGitRunner
			}
			return nil
		}
		defer func() { MockCWD, DefaultGitRunner, RootCmd.PersistentPreRunE = prevCWD, prevGit, prevPreRunE }()
	}
	ResetAuthTokenCaches()
	var stdout, stderr bytes.Buffer
	err := executeCommandIO(RootCmd, nil, &stdout, &stderr, args...)
	if refused := f.network.takeRefused(); len(refused) > 0 {
		t.Errorf("`gh %s` reached for the real network (%v); the kind smoke fixture must stay hermetic", strings.Join(args, " "), refused)
	}
	return KindSmokeSplitResult{Stdout: stdout.String(), Stderr: stderr.String(), Err: err}
}

// ServeFailingBuild makes change 12345's CI report one failed blocking build
// (linux-gn) next to a passing one, with build details and a log, so
// `pr checks --log-failed` has a failure report to print.
func (f *KindSmokeFixture) ServeFailingBuild(t *testing.T) {
	t.Helper()
	s := f.gerrit
	builds := []bbBuild{
		FakeBuild("87654321001", "linux-bazel", "SUCCESS", WithBucket("try")),
		FakeBuild("87654321002", "linux-gn", "FAILURE", WithBucket("try"), WithSummary("compile failed")),
	}
	details := FakeBuildDetails("87654321002", "linux-gn", "FAILURE",
		WithSteps(
			FakeStep("setup", "SUCCESS"),
			FakeStep("compile", "FAILURE",
				WithStepSummary("ninja: build stopped: subcommand failed."),
				WithStepLogs(FakeLog("stdout", s.URL+"/logs/87654321002")),
			),
		),
	)
	routes := []MockRoute{
		{Method: "POST", Path: "/prpc/buildbucket.v2.Builds/SearchBuilds", Handler: func(w http.ResponseWriter, r *http.Request) {
			s.RespondJSON(w, http.StatusOK, map[string]any{"builds": builds})
		}},
		{Method: "POST", Path: "/prpc/buildbucket.v2.Builds/GetBuild", Handler: func(w http.ResponseWriter, r *http.Request) {
			var req struct {
				ID string `json:"id"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ID != details.ID {
				w.WriteHeader(http.StatusNotFound)
				return
			}
			s.RespondJSON(w, http.StatusOK, details)
		}},
		{Method: "GET", Path: "/logs/87654321002", Handler: func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "text/plain")
			_, _ = w.Write([]byte("[1/2] compiling pw_feature/feature.cc\nFAILED: pw_feature/feature.cc.o\n"))
		}},
	}
	// Ahead of the fixture's passing SearchBuilds route.
	s.mu.Lock()
	s.routes = append(routes, s.routes...)
	s.mu.Unlock()
}
