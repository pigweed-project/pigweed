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

//go:build live

package pw_ghish

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/andygrunwald/go-gerrit"
	"github.com/google/go-cmp/cmp"
)

func executeLiveCommand(args ...string) (string, error) {
	setupRootAliases()
	resetAllFlags(RootCmd)
	HostFlag = "https://pigweed-review.googlesource.com"
	ProfileFlag = ""

	var outBuf, errBuf bytes.Buffer
	RootCmd.SetOut(&outBuf)
	RootCmd.SetErr(&errBuf)
	RootCmd.SetArgs(args)

	err := RootCmd.Execute()
	out := outBuf.String()
	if err != nil && out == "" {
		out = errBuf.String()
	}
	return out, err
}

// isCIVerdict reports whether err is a statement about CI (checks failed, or
// checks are still running) rather than a malfunction of the tool.
//
// `pr checks` exits 1 or 8 in those cases by design, and only the code paths
// that reach a verdict wrap their error in *ExitCodeError -- an auth failure
// or a 404 returns a plain error. A test that only inspects the *shape* of
// the output must therefore tolerate a verdict, or it starts failing every
// time somebody uploads a patchset to the CL it queries.
func isCIVerdict(err error) bool {
	var codeErr *ExitCodeError
	return errors.As(err, &codeErr)
}

// checksSummaryGlyphs are the leading status markers `pr view` and
// `pr status` use to summarize CI in one line.
var checksSummaryGlyphs = []string{"✓", "✖", "●"}

// assertChecksSummary requires summary to be a recognized CI summary, without
// dictating which verdict it reports.
//
// Asserting a specific verdict on the current patchset of an actively
// developed CL is not a testable invariant: uploading a patchset restarts CI,
// so the fixture silently changes meaning and the test fails for reasons that
// have nothing to do with the code. Pin a verdict only against a superseded
// patchset, whose builds can no longer change.
func assertChecksSummary(t *testing.T, what, summary string) {
	t.Helper()
	for _, glyph := range checksSummaryGlyphs {
		if strings.HasPrefix(summary, glyph) {
			return
		}
	}
	t.Errorf("%s: expected a checks summary starting with one of %v, got %q",
		what, checksSummaryGlyphs, summary)
}

// TestLive_GerritAuthAndMetadata verifies live authentication and metadata inspection
// against production pigweed-review.googlesource.com.
func TestLive_GerritAuthAndMetadata(t *testing.T) {
	ctx := context.Background()

	client, err := NewGerritClient(ctx, RootCmd)
	if err != nil {
		t.Skipf("Skipping live test: could not initialize Gerrit client: %v", err)
	}

	self, _, err := client.Accounts.GetAccount(ctx, "self")
	if err != nil {
		t.Skipf("Skipping live test: cannot authenticate to production Gerrit: %v", err)
	}
	t.Logf("Authenticated as: %s <%s>", self.Name, self.Email)

	// Verify viewing CL 472267 (pw_ghish)
	output, err := executeLiveCommand("pr", "view", "472267", "--json", "number,title,status,author")
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, output)
	}

	var data struct {
		Number int    `json:"number"`
		Title  string `json:"title"`
		Status string `json:"status"`
	}
	if err := json.Unmarshal([]byte(output), &data); err != nil {
		t.Fatalf("Failed to parse pr view JSON output: %v\nRaw: %s", err, output)
	}

	if data.Number != 472267 {
		t.Errorf("Expected CL 472267, got %d", data.Number)
	}
	if !strings.Contains(data.Title, "pw_ghish") {
		t.Errorf("Expected title to contain 'pw_ghish', got %q", data.Title)
	}
}

// TestLive_BuildbucketAndLogDogFailureInspection queries live Buildbucket and LogDog
// against a known failing change (CL 467905/22).
func TestLive_BuildbucketAndLogDogFailureInspection(t *testing.T) {
	ctx := context.Background()

	// 1. Query Buildbucket for CL 467905 Patchset 22
	builds, err := queryBuildbucket(ctx, "cr-buildbucket.appspot.com", "pigweed-review.googlesource.com", "pigweed/pigweed", 467905, 22, http.DefaultClient)
	if err != nil {
		t.Fatalf("queryBuildbucket failed on live Buildbucket: %v", err)
	}

	if len(builds) == 0 {
		t.Fatalf("Expected builds for CL 467905/22, got 0")
	}

	// 2. Find the failing pigweed-lintformat build
	var failedBuildID string
	for _, b := range builds {
		if b.Builder.Builder == "pigweed-lintformat" && (b.Status == "FAILURE" || b.Status == "INFRA_FAILURE") {
			failedBuildID = b.ID
			break
		}
	}
	if failedBuildID == "" {
		t.Fatalf("Could not find failed pigweed-lintformat build on CL 467905/22")
	}

	// 3. Fetch step-level details via pRPC
	details, err := GetBuildDetails(ctx, "cr-buildbucket.appspot.com", failedBuildID, http.DefaultClient)
	if err != nil {
		t.Fatalf("GetBuildDetails failed on live build %s: %v", failedBuildID, err)
	}

	if len(details.Steps) == 0 {
		t.Fatalf("Expected steps in build details for %s, got 0", failedBuildID)
	}

	// 4. Extract failure report including LogDog log stream
	report := details.ExtractFailureReport(ctx, 30, http.DefaultClient)
	if report == nil {
		t.Fatalf("Expected non-nil FailureReport for failing build %s", failedBuildID)
	}

	if report.FailedStep == "" {
		t.Errorf("Expected non-empty FailedStep in failure report")
	}
	if report.LogSnippet == "" && report.StepSummary == "" {
		t.Errorf("Expected either LogSnippet or StepSummary to be populated")
	}
	if !strings.HasPrefix(report.FullLogURL, "https://logs.chromium.org/") {
		t.Errorf("Expected LogDog URL to start with https://logs.chromium.org/, got %q", report.FullLogURL)
	}

	t.Logf("Successfully triaged live failure: Builder=%s Step=%s LogURL=%s", report.Builder, report.FailedStep, report.FullLogURL)
}

// TestLive_DraftCommentRoundTrip tests creating, verifying, and cleaning up a draft comment
// on CL 472267 without leaving any persistent comments or notifying reviewers.
func TestLive_DraftCommentRoundTrip(t *testing.T) {
	ctx := context.Background()

	client, err := NewGerritClient(ctx, RootCmd)
	if err != nil {
		t.Skipf("Skipping live test: could not initialize Gerrit client: %v", err)
	}

	// 1. Post draft comment via CLI
	draftMsg := "[AUTOMATED_LIVE_INTEGRATION_TEST] Temporary verification draft"
	cleanedUp := false
	t.Cleanup(func() {
		if cleanedUp {
			return
		}
		if draftsMap, _, err := client.Changes.ListChangeDrafts(ctx, "472267"); err == nil && draftsMap != nil {
			for filePath, comments := range *draftsMap {
				if filePath == "pw_ghish/docs.rst" {
					for _, c := range comments {
						if strings.Contains(c.Message, draftMsg) {
							_, _ = client.Changes.DeleteDraft(ctx, "472267", "current", c.ID)
						}
					}
				}
			}
		}
	})

	output, err := executeLiveCommand("pr", "comment", "472267", "--path", "pw_ghish/docs.rst", "--line", "1", "-m", draftMsg, "--draft")
	if err != nil {
		t.Fatalf("pr comment --draft failed: %v\nOutput: %s", err, output)
	}

	// 2. Query draft comments via Gerrit REST API
	draftsMap, _, err := client.Changes.ListChangeDrafts(ctx, "472267")
	if err != nil {
		t.Fatalf("Failed to list drafts from Gerrit: %v", err)
	}

	var foundDraftID string
	for filePath, comments := range *draftsMap {
		if filePath == "pw_ghish/docs.rst" {
			for _, c := range comments {
				if strings.Contains(c.Message, draftMsg) {
					foundDraftID = c.ID
					break
				}
			}
		}
	}

	if foundDraftID == "" {
		t.Fatalf("Draft comment was not found in Gerrit draft comments")
	}
	t.Logf("Draft comment verified on live Gerrit (Draft ID: %s)", foundDraftID)

	// 3. Clean up the draft comment immediately
	_, err = client.Changes.DeleteDraft(ctx, "472267", "current", foundDraftID)
	if err != nil {
		t.Errorf("Failed to cleanup draft comment %s: %v", foundDraftID, err)
	} else {
		cleanedUp = true
		t.Logf("Cleanly deleted draft comment %s", foundDraftID)
	}
}

// TestLive_RunRerunDryRun verifies that run rerun correctly resolves live change coordinates.
//
// The builder is discovered from the change at runtime rather than hardcoded.
// This test used to name pigweed-mac-arm-vscode, which ran on CL 472267 only
// while patchset 43 was current: a full Commit-Queue dry run had happened
// there, so all 75 builders were present. Patchset 47 has had only two builders
// triggered, so the hardcoded name vanished and the test started failing for
// reasons that had nothing to do with `run rerun`. Which builders exist on a
// change is not a property this test should depend on.
func TestLive_RunRerunDryRun(t *testing.T) {
	checks := liveChecks(t, "472267")
	if len(checks) == 0 {
		t.Skip("CL 472267 currently has no checks to rerun")
	}
	builder := checks[0].Name

	output, err := executeLiveCommand("run", "rerun", "472267", "-j", builder, "--dry-run")
	if err != nil {
		t.Fatalf("run rerun -j %s --dry-run failed: %v\nOutput: %s", builder, err, output)
	}

	expectedPrefix := "[dry-run] bb add -cl https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267/"
	expectedSuffix := "pigweed/pigweed.try/" + builder
	if !strings.Contains(output, expectedPrefix) || !strings.Contains(output, expectedSuffix) {
		t.Errorf("Unexpected dry run command:\n%s\nWant prefix %q and suffix %q", output, expectedPrefix, expectedSuffix)
	}
}

// TestLive_RunRerun_UnknownBuilderIsActionable verifies the error path that the
// stale fixture in TestLive_RunRerunDryRun was accidentally exercising.
//
// Asking to rerun a builder that did not run on the change is a mistake worth
// catching, and the error has to say which builders are actually available --
// otherwise the caller has no way to correct themselves. A name that cannot
// ever exist keeps this stable.
func TestLive_RunRerun_UnknownBuilderIsActionable(t *testing.T) {
	const bogus = "definitely-not-a-real-builder-name"

	output, err := executeLiveCommand("run", "rerun", "472267", "-j", bogus, "--dry-run")
	if err == nil {
		t.Fatalf("expected an error for an unknown builder, got success:\n%s", output)
	}

	msg := err.Error()
	if !strings.Contains(msg, bogus) {
		t.Errorf("error should name the builder that was not found, got:\n%s", msg)
	}

	// The tool only offers alternatives when there are some. Asserting the
	// list unconditionally would make this test fail on a patchset that has
	// no checks yet, which is not what it is testing.
	checks := liveChecks(t, "472267")
	if len(checks) == 0 {
		t.Log("CL 472267 currently reports no checks, so there is no list to offer")
		return
	}

	if !strings.Contains(msg, "Available checks on Change 472267") {
		t.Errorf("error should list the builders that are available, got:\n%s", msg)
	}
	// The remediation is only useful if the names it offers actually work.
	for _, c := range checks {
		if !strings.Contains(msg, c.Name) {
			t.Errorf("error omits available builder %q, got:\n%s", c.Name, msg)
		}
	}
}

// TestLive_ProfileValidation verifies that an invalid --profile is strictly rejected.
func TestLive_ProfileValidation(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "472267", "--profile", "nonexistent_profile")
	if err == nil {
		t.Fatalf("Expected error for unknown profile, got success: %s", output)
	}
	if !strings.Contains(err.Error(), "unknown profile") {
		t.Errorf("Expected 'unknown profile' in error, got: %v", err)
	}
}

// TestLive_AuthMethodValidation verifies that an unrecognized GH_ISH_AUTH_METHOD is strictly rejected.
func TestLive_AuthMethodValidation(t *testing.T) {
	t.Setenv("GH_ISH_AUTH_METHOD", "bogus_auth")
	output, err := executeLiveCommand("pr", "view", "472267")
	if err == nil {
		t.Fatalf("Expected error for bogus auth method, got success: %s", output)
	}
	if !strings.Contains(err.Error(), "unrecognized auth method") {
		t.Errorf("Expected 'unrecognized auth method' in error, got: %v", err)
	}
}

// TestLive_CommentResolvedValidation verifies that --resolved without --path and --line fails fast.
func TestLive_CommentResolvedValidation(t *testing.T) {
	output, err := executeLiveCommand("pr", "comment", "472267", "-m", "Fixed issue", "--resolved")
	if err == nil {
		t.Fatalf("Expected error for --resolved without path/line, got success: %s", output)
	}
	if !strings.Contains(err.Error(), "--resolved requires both --path and --line") {
		t.Errorf("Expected '--resolved requires both --path and --line' in error, got: %v", err)
	}
}

// TestLive_Status_ActiveBranchComments verifies that pr status correctly fetches
// and summarizes comments for the active change against production Gerrit.
func TestLive_Status_ActiveBranchComments(t *testing.T) {
	output, err := executeLiveCommand("pr", "status", "--json", "current_branch")
	if err != nil {
		t.Fatalf("pr status --json failed on live Gerrit: %v\nOutput: %s", err, output)
	}

	var res struct {
		CurrentBranch struct {
			Number   int             `json:"number"`
			Comments CommentsSummary `json:"comments"`
		} `json:"current_branch"`
	}
	if err := json.Unmarshal([]byte(output), &res); err != nil {
		t.Fatalf("Failed to parse JSON output: %v\nOutput: %s", err, output)
	}

	if res.CurrentBranch.Number == 0 {
		t.Fatalf("Expected active PR number to be non-zero")
	}

	t.Logf("Active PR #%d comments: TotalThreads=%d Unresolved=%d Drafts=%d",
		res.CurrentBranch.Number, res.CurrentBranch.Comments.TotalThreads,
		res.CurrentBranch.Comments.UnresolvedThreads, res.CurrentBranch.Comments.DraftsCount)

	textOutput, err := executeLiveCommand("pr", "status")
	if err != nil {
		t.Fatalf("pr status failed on live Gerrit: %v\nOutput: %s", err, textOutput)
	}

	if !strings.Contains(textOutput, "Comments:") {
		t.Errorf("Expected live pr status output to contain 'Comments:', got:\n%s", textOutput)
	}
}

// TestLive_ViewWithGerritURLAndShortlink verifies that commands accepting <id> accept full
// Gerrit URLs and pwrev shortlinks against production Gerrit.
func TestLive_ViewWithGerritURLAndShortlink(t *testing.T) {
	// 1. Full URL
	fullURL := "https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/472267"
	output, err := executeLiveCommand("pr", "view", fullURL, "--json", "number,title")
	if err != nil {
		t.Fatalf("pr view <url> failed: %v\nOutput: %s", err, output)
	}
	var data struct {
		Number int    `json:"number"`
		Title  string `json:"title"`
	}
	if err := json.Unmarshal([]byte(output), &data); err != nil {
		t.Fatalf("Failed to parse JSON output: %v", err)
	}
	if data.Number != 472267 {
		t.Errorf("got number %d, want 472267", data.Number)
	}

	// 2. Shortlink pwrev/472267
	shortOutput, err := executeLiveCommand("pr", "view", "pwrev/472267", "--json", "number,title")
	if err != nil {
		t.Fatalf("pr view pwrev/472267 failed: %v\nOutput: %s", err, shortOutput)
	}
	var shortData struct {
		Number int    `json:"number"`
		Title  string `json:"title"`
	}
	if err := json.Unmarshal([]byte(shortOutput), &shortData); err != nil {
		t.Fatalf("Failed to parse JSON output: %v", err)
	}
	if shortData.Number != 472267 {
		t.Errorf("got number %d, want 472267", shortData.Number)
	}
}

// TestLive_ViewActiveChangeDefault verifies that pr view with 0 args defaults to the active change.
func TestLive_ViewActiveChangeDefault(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "--json", "number,title")
	if err != nil {
		t.Fatalf("pr view with 0 args failed: %v\nOutput: %s", err, output)
	}
	var data struct {
		Number int    `json:"number"`
		Title  string `json:"title"`
	}
	if err := json.Unmarshal([]byte(output), &data); err != nil {
		t.Fatalf("Failed to parse JSON output: %v", err)
	}
	if data.Number != 472267 {
		t.Errorf("got number %d, want 472267", data.Number)
	}
}

// TestLive_CommentTreeVerification verifies that production Gerrit comments
// are parsed into trees without node loss, cycles, or formatting corruption.
func TestLive_CommentTreeVerification(t *testing.T) {
	ctx := context.Background()

	client, err := NewGerritClient(ctx, RootCmd)
	if err != nil {
		t.Skipf("Skipping live test: could not initialize Gerrit client: %v", err)
	}

	// Fetch comments from CL 413992 (pw_multibuf, known to have rich inline comment threads)
	changeIDs := []string{"413992", "473645", "472267"}
	var foundComments map[string][]gerrit.CommentInfo
	var targetCL string

	for _, cl := range changeIDs {
		comments, _, err := client.Changes.ListChangeComments(ctx, cl)
		if err != nil {
			t.Logf("Warning: could not list comments for CL %s: %v", cl, err)
			continue
		}
		if comments != nil && len(*comments) > 0 {
			foundComments = *comments
			targetCL = cl
			break
		}
	}

	if len(foundComments) == 0 {
		t.Skip("Skipping live comment verification: no comments found on test CLs")
	}

	t.Logf("Verifying comment forest for CL %s with %d files having comments", targetCL, len(foundComments))

	totalComments := 0
	for path, fileComments := range foundComments {
		totalComments += len(fileComments)
		roots := BuildCommentForest(fileComments)

		// Verify no nodes were dropped: count all nodes in forest
		countNodes := 0
		var walk func(n *CommentNode, depth int)
		walk = func(n *CommentNode, depth int) {
			if depth > len(fileComments) {
				t.Fatalf("Cycle detected in comment forest for file %s: depth %d > total %d", path, depth, len(fileComments))
			}
			countNodes++
			for _, ch := range n.Children {
				walk(ch, depth+1)
			}
		}

		for _, r := range roots {
			walk(r, 0)
		}

		if countNodes != len(fileComments) {
			t.Errorf("File %s: node count mismatch: forest has %d, input had %d", path, countNodes, len(fileComments))
		}

		// Verify thread extraction
		threads := BuildCommentThreads(path, fileComments, nil)
		if len(threads) != len(roots) {
			t.Errorf("File %s: thread count %d != root count %d", path, len(threads), len(roots))
		}

		// If any comment has a line number, verify FindLatestCommentAtLine
		for _, c := range fileComments {
			if c.Line > 0 {
				latest := FindLatestCommentAtLine(fileComments, c.Line)
				if latest == nil {
					t.Errorf("File %s line %d: FindLatestCommentAtLine returned nil", path, c.Line)
				} else if latest.Line != c.Line {
					t.Errorf("File %s: expected line %d, got %d", path, c.Line, latest.Line)
				}
				break
			}
		}
	}

	// Verify formatted output
	formatted := FormatCommentForest(foundComments)
	if totalComments > 0 && formatted == "" {
		t.Errorf("FormatCommentForest returned empty string for non-empty comments")
	}
	t.Logf("Formatted comment forest length: %d characters across %d comments", len(formatted), totalComments)
}

type dirGitRunner struct {
	dir string
}

func (r *dirGitRunner) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	cmd := exec.CommandContext(ctx, "git", args...)
	cmd.Dir = r.dir
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	return cmd.Run()
}

func TestLive_GitRemoteTipIntegration(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Fatalf("test environment setup broken: git executable not found in PATH: %v (git is required for live git integration test)", err)
	}
	ctx := context.Background()
	tempDir := t.TempDir()

	// 1. Create bare origin repository
	originDir := filepath.Join(tempDir, "origin.git")
	if err := os.MkdirAll(originDir, 0755); err != nil {
		t.Fatalf("failed to create origin dir: %v", err)
	}
	execGit := func(dir string, args ...string) {
		t.Helper()
		cmd := exec.Command("git", args...)
		cmd.Dir = dir
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("git %v failed in %s: %v\nOutput: %s", args, dir, err, string(out))
		}
	}

	execGit(originDir, "init", "--bare", "-b", "main")

	// 2. Clone to local repository
	workDir := filepath.Join(tempDir, "work")
	execGit(tempDir, "clone", originDir, workDir)
	execGit(workDir, "config", "user.name", "Test Runner")
	execGit(workDir, "config", "user.email", "testrunner@example.com")

	// Create initial commit in work repo and push to origin
	readme := filepath.Join(workDir, "README.md")
	if err := os.WriteFile(readme, []byte("hello\n"), 0644); err != nil {
		t.Fatalf("failed to write readme: %v", err)
	}
	execGit(workDir, "add", "README.md")
	execGit(workDir, "commit", "-m", "Initial commit")
	execGit(workDir, "push", "origin", "main")

	// Point client to the real working directory
	runner := &dirGitRunner{dir: workDir}
	client := NewGitClient(runner)

	// In workDir, HEAD is currently pointing directly to refs/remotes/origin/main
	isTip, remotes, err := client.IsHeadRemoteTip(ctx)
	if err != nil {
		t.Fatalf("IsHeadRemoteTip failed: %v", err)
	}
	if !isTip {
		t.Fatalf("expected HEAD to point to remote tracking tip, remotes: %v", remotes)
	}

	// CheckAmendAllowed must fail
	if err := client.CheckAmendAllowed(ctx); err == nil {
		t.Fatal("expected CheckAmendAllowed to fail when on remote tracking tip, got nil")
	}

	// CommitAmendNoEdit must fail
	if err := client.CommitAmendNoEdit(ctx, io.Discard, io.Discard); err == nil {
		t.Fatal("expected CommitAmendNoEdit to fail when on remote tracking tip, got nil")
	}

	// 3. Now make a local unpushed commit on top of main
	if err := os.WriteFile(readme, []byte("hello world\n"), 0644); err != nil {
		t.Fatalf("failed to update readme: %v", err)
	}
	execGit(workDir, "commit", "-am", "Local unpushed change")

	// Now HEAD is 1 commit ahead of origin/main
	isTip, remotes, err = client.IsHeadRemoteTip(ctx)
	if err != nil {
		t.Fatalf("IsHeadRemoteTip failed: %v", err)
	}
	if isTip {
		t.Fatalf("expected HEAD NOT to point to remote tracking tip, remotes: %v", remotes)
	}

	// CheckAmendAllowed must now succeed
	if err := client.CheckAmendAllowed(ctx); err != nil {
		t.Fatalf("expected CheckAmendAllowed to succeed on local commit, got: %v", err)
	}

	// CommitAmendNoEdit must now succeed
	if err := client.CommitAmendNoEdit(ctx, io.Discard, io.Discard); err != nil {
		t.Fatalf("expected CommitAmendNoEdit to succeed on local commit, got: %v", err)
	}
}

// TestLive_Checks_List verifies that pr checks queries live Buildbucket for
// CL 472267, returning the checks table and matching JSON output.
//
// It asserts the shape of the output, not the verdict: CL 472267 is actively
// developed, so its checks may be passing, failing, or still running, and
// `pr checks` deliberately exits non-zero for the latter two.
func TestLive_Checks_List(t *testing.T) {
	// 1. Plain text checks table
	output, err := executeLiveCommand("pr", "checks", "472267")
	if err != nil && !isCIVerdict(err) {
		t.Fatalf("pr checks 472267 failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Checks for Change 472267") {
		t.Errorf("Expected output to contain 'Checks for Change 472267', got:\n%s", output)
	}
	if !strings.Contains(output, "https://ci.chromium.org/b/") {
		t.Errorf("Expected output to contain Buildbucket URLs, got:\n%s", output)
	}

	// 2. Structured JSON output
	jsonOutput, err := executeLiveCommand("pr", "checks", "472267", "--json", "checks")
	if err != nil && !isCIVerdict(err) {
		t.Fatalf("pr checks 472267 --json checks failed: %v\nOutput: %s", err, jsonOutput)
	}

	var data struct {
		Checks []struct {
			Name         string `json:"name"`
			Status       string `json:"status"`
			StatusSymbol string `json:"statusSymbol"`
			Duration     string `json:"duration"`
			URL          string `json:"url"`
		} `json:"checks"`
	}
	if err := json.Unmarshal([]byte(jsonOutput), &data); err != nil {
		t.Fatalf("Failed to parse JSON output: %v\nRaw: %s", err, jsonOutput)
	}

	if len(data.Checks) == 0 {
		t.Fatalf("Expected at least one check in JSON output, got 0")
	}

	foundSuccess := false
	for _, c := range data.Checks {
		if c.Name == "" {
			t.Errorf("Found check with empty name: %+v", c)
		}
		if !strings.HasPrefix(c.URL, "https://ci.chromium.org/b/") {
			t.Errorf("Expected URL prefix https://ci.chromium.org/b/, got %q", c.URL)
		}
		if c.Status == "SUCCESS" && c.StatusSymbol == "✓" {
			foundSuccess = true
		}
	}
	if !foundSuccess {
		t.Errorf("Expected at least one successful check on CL 472267")
	}
	t.Logf("Verified %d live checks on CL 472267", len(data.Checks))
}

// TestLive_Run_View_LogFailed verifies that gh run view --log-failed fetches failure diagnostics and
// LogDog snippets end-to-end for a known failing change (CL 467905/22).
func TestLive_Run_View_LogFailed(t *testing.T) {
	// 1. Text format
	output, err := executeLiveCommand("run", "view", "467905/22", "--log-failed")
	if err != nil {
		t.Fatalf("run view 467905/22 --log-failed failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "FAILURE: pigweed-lintformat") {
		t.Errorf("Expected output to contain 'FAILURE: pigweed-lintformat', got:\n%s", output)
	}
	if !strings.Contains(output, "Status: FAILURE") {
		t.Errorf("Expected output to contain 'Status: FAILURE', got:\n%s", output)
	}
	if !strings.Contains(output, "Step: python_format") {
		t.Errorf("Expected output to contain 'Step: python_format', got:\n%s", output)
	}
	if !strings.Contains(output, "Full Log URL: https://logs.chromium.org/") {
		t.Errorf("Expected output to contain LogDog Full Log URL, got:\n%s", output)
	}

	// 2. JSON format
	jsonOutput, err := executeLiveCommand("run", "view", "467905/22", "--log-failed", "--json")
	if err != nil {
		t.Fatalf("run view 467905/22 --log-failed --json failed: %v\nOutput: %s", err, jsonOutput)
	}

	var reports []FailureReport
	if err := json.Unmarshal([]byte(jsonOutput), &reports); err != nil {
		t.Fatalf("Failed to parse failure reports JSON: %v\nRaw: %s", err, jsonOutput)
	}

	if len(reports) == 0 {
		t.Fatalf("Expected at least one failure report for CL 467905/22, got 0")
	}
	if reports[0].Builder != "pigweed-lintformat" {
		t.Errorf("Expected builder pigweed-lintformat, got %q", reports[0].Builder)
	}
	if !strings.Contains(reports[0].FailedStep, "python_format") {
		t.Errorf("Expected failed step python_format, got %q", reports[0].FailedStep)
	}
	if !strings.HasPrefix(reports[0].FullLogURL, "https://logs.chromium.org/") {
		t.Errorf("Expected LogDog URL prefix, got %q", reports[0].FullLogURL)
	}
}

// TestLive_Checks_Watch_FastCompletion verifies that pr checks --watch on a change
// whose checks have all reached a terminal state (CL 472267/43) prints the watch
// header, discovers all checks, and completes promptly instead of looping.
//
// It deliberately does NOT require a green result. Patchset 43 is a historical
// patchset of an actively developed CL, so its builds were canceled the moment
// patchset 44 was uploaded: as of this writing it is 46 SUCCESS and 29 CANCELED.
// The property under test is that --watch reaches a verdict and stops, which is
// exactly "the exit code is not ExitCodePending".
func TestLive_Checks_Watch_FastCompletion(t *testing.T) {
	output, err := executeLiveCommand("pr", "checks", "472267/43", "--watch", "--interval", "100ms")

	if got := ExitCodeFor(err); got == ExitCodePending {
		t.Errorf("--watch returned exit %d (pending) on a change with no running checks; "+
			"the watch loop must not stop while it still believes work is outstanding.\nOutput:\n%s",
			got, output)
	}

	if !strings.Contains(output, "Watching") || !strings.Contains(output, "(Patchset 43)") {
		t.Errorf("Expected watching header for patchset 43, got:\n%s", output)
	}
	if !strings.Contains(output, "passed") || !strings.Contains(output, "running") {
		t.Errorf("Expected initial status breakdown (passed, running) in watching header, got:\n%s", output)
	}
	if !strings.Contains(output, "Checks for Change 472267 (Patchset 43)") {
		t.Errorf("Expected final checks table, got:\n%s", output)
	}
}

// TestLive_Checks_Watch_FailFast verifies that pr checks --watch --fail-fast on a change
// with a failing check (CL 467905/22) detects the failure, halts early, and dumps
// the LogDog failure diagnostics.
func TestLive_Checks_Watch_FailFast(t *testing.T) {
	output, err := executeLiveCommand("pr", "checks", "467905/22", "--watch", "--interval", "100ms", "--fail-fast")
	if err == nil {
		t.Fatalf("Expected error for failed check with --fail-fast, got success:\n%s", output)
	}

	if !strings.Contains(output, "Watching") || !strings.Contains(output, "(Patchset 22)") {
		t.Errorf("Expected watching header for patchset 22, got:\n%s", output)
	}
	if !strings.Contains(output, "failed") {
		t.Errorf("Expected initial status breakdown to report failed check, got:\n%s", output)
	}
	if !strings.Contains(output, "--fail-fast triggered: stopping watch.") {
		t.Errorf("Expected '--fail-fast triggered: stopping watch.' in output, got:\n%s", output)
	}
	if !strings.Contains(output, "FAILURE: pigweed-lintformat") {
		t.Errorf("Expected LogDog failure report for pigweed-lintformat, got:\n%s", output)
	}
	if !strings.Contains(err.Error(), "pigweed-lintformat") {
		t.Errorf("Expected error to mention failed builder pigweed-lintformat, got: %v", err)
	}
}

// TestLive_View_DisplaysCommitBody verifies that pr view renders the commit message body
// and suppresses "No score" labels on a live Gerrit change (CL 472267).
func TestLive_View_DisplaysCommitBody(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "472267")
	if err != nil {
		t.Fatalf("pr view 472267 failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Change 472267") {
		t.Errorf("Expected output to contain Change 472267, got:\n%s", output)
	}
	// Verify commit description is visible in view output
	if !strings.Contains(output, "Introduce pw_ghish (./gh)") {
		t.Errorf("Expected commit message body in view output, got:\n%s", output)
	}
	// Verify "No score" labels are filtered out
	if strings.Contains(output, "No score") {
		t.Errorf("Expected 'No score' labels to be suppressed in view output, got:\n%s", output)
	}
}

// TestLive_View_JSON_Body verifies that --json body returns the commit message body.
func TestLive_View_JSON_Body(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "472267", "--json", "number,body,title")
	if err != nil {
		t.Fatalf("pr view --json body failed: %v\nOutput: %s", err, output)
	}

	var data struct {
		Number int    `json:"number"`
		Title  string `json:"title"`
		Body   string `json:"body"`
	}
	if err := json.Unmarshal([]byte(output), &data); err != nil {
		t.Fatalf("Failed to parse JSON output: %v\nRaw: %s", err, output)
	}

	if data.Number != 472267 {
		t.Errorf("Expected number 472267, got %d", data.Number)
	}
	if !strings.Contains(data.Body, "Introduce pw_ghish (./gh)") {
		t.Errorf("Expected body to contain 'Introduce pw_ghish (./gh)', got: %q", data.Body)
	}
}

// TestLive_List_AuthorMe verifies that --author @me queries changes for the current user,
// and --author <email> queries changes for a specific user.
func TestLive_List_AuthorMe(t *testing.T) {
	ctx := t.Context()
	client, err := NewGerritClient(ctx, RootCmd)
	if err != nil {
		t.Fatalf("NewGerritClient failed: %v", err)
	}
	self, _, err := client.Accounts.GetAccount(ctx, "self")
	if err != nil {
		t.Fatalf("GetAccount(self) failed: %v", err)
	}

	output, err := executeLiveCommand("pr", "list", "--author", "@me", "--state", "all", "--limit", "5", "--json", "number,author")
	if err != nil {
		t.Fatalf("pr list --author @me failed: %v\nOutput: %s", err, output)
	}

	var items []struct {
		Number int    `json:"number"`
		Author string `json:"author"`
	}
	if err := json.Unmarshal([]byte(output), &items); err != nil {
		t.Fatalf("Failed to unmarshal JSON output from pr list --author @me: %v\nRaw: %s", err, output)
	}

	t.Logf("Verified --author @me resolved to %s <%s> (%d change(s) returned)", self.Name, self.Email, len(items))
	for _, item := range items {
		if item.Author != self.Name && item.Author != self.Email && item.Author != self.Username {
			t.Errorf("Change #%d returned by --author @me has author %q, expected %q or %q",
				item.Number, item.Author, self.Name, self.Email)
		}
	}

	// Also verify explicit author lookup works for a known public contributor.
	knownOutput, err := executeLiveCommand("pr", "list", "--author", "keir@google.com", "--state", "all", "--limit", "5")
	if err != nil {
		t.Fatalf("pr list --author keir@google.com failed: %v\nOutput: %s", err, knownOutput)
	}
	if !strings.Contains(knownOutput, "472267") && !strings.Contains(knownOutput, "Keir Mierle") {
		t.Errorf("Expected pr list --author keir@google.com to find known CLs, got:\n%s", knownOutput)
	}
}

// TestLive_Run_List verifies that 'gh run list' lists tryjob builds for a change on live Buildbucket.
func TestLive_Run_List(t *testing.T) {
	output, err := executeLiveCommand("run", "list", "472267")
	if err != nil {
		t.Fatalf("run list 472267 failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "BUILDER") || !strings.Contains(output, "STATUS") {
		t.Errorf("Expected table headers in run list output, got:\n%s", output)
	}
	if !strings.Contains(output, "static-checks-pigweed") {
		t.Errorf("Expected static-checks-pigweed in run list output, got:\n%s", output)
	}
}

// TestLive_Run_View_VerboseSteps verifies that 'gh run view -v -j <builder>' shows step tree on live Buildbucket.
func TestLive_Run_View_VerboseSteps(t *testing.T) {
	output, err := executeLiveCommand("run", "view", "472267", "-v", "-j", "static-checks-pigweed")
	if err != nil {
		t.Fatalf("run view -v -j failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Steps for static-checks-pigweed") {
		t.Errorf("Expected 'Steps for static-checks-pigweed' in output, got:\n%s", output)
	}
}

// TestLive_View_DisplaysChecksSummary verifies that pr view queries
// Buildbucket and renders a one-line CI summary for a live Gerrit change.
//
// The companion test TestLive_View_DisplaysChecks_Failing pins a specific
// verdict, which it can do because it queries a superseded patchset whose
// builds are frozen. CL 472267's current patchset is not frozen, so this test
// asserts only that a recognized summary is rendered.
func TestLive_View_DisplaysChecksSummary(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "472267")
	if err != nil {
		t.Fatalf("pr view 472267 failed: %v\nOutput: %s", err, output)
	}

	summaryLine := ""
	for _, line := range strings.Split(output, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "Checks:") {
			summaryLine = line
			break
		}
	}
	if summaryLine == "" {
		t.Fatalf("Expected a 'Checks:' line in pr view output, got:\n%s", output)
	}
	assertChecksSummary(t, "pr view",
		strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(summaryLine), "Checks:")))

	jsonOutput, err := executeLiveCommand("pr", "view", "472267", "--json", "number,checks")
	if err != nil {
		t.Fatalf("pr view 472267 --json failed: %v\nOutput: %s", err, jsonOutput)
	}

	var data struct {
		Number int    `json:"number"`
		Checks string `json:"checks"`
	}
	if err := json.Unmarshal([]byte(jsonOutput), &data); err != nil {
		t.Fatalf("Failed to parse JSON: %v\nOutput: %s", err, jsonOutput)
	}

	if data.Number != 472267 {
		t.Errorf("Expected number 472267 in JSON, got %d", data.Number)
	}
	assertChecksSummary(t, "pr view --json checks", data.Checks)
}

// TestLive_View_DisplaysChecks_Failing verifies that pr view detects and reports
// failures on a live change with failing builders (CL 467905/22).
func TestLive_View_DisplaysChecks_Failing(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "467905/22")
	if err != nil {
		t.Fatalf("pr view 467905/22 failed: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Checks:  ✖") {
		t.Errorf("Expected 'Checks:  ✖' in pr view output, got:\n%s", output)
	}
	if !strings.Contains(output, "pigweed-lintformat") {
		t.Errorf("Expected failed builder 'pigweed-lintformat' in pr view output, got:\n%s", output)
	}
	if !strings.Contains(output, "(run 'gh run view --log-failed' to view errors)") {
		t.Errorf("Expected '(run 'gh run view --log-failed' to view errors)' in pr view output, got:\n%s", output)
	}
}

// TestLive_Status_ChecksSummaryAndParallelPerformance verifies that pr status
// displays a 1-line check summary on active change 472267 without 75 lines of dump,
// and completes in parallel cleanly.
func TestLive_Status_ChecksSummaryAndParallelPerformance(t *testing.T) {
	output, err := executeLiveCommand("pr", "status")
	if err != nil {
		t.Fatalf("pr status failed on live Gerrit: %v\nOutput: %s", err, output)
	}

	if !strings.Contains(output, "Current branch") {
		t.Errorf("Expected 'Current branch' in output, got:\n%s", output)
	}
	summaryLine := ""
	for _, line := range strings.Split(output, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "Checks:") {
			summaryLine = line
			break
		}
	}
	if summaryLine == "" {
		t.Fatalf("Expected a 'Checks:' summary line in pr status output, got:\n%s", output)
	}
	assertChecksSummary(t, "pr status",
		strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(summaryLine), "Checks:")))
	if strings.Contains(output, "https://ci.chromium.org/b/") {
		t.Errorf("Expected 1-line check summary instead of verbose URL table, got:\n%s", output)
	}

	// Verify JSON output has checks_summary and checks array
	jsonOutput, err := executeLiveCommand("pr", "status", "--json", "current_branch")
	if err != nil {
		t.Fatalf("pr status --json failed: %v\nOutput: %s", err, jsonOutput)
	}

	var data struct {
		CurrentBranch struct {
			ChecksSummary string      `json:"checks_summary"`
			Checks        []CheckItem `json:"checks"`
		} `json:"current_branch"`
	}
	if err := json.Unmarshal([]byte(jsonOutput), &data); err != nil {
		t.Fatalf("Failed to parse JSON: %v\nOutput: %s", err, jsonOutput)
	}

	assertChecksSummary(t, "pr status --json checks_summary", data.CurrentBranch.ChecksSummary)
	if len(data.CurrentBranch.Checks) == 0 {
		t.Errorf("Expected checks array to be populated for active change")
	}
}

// TestLive_RootAliases verifies that root-level aliases (checks, view, diff, status)
// execute correctly against production pigweed-review.
func TestLive_RootAliases(t *testing.T) {
	// 1. Root "view 472267"
	outView, err := executeLiveCommand("view", "472267")
	if err != nil {
		t.Fatalf("root 'view 472267' failed: %v\nOutput: %s", err, outView)
	}
	if !strings.Contains(outView, "Change 472267") {
		t.Errorf("Expected 'Change 472267' in view output, got:\n%s", outView)
	}

	// 2. Root "checks 472267". This is an alias-dispatch smoke test, so a CI
	// verdict (exit 1 or 8) is an acceptable outcome; only a malfunction is not.
	outChecks, err := executeLiveCommand("checks", "472267")
	if err != nil && !isCIVerdict(err) {
		t.Fatalf("root 'checks 472267' failed: %v\nOutput: %s", err, outChecks)
	}
	if !strings.Contains(outChecks, "Checks for Change 472267") {
		t.Errorf("Expected 'Checks for Change 472267' in checks output, got:\n%s", outChecks)
	}

	// 3. Root "diff 472267 --name-only"
	outDiff, err := executeLiveCommand("diff", "472267", "--name-only")
	if err != nil {
		t.Fatalf("root 'diff 472267 --name-only' failed: %v\nOutput: %s", err, outDiff)
	}
	if !strings.Contains(outDiff, "pw_ghish/") {
		t.Errorf("Expected pw_ghish files in diff output, got:\n%s", outDiff)
	}

	// 4. Root "status"
	outStatus, err := executeLiveCommand("status")
	if err != nil {
		t.Fatalf("root 'status' failed: %v\nOutput: %s", err, outStatus)
	}
	if !strings.Contains(outStatus, "Current branch") {
		t.Errorf("Expected 'Current branch' in status output, got:\n%s", outStatus)
	}
}

// TestLive_Diff_NameOnlyAndStat verifies --name-only and --stat on live Gerrit change 472267.
func TestLive_Diff_NameOnlyAndStat(t *testing.T) {
	// 1. --name-only
	outName, err := executeLiveCommand("pr", "diff", "472267", "--name-only")
	if err != nil {
		t.Fatalf("pr diff --name-only failed: %v\nOutput: %s", err, outName)
	}
	if strings.Contains(outName, "/COMMIT_MSG") {
		t.Errorf("Expected /COMMIT_MSG to be excluded from --name-only output")
	}
	if strings.Contains(outName, "diff --git") {
		t.Errorf("Expected only filenames without git patch headers, got:\n%s", outName)
	}
	if !strings.Contains(outName, "pw_ghish/") {
		t.Errorf("Expected pw_ghish files listed, got:\n%s", outName)
	}

	// 2. --stat
	outStat, err := executeLiveCommand("pr", "diff", "472267", "--stat")
	if err != nil {
		t.Fatalf("pr diff --stat failed: %v\nOutput: %s", err, outStat)
	}
	if strings.Contains(outStat, "/COMMIT_MSG") {
		t.Errorf("Expected /COMMIT_MSG to be excluded from --stat output")
	}
	if !strings.Contains(outStat, "|") {
		t.Errorf("Expected diffstat bar '|' in output, got:\n%s", outStat)
	}
	if !strings.Contains(outStat, "changed") || !strings.Contains(outStat, "insertions(+)") {
		t.Errorf("Expected summary line in diffstat output, got:\n%s", outStat)
	}
}

// TestLive_Run_Suite verifies 'gh run' commands against live Buildbucket checks.
func TestLive_Run_Suite(t *testing.T) {
	// 1. gh run list on change 472267
	outList, err := executeLiveCommand("run", "list", "472267")
	if err != nil {
		t.Fatalf("run list failed: %v\nOutput: %s", err, outList)
	}
	if !strings.Contains(outList, "static-checks-pigweed") {
		t.Errorf("Expected 'static-checks-pigweed' in run list output, got:\n%s", outList)
	}

	// 2. gh run view --log-failed on passing change 472267
	outLogPass, err := executeLiveCommand("run", "view", "472267", "--log-failed")
	if err != nil {
		t.Fatalf("run view --log-failed on passing change failed: %v\nOutput: %s", err, outLogPass)
	}
	if !strings.Contains(outLogPass, "No failed") {
		t.Errorf("Expected 'No failed' in run view output, got:\n%s", outLogPass)
	}

	// 3. gh run view --log-failed on failing change 467905/22
	outFailLog, err := executeLiveCommand("run", "view", "467905/22", "--log-failed")
	if err != nil {
		t.Fatalf("run view 467905/22 --log-failed failed: %v\nOutput: %s", err, outFailLog)
	}
	if !strings.Contains(outFailLog, "FAILURE: pigweed-lintformat") {
		t.Errorf("Expected FAILURE report in run view for 467905/22, got:\n%s", outFailLog)
	}

	// 4. gh run view -v -j on active builder
	outSteps, err := executeLiveCommand("run", "view", "472267", "-v", "-j", "static-checks-pigweed")
	if err != nil {
		t.Fatalf("run view -v -j failed: %v\nOutput: %s", err, outSteps)
	}
	if !strings.Contains(outSteps, "Steps for static-checks-pigweed") {
		t.Errorf("Expected 'Steps for static-checks-pigweed' in output, got:\n%s", outSteps)
	}

	// 5. gh run rerun -j --dry-run
	outRerun, err := executeLiveCommand("run", "rerun", "472267", "-j", "static-checks-pigweed", "--dry-run")
	if err != nil {
		t.Fatalf("run rerun -j --dry-run failed: %v\nOutput: %s", err, outRerun)
	}
	if !strings.Contains(outRerun, "[dry-run]") || !strings.Contains(outRerun, "static-checks-pigweed") {
		t.Errorf("Expected dry-run rerun output, got:\n%s", outRerun)
	}
}

// liveCheck is one row of `gh pr checks --json checks` from production.
type liveCheck struct {
	Name   string `json:"name"`
	Status string `json:"status"`
}

// liveChecks returns every blocking check that `gh pr checks <target>` reports,
// taken from production.
//
// Tests use this to discover real builder names instead of hardcoding them.
// Which builders run on a change depends on whether a Commit-Queue dry run has
// happened on the current patchset, so any hardcoded name is a fixture that
// will eventually rot.
//
// It ignores the command's error, because a non-zero exit is the expected
// result for any change that is not entirely green; only the payload matters.
func liveChecks(t *testing.T, target string) []liveCheck {
	t.Helper()

	out, err := executeLiveCommand("pr", "checks", target, "--json", "checks")
	if out == "" {
		t.Fatalf("pr checks %s --json checks printed nothing (err = %v)", target, err)
	}

	var data struct {
		Checks []liveCheck `json:"checks"`
	}
	if jsonErr := json.Unmarshal([]byte(out), &data); jsonErr != nil {
		t.Fatalf("could not parse pr checks %s --json checks: %v\nRaw:\n%s", target, jsonErr, out)
	}
	return data.Checks
}

// liveCheckStatuses returns just the Buildbucket statuses reported for target.
func liveCheckStatuses(t *testing.T, target string) []string {
	t.Helper()

	checks := liveChecks(t, target)
	statuses := make([]string, 0, len(checks))
	for _, c := range checks {
		statuses = append(statuses, c.Status)
	}
	return statuses
}

// expectedExitForStatuses derives the documented exit code from a set of
// Buildbucket statuses.
//
// This deliberately re-implements the mapping instead of calling
// classifyCheckStatus, so that the test fails if the production classifier
// drifts away from the documented contract rather than moving along with it.
func expectedExitForStatuses(statuses []string) int {
	var failed, canceled, pending int
	for _, s := range statuses {
		switch s {
		case "SUCCESS":
		case "SCHEDULED", "STARTED":
			pending++
		case "CANCELED":
			canceled++
		default:
			failed++
		}
	}
	switch {
	case failed > 0:
		return ExitCodeFailure
	case canceled > 0:
		return ExitCodeFailure
	case pending > 0:
		return ExitCodePending
	case len(statuses) == 0:
		return ExitCodeFailure
	default:
		return ExitCodeOK
	}
}

// TestLive_Checks_ExitCodeContract verifies against production Gerrit and
// Buildbucket that the exit code `gh pr checks` returns always agrees with the
// check statuses it just printed.
//
// The assertion is self-consistent rather than hard-coded, so it cannot rot as
// the referenced changes accumulate patchsets: whatever production reports, the
// exit code has to match it. Each target is chosen to exercise a different
// branch of the contract, but the test still passes if a target's state
// changes.
func TestLive_Checks_ExitCodeContract(t *testing.T) {
	targets := []struct {
		target string
		why    string
	}{
		{"472267", "current patchset of an active CL"},
		{"472267/43", "superseded patchset: passes plus canceled builds"},
		{"467905/22", "patchset with a genuinely failing builder"},
	}

	for _, tc := range targets {
		t.Run(tc.target, func(t *testing.T) {
			statuses := liveCheckStatuses(t, tc.target)
			want := expectedExitForStatuses(statuses)

			_, err := executeLiveCommand("pr", "checks", tc.target)
			got := ExitCodeFor(err)

			if got != want {
				t.Errorf("pr checks %s (%s) exited %d, want %d for statuses %v\nerr = %v",
					tc.target, tc.why, got, want, statuses, err)
			}
			t.Logf("%s (%s): %d checks, exit %d", tc.target, tc.why, len(statuses), got)
		})
	}
}

// TestLive_Checks_CanceledIsNotReportedAsFailed pins the regression that a
// patchset superseded by a newer one is described accurately.
//
// CL 472267/43 is 46 SUCCESS and 29 CANCELED with nothing failing. Reporting
// "29 of 75 checks failed" sent the reader looking for a broken build that does
// not exist. The exit code is still non-zero, because a canceled check did not
// pass, but the wording has to tell the truth.
func TestLive_Checks_CanceledIsNotReportedAsFailed(t *testing.T) {
	statuses := liveCheckStatuses(t, "472267/43")

	var canceled, failed int
	for _, s := range statuses {
		switch s {
		case "CANCELED":
			canceled++
		case "SUCCESS", "SCHEDULED", "STARTED":
		default:
			failed++
		}
	}
	if canceled == 0 {
		t.Skipf("CL 472267/43 no longer has canceled builds (statuses: %v); "+
			"this regression needs a different fixture", statuses)
	}
	if failed > 0 {
		t.Skipf("CL 472267/43 now has %d genuinely failing builds, so the "+
			"canceled wording is not the one under test", failed)
	}

	_, err := executeLiveCommand("pr", "checks", "472267/43")
	if err == nil {
		t.Fatalf("expected a non-zero exit for a patchset with %d canceled checks", canceled)
	}
	if got := ExitCodeFor(err); got != ExitCodeFailure {
		t.Errorf("exit code = %d, want %d", got, ExitCodeFailure)
	}

	msg := err.Error()
	if !strings.Contains(msg, "canceled") {
		t.Errorf("error should say the checks were canceled, got:\n%s", msg)
	}
	if strings.Contains(strings.ToLower(msg), "failed") {
		t.Errorf("error must not claim anything failed when %d checks were canceled "+
			"and none failed, got:\n%s", canceled, msg)
	}
}

// TestLive_Checks_ProcessExitCode verifies the contract at the only layer that
// actually matters to a caller: the exit status of the compiled binary.
//
// Every other test observes the error value returned by RunE. This one builds
// the real binary and reads $? through os/exec, which is what proves that
// cmd/main.go translates the error into the documented status instead of the
// blanket exit 1 it used to use.
func TestLive_Checks_ProcessExitCode(t *testing.T) {
	bin := filepath.Join(t.TempDir(), "gh-ish")
	build := exec.Command("go", "build", "-o", bin, "./cmd")
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("could not build the binary under test: %v\n%s", err, out)
	}

	for _, target := range []string{"472267", "472267/43", "467905/22"} {
		t.Run(target, func(t *testing.T) {
			want := expectedExitForStatuses(liveCheckStatuses(t, target))

			cmd := exec.Command(bin, "pr", "checks", target)
			out, err := cmd.CombinedOutput()

			got := 0
			if err != nil {
				var exitErr *exec.ExitError
				if !errors.As(err, &exitErr) {
					t.Fatalf("running %s pr checks %s: %v\n%s", bin, target, err, out)
				}
				got = exitErr.ExitCode()
			}

			if got != want {
				t.Errorf("`gh pr checks %s` exited %d, want %d\nOutput:\n%s", target, got, want, out)
			}
			if got != ExitCodeOK && got != ExitCodeFailure && got != ExitCodePending {
				t.Errorf("exit code %d is outside the documented 0/1/8 contract", got)
			}
		})
	}
}

// TestLive_View_JSONBugFields reads a bug link back out of production Gerrit.
//
// CL 466725 is submitted, so its commit message is immutable and the specific
// bug can be pinned. Doing this against an open change would rot the moment
// somebody edited the message -- the same rule that governs the check-verdict
// fixtures above.
func TestLive_View_JSONBugFields(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "466725", "--json", "number,bug,bugs")
	if err != nil {
		t.Fatalf("pr view --json bug failed: %v\nOutput: %s", err, output)
	}

	var data struct {
		Number int       `json:"number"`
		Bug    string    `json:"bug"`
		Bugs   []BugLink `json:"bugs"`
	}
	if err := json.Unmarshal([]byte(output), &data); err != nil {
		t.Fatalf("Failed to parse pr view JSON output: %v\nRaw: %s", err, output)
	}

	if data.Number != 466725 {
		t.Errorf("Expected CL 466725, got %d", data.Number)
	}
	// The submitted message reads `Fixed: b/555334846`.
	if data.Bug != "b/555334846" {
		t.Errorf("bug = %q, want %q\nRaw: %s", data.Bug, "b/555334846", output)
	}
	want := []BugLink{{ID: "b/555334846", Closes: true}}
	if diff := cmp.Diff(want, data.Bugs); diff != "" {
		t.Errorf("bugs mismatch (-want +got):\n%s\nRaw: %s", diff, output)
	}
}

// TestLive_View_BugAgreesWithCommitMessage checks the invariant rather than a
// fixture: whatever bug fields gh-ish reports for the change under development
// must be exactly what its own commit message says. This one cannot rot, since
// both sides are read from the same live change.
func TestLive_View_BugAgreesWithCommitMessage(t *testing.T) {
	output, err := executeLiveCommand("pr", "view", "472267", "--json", "bug,bugs,commitMessage")
	if err != nil {
		t.Fatalf("pr view failed: %v\nOutput: %s", err, output)
	}

	var data struct {
		Bug           string    `json:"bug"`
		Bugs          []BugLink `json:"bugs"`
		CommitMessage string    `json:"commitMessage"`
	}
	if err := json.Unmarshal([]byte(output), &data); err != nil {
		t.Fatalf("Failed to parse pr view JSON output: %v\nRaw: %s", err, output)
	}
	if data.CommitMessage == "" {
		t.Fatal("live Gerrit returned no commit message; the bug fields cannot be checked")
	}

	wantLinks := ExtractBugLinks(data.CommitMessage)
	if diff := cmp.Diff(wantLinks, data.Bugs); diff != "" {
		t.Errorf("reported bugs disagree with the commit message (-want +got):\n%s", diff)
	}
	if want := FormatBugLinks(wantLinks); data.Bug != want {
		t.Errorf("bug = %q, want %q", data.Bug, want)
	}
}

func isInteractiveLiveTest() bool {
	if val := strings.TrimSpace(os.Getenv("GHISH_INTERACTIVE")); val != "" {
		return val == "1" || strings.EqualFold(val, "true") || strings.EqualFold(val, "yes")
	}
	if os.Getenv("CI") != "" || os.Getenv("GHISH_NONINTERACTIVE") != "" {
		return false
	}
	for _, arg := range os.Args {
		if strings.Contains(arg, "TestLive_IssueLifecycle") {
			return true
		}
	}
	return false
}

func printSupervisorBanner(t *testing.T, msg string) {
	t.Helper()
	if tty, err := os.OpenFile("/dev/tty", os.O_WRONLY, 0); err == nil {
		defer tty.Close()
		fmt.Fprintln(tty, msg)
		return
	}
	t.Log(msg)
}

func pauseForSupervisor(t *testing.T, stepNum, totalSteps int, title string, issueID int64, details string) {
	t.Helper()
	pwURL := fmt.Sprintf("https://issues.pigweed.dev/issues/%d", issueID)
	corpURL := fmt.Sprintf("https://issuetracker.google.com/issues/%d", issueID)

	var sb strings.Builder
	sb.WriteString("\n================================================================================\n")
	fmt.Fprintf(&sb, "👉 [STEP %d/%d COMPLETED] %s\n", stepNum, totalSteps, title)
	sb.WriteString("--------------------------------------------------------------------------------\n")
	fmt.Fprintf(&sb, "🔗 Buganizer URL (Pigweed):  %s\n", pwURL)
	fmt.Fprintf(&sb, "🔗 Buganizer URL (Internal): %s\n", corpURL)
	if strings.TrimSpace(details) != "" {
		sb.WriteString("--------------------------------------------------------------------------------\n")
		sb.WriteString(strings.TrimSpace(details) + "\n")
	}
	sb.WriteString("================================================================================\n")

	tty, err := os.OpenFile("/dev/tty", os.O_RDWR, 0)
	if err == nil {
		defer tty.Close()
		fmt.Fprint(tty, sb.String())
		if isInteractiveLiveTest() && stepNum < totalSteps {
			fmt.Fprintf(tty, "⏸  Press [ENTER] to advance to Step %d (or Ctrl+C to abort)... ", stepNum+1)
			reader := bufio.NewReader(tty)
			_, _ = reader.ReadString('\n')
			fmt.Fprintln(tty, "")
		}
		return
	}

	t.Log(sb.String())
}

// TestLive_IssueLifecycle exercises the complete ./gh issue lifecycle against live Buganizer:
//
//	Step 1: Create a new issue (./gh issue create)
//	Step 2: View & validate JSON schema (./gh issue view --json)
//	Step 3: Edit title, priority, severity label, and assign to 'me' (./gh issue edit)
//	Step 4: Add comments and verify comment thread (./gh issue comment & view --comments)
//	Step 5: Verify issue appears in './gh issue list' and './gh issue status'
//	Step 6: Close issue as 'not planned' (OBSOLETE) (./gh issue close)
//	Step 7: Reopen issue (./gh issue reopen) and perform final close (./gh issue close)
//
// Run interactively (pauses at each step so you can inspect Buganizer in your browser):
//
//	go test -v -tags=live ./pw_ghish -run TestLive_IssueLifecycle
//
// Run non-interactively (auto-advances while still printing live URLs):
//
//	GHISH_INTERACTIVE=0 go test -v -tags=live ./pw_ghish -run TestLive_IssueLifecycle
func TestLive_IssueLifecycle(t *testing.T) {
	ctx := context.Background()

	// Verify IssueTracker OAuth credentials are available before starting.
	if _, err := DefaultIssueTrackerToken(ctx); err != nil {
		t.Skipf("Skipping live Buganizer test: no OAuth token available: %v", err)
	}

	const totalSteps = 7
	timestamp := time.Now().UTC().Format("2006-01-02 15:04:05 UTC")
	initialTitle := fmt.Sprintf("[gh-ish LIVE TEST] Issue Lifecycle Walkthrough (%s)", timestamp)
	initialBody := "This is a temporary live integration test issue created by `pw_ghish` (`./gh issue`).\n\n" +
		"It will be automatically closed as OBSOLETE at the end of the test run."

	printSupervisorBanner(t, fmt.Sprintf("\n"+
		"🚀 Starting Live Buganizer Issue Lifecycle Test\n"+
		"   Interactive Mode: %v (set GHISH_INTERACTIVE=0 to auto-advance, or GHISH_INTERACTIVE=1 to pause)\n",
		isInteractiveLiveTest()))

	// -------------------------------------------------------------------------
	// STEP 1: Create a new Buganizer issue
	// -------------------------------------------------------------------------
	createOut, err := executeLiveCommand("issue", "create",
		"-t", initialTitle,
		"-b", initialBody,
		"-P", "P3",
		"-T", "task")
	if err != nil {
		t.Fatalf("Step 1 (issue create) failed: %v\nOutput: %s", err, createOut)
	}

	issueID, err := ParseIssueID(createOut)
	if err != nil {
		// Extract b/<id> from output lines
		for _, line := range strings.Split(createOut, "\n") {
			for _, token := range strings.Fields(line) {
				if parsed, pErr := ParseIssueID(strings.TrimSuffix(token, ":")); pErr == nil && parsed > 0 {
					issueID = parsed
					break
				}
			}
			if issueID > 0 {
				break
			}
		}
	}
	if issueID <= 0 {
		t.Fatalf("Step 1: could not parse created issue ID from output:\n%s", createOut)
	}

	issueIDStr := fmt.Sprintf("%d", issueID)
	testCompletedCleanly := false

	// Safety net: guarantee the test issue is closed as OBSOLETE even if the test fails or is interrupted.
	t.Cleanup(func() {
		if !testCompletedCleanly {
			printSupervisorBanner(t, fmt.Sprintf("🧹 Cleanup: closing test issue b/%d as OBSOLETE...", issueID))
			_, _ = executeLiveCommand("issue", "close", issueIDStr,
				"--reason", "not planned",
				"-c", "Closed automatically by TestLive_IssueLifecycle cleanup handler.")
		}
	})

	pauseForSupervisor(t, 1, totalSteps,
		fmt.Sprintf("Created new Buganizer issue b/%d", issueID),
		issueID,
		fmt.Sprintf("CLI Output:\n%s\nVerify in Buganizer:\n  • Title: %s\n  • Priority: P3, Type: TASK, Status: NEW",
			strings.TrimSpace(createOut), initialTitle))

	// -------------------------------------------------------------------------
	// STEP 2: View issue & validate JSON schema
	// -------------------------------------------------------------------------
	viewJSONOut, err := executeLiveCommand("issue", "view", issueIDStr,
		"--json", "number,title,state,priority,type,componentId,url")
	if err != nil {
		t.Fatalf("Step 2 (issue view --json) failed: %v\nOutput: %s", err, viewJSONOut)
	}

	var viewData struct {
		Number      int64  `json:"number"`
		Title       string `json:"title"`
		State       string `json:"state"`
		Priority    string `json:"priority"`
		Type        string `json:"type"`
		ComponentID int64  `json:"componentId"`
		URL         string `json:"url"`
	}
	if err := json.Unmarshal([]byte(viewJSONOut), &viewData); err != nil {
		t.Fatalf("Step 2: failed to unmarshal JSON: %v\nRaw:\n%s", err, viewJSONOut)
	}
	if viewData.Number != issueID || viewData.State != "OPEN" || viewData.Priority != "P3" {
		t.Errorf("Step 2: unexpected JSON fields: %+v", viewData)
	}

	humanViewOut, err := executeLiveCommand("issue", "view", issueIDStr)
	if err != nil {
		t.Fatalf("Step 2 (human issue view) failed: %v", err)
	}

	pauseForSupervisor(t, 2, totalSteps,
		fmt.Sprintf("Inspected issue b/%d (human & JSON view)", issueID),
		issueID,
		fmt.Sprintf("Human View Output:\n%s", strings.TrimSpace(humanViewOut)))

	// -------------------------------------------------------------------------
	// STEP 3: Edit issue (Title, Priority -> P2, Severity -> S3, Assignee -> me)
	// -------------------------------------------------------------------------
	updatedTitle := fmt.Sprintf("[gh-ish LIVE TEST] Updated & Assigned (%s)", timestamp)
	editOut, err := executeLiveCommand("issue", "edit", issueIDStr,
		"-t", updatedTitle,
		"-P", "P2",
		"--add-label", "S3",
		"--add-assignee", "me")
	if err != nil {
		t.Fatalf("Step 3 (issue edit) failed: %v\nOutput: %s", err, editOut)
	}

	afterEditJSON, err := executeLiveCommand("issue", "view", issueIDStr,
		"--json", "title,priority,severity,assignees")
	if err != nil {
		t.Fatalf("Step 3 verification view failed: %v", err)
	}

	pauseForSupervisor(t, 3, totalSteps,
		fmt.Sprintf("Edited issue b/%d: Title, Priority=P2, Severity=S3, Assignee=me", issueID),
		issueID,
		fmt.Sprintf("CLI Output:\n%s\nUpdated JSON State:\n%s\nVerify in Buganizer:\n  • Status automatically transitioned from NEW -> ASSIGNED\n  • Priority is now P2, Severity is S3\n  • Assigned to your email",
			strings.TrimSpace(editOut), strings.TrimSpace(afterEditJSON)))

	// -------------------------------------------------------------------------
	// STEP 4: Add comments and verify comment thread
	// -------------------------------------------------------------------------
	comment1Out, err := executeLiveCommand("issue", "comment", issueIDStr,
		"-b", "Live test comment #1: Verifying `./gh issue comment` execution.")
	if err != nil {
		t.Fatalf("Step 4 (first comment) failed: %v\nOutput: %s", err, comment1Out)
	}

	comment2Out, err := executeLiveCommand("issue", "comment", issueIDStr,
		"-b", "Live test comment #2: Verifying multi-comment thread formatting.")
	if err != nil {
		t.Fatalf("Step 4 (second comment) failed: %v\nOutput: %s", err, comment2Out)
	}

	viewCommentsOut, err := executeLiveCommand("issue", "view", issueIDStr, "--comments")
	if err != nil {
		t.Fatalf("Step 4 (issue view --comments) failed: %v", err)
	}
	if !strings.Contains(viewCommentsOut, "temporary live integration test issue") {
		t.Errorf("Step 4: expected initial issue description in view output, got:\n%s", viewCommentsOut)
	}
	if strings.Contains(viewCommentsOut, "by unknown") {
		t.Errorf("Step 4: expected comment authors to resolve to real email addresses (not 'unknown'), got:\n%s", viewCommentsOut)
	}
	idx1 := strings.Index(viewCommentsOut, "Live test comment #1")
	idx2 := strings.Index(viewCommentsOut, "Live test comment #2")
	if idx1 < 0 || idx2 < 0 || idx1 > idx2 {
		t.Errorf("Step 4: expected both comments in chronological order (#1 before #2), got:\n%s", viewCommentsOut)
	}

	pauseForSupervisor(t, 4, totalSteps,
		fmt.Sprintf("Posted 2 comments and verified thread on b/%d", issueID),
		issueID,
		fmt.Sprintf("View with --comments:\n%s\nVerify in Buganizer:\n  • Both comments appear in the issue history",
			strings.TrimSpace(viewCommentsOut)))

	// -------------------------------------------------------------------------
	// STEP 5: Verify issue in `issue list` and `issue status`
	// -------------------------------------------------------------------------
	listOut, err := executeLiveCommand("issue", "list", "--assignee", "me", "--limit", "15")
	if err != nil {
		t.Fatalf("Step 5 (issue list) failed: %v\nOutput: %s", err, listOut)
	}

	statusOut, err := executeLiveCommand("issue", "status")
	if err != nil {
		t.Fatalf("Step 5 (issue status) failed: %v\nOutput: %s", err, statusOut)
	}

	pauseForSupervisor(t, 5, totalSteps,
		fmt.Sprintf("Verified b/%d in `./gh issue list` and `./gh issue status`", issueID),
		issueID,
		fmt.Sprintf("`./gh issue status` output:\n%s", strings.TrimSpace(statusOut)))

	// -------------------------------------------------------------------------
	// STEP 6: Close the issue as 'not planned' (OBSOLETE)
	// -------------------------------------------------------------------------
	closeOut, err := executeLiveCommand("issue", "close", issueIDStr,
		"--reason", "not planned",
		"-c", "Closing temporarily to test `./gh issue close` -> OBSOLETE state.")
	if err != nil {
		t.Fatalf("Step 6 (issue close) failed: %v\nOutput: %s", err, closeOut)
	}

	closedJSONOut, err := executeLiveCommand("issue", "view", issueIDStr, "--json", "state,stateReason")
	if err != nil {
		t.Fatalf("Step 6 verification failed: %v", err)
	}

	pauseForSupervisor(t, 6, totalSteps,
		fmt.Sprintf("Closed issue b/%d as 'not planned' (OBSOLETE)", issueID),
		issueID,
		fmt.Sprintf("CLI Output:\n%s\nJSON state:\n%s\nVerify in Buganizer:\n  • Issue status is now OBSOLETE (Closed)",
			strings.TrimSpace(closeOut), strings.TrimSpace(closedJSONOut)))

	// -------------------------------------------------------------------------
	// STEP 7: Reopen the issue, verify ASSIGNED state, then perform final close
	// -------------------------------------------------------------------------
	reopenOut, err := executeLiveCommand("issue", "reopen", issueIDStr,
		"-c", "Reopening to test `./gh issue reopen` state restoration.")
	if err != nil {
		t.Fatalf("Step 7 (issue reopen) failed: %v\nOutput: %s", err, reopenOut)
	}

	reopenedJSONOut, err := executeLiveCommand("issue", "view", issueIDStr, "--json", "state")
	if err != nil {
		t.Fatalf("Step 7 verification failed: %v", err)
	}
	if !strings.Contains(reopenedJSONOut, `"OPEN"`) {
		t.Errorf("Step 7: expected reopened issue state to be OPEN, got:\n%s", reopenedJSONOut)
	}

	if isInteractiveLiveTest() {
		pauseForSupervisor(t, 7, totalSteps+1,
			fmt.Sprintf("Reopened issue b/%d (restored to ASSIGNED)", issueID),
			issueID,
			fmt.Sprintf("CLI Output:\n%s\nVerify in Buganizer:\n  • Issue is open again (ASSIGNED)\n  • Next: Pressing ENTER will perform the final close.",
				strings.TrimSpace(reopenOut)))
	}

	finalCloseOut, err := executeLiveCommand("issue", "close", issueIDStr,
		"--reason", "not planned",
		"-c", "Live integration test completed successfully. Closing ephemeral test issue.")
	if err != nil {
		t.Fatalf("Step 7 (final close) failed: %v\nOutput: %s", err, finalCloseOut)
	}

	testCompletedCleanly = true
	pauseForSupervisor(t, totalSteps, totalSteps,
		fmt.Sprintf("Lifecycle test complete! Issue b/%d is closed (OBSOLETE)", issueID),
		issueID,
		fmt.Sprintf("Final Close Output:\n%s\n🎉 All 7 Buganizer lifecycle steps succeeded!",
			strings.TrimSpace(finalCloseOut)))
}
