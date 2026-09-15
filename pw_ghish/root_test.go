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

import (
	"context"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

func TestGerritURL_HostFlag(t *testing.T) {
	tests := []struct {
		desc string
		host string
		want string
	}{
		{
			desc: "googlesource.com host",
			host: "fuchsia-review.googlesource.com",
			want: "https://fuchsia-review.googlesource.com/a",
		},
		{
			desc: "googlesource.com host with https",
			host: "https://fuchsia-review.googlesource.com",
			want: "https://fuchsia-review.googlesource.com/a",
		},
		{
			desc: "turquoise-internal host",
			host: "turquoise-internal-review.googlesource.com",
			want: "https://turquoise-internal-review.googlesource.com/a",
		},
		{
			desc: "other host",
			host: "example.com",
			want: "https://example.com",
		},
	}

	for _, tt := range tests {
		t.Run(tt.desc, func(t *testing.T) {
			c := &Config{Host: tt.host}
			got, err := c.GerritURL(t.Context())
			if err != nil {
				t.Fatalf("GerritURL() failed: %v", err)
			}
			if got != tt.want {
				t.Errorf("GerritURL() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestGerritURL_GitRemote(t *testing.T) {
	c := &Config{
		Git: &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				stdout.Write([]byte("https://pigweed.googlesource.com/pigweed/pigweed\n"))
				return nil
			},
		},
	}
	got, err := c.GerritURL(context.Background())
	if err != nil {
		t.Fatalf("GerritURL() failed: %v", err)
	}
	want := "https://pigweed-review.googlesource.com/a"
	if got != want {
		t.Errorf("GerritURL() = %q, want %q", got, want)
	}
}

func TestGerritURL_MissingOriginRemoteErrorWithGuidance(t *testing.T) {
	c := &Config{
		Git: &MockGitRunner{
			RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
				return fmt.Errorf("error: key does not contain a section: remote.origin.url")
			},
		},
	}
	_, err := c.GerritURL(context.Background())
	if err == nil {
		t.Fatal("expected error when remote.origin.url is missing, got nil")
	}
	if !strings.Contains(err.Error(), "could not detect Gerrit host") {
		t.Errorf("expected error to mention 'could not detect Gerrit host', got: %v", err)
	}
	if !strings.Contains(err.Error(), "git remote add origin") {
		t.Errorf("expected error to suggest 'git remote add origin', got: %v", err)
	}
	if !strings.Contains(err.Error(), "--host") {
		t.Errorf("expected error to suggest '--host', got: %v", err)
	}
}

func TestNewGerritClient_RPCFailure(t *testing.T) {
	orig := getRPCClient
	getRPCClient = func(ctx context.Context, host string) (*http.Client, error) {
		return nil, fmt.Errorf("mocked RPC failure")
	}
	defer func() { getRPCClient = orig }()

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Host: "example.com"})

	_, err := NewGerritClient(context.Background(), cmd)
	if err == nil {
		t.Fatal("Expected NewGerritClient to fail due to RPC failure, but it succeeded")
	}
}

func TestFallbackTransport(t *testing.T) {
	var reqs []*http.Request

	mock := &mockTransport{
		roundTrip: func(req *http.Request) (*http.Response, error) {
			reqs = append(reqs, req)
			if len(reqs) == 1 {
				return &http.Response{StatusCode: 401}, nil
			}
			return &http.Response{StatusCode: 200}, nil
		},
	}

	transport := &fallbackTransport{base: mock}

	req, err := http.NewRequest("GET", "https://fuchsia-review.googlesource.com/a/changes/123", nil)
	if err != nil {
		t.Fatalf("http.NewRequest failed: %v", err)
	}

	resp, err := transport.RoundTrip(req)
	if err != nil {
		t.Fatalf("RoundTrip failed: %v", err)
	}

	if resp.StatusCode != 200 {
		t.Errorf("StatusCode = %d, want 200", resp.StatusCode)
	}

	if len(reqs) != 2 {
		t.Fatalf("Expected 2 requests, got %d", len(reqs))
	}

	if reqs[0].URL.Path != "/a/changes/123" {
		t.Errorf("reqs[0].URL.Path = %q, want %q", reqs[0].URL.Path, "/a/changes/123")
	}

	if reqs[1].URL.Path != "/changes/123" {
		t.Errorf("reqs[1].URL.Path = %q, want %q", reqs[1].URL.Path, "/changes/123")
	}
}

func TestFindLatestChangeBySubject(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/*", http.StatusOK, []map[string]any{
		{"_number": 1249324, "id": "I123"},
	})

	cmd := &cobra.Command{}
	cmd.SetContext(context.Background())
	SetConfig(cmd, &Config{Host: server.URL})

	num, err := FindLatestChangeBySubject(context.Background(), cmd, "[cog] Update the cog workspace SKILL")
	if err != nil {
		t.Fatalf("FindLatestChangeBySubject failed: %v", err)
	}

	if num != 1249324 {
		t.Errorf("FindLatestChangeBySubject(...) = %d, want 1249324", num)
	}

	if len(server.Requests()) == 0 {
		t.Fatal("expected request")
	}
	q := server.LastRequest().URL.Query().Get("q")
	if !strings.Contains(q, "owner:self") {
		t.Errorf("expected query to contain owner:self, got %s", q)
	}
	if !strings.Contains(q, `message:"[cog] Update the cog workspace SKILL"`) {
		t.Errorf("expected query to contain subject, got %s", q)
	}
}

func TestGlobalFlagsBeforeSubcommand(t *testing.T) {
	oldHost := HostFlag
	defer func() { HostFlag = oldHost }()

	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	fs.StringVar(&HostFlag, "host", "", "Gerrit host to connect to")

	args := []string{"--host", "pigweed-review.googlesource.com", "pr", "view", "1298665"}
	if err := fs.Parse(args); err != nil {
		t.Fatalf("fs.Parse() failed: %v", err)
	}

	if HostFlag != "pigweed-review.googlesource.com" {
		t.Errorf("HostFlag = %q, want %q", HostFlag, "pigweed-review.googlesource.com")
	}

	if len(fs.Args()) != 3 || fs.Args()[0] != "pr" {
		t.Errorf("fs.Args() = %v, want [pr view 1298665]", fs.Args())
	}

	RootCmd.SetArgs(fs.Args())
	if err := RootCmd.ParseFlags(fs.Args()); err != nil {
		t.Fatalf("RootCmd.ParseFlags() failed: %v", err)
	}

	if HostFlag != "pigweed-review.googlesource.com" {
		t.Errorf("HostFlag after Cobra parse = %q, want %q", HostFlag, "pigweed-review.googlesource.com")
	}

	if RootCmd.Use != "gh-ish" {
		t.Errorf("init() set RootCmd.Use = %q, want %q", RootCmd.Use, "gh-ish")
	}

	if got := RootCmd.Annotations[cobra.CommandDisplayNameAnnotation]; got != "gh-ish" {
		t.Errorf("init() set CommandDisplayNameAnnotation = %q, want %q", got, "gh-ish")
	}
}

func TestExecute_InvokedAs(t *testing.T) {
	t.Setenv("GH_ISH_INVOKED_AS", "pw gh")
	RootCmd.SetArgs([]string{"--help"})
	RootCmd.SetOut(io.Discard)
	if err := Execute(); err != nil {
		t.Fatalf("Execute() with GH_ISH_INVOKED_AS=%q failed: %v", "pw gh", err)
	}

	if RootCmd.Use != "pw gh" {
		t.Errorf("Execute() set RootCmd.Use = %q, want %q", RootCmd.Use, "pw gh")
	}

	if got := RootCmd.Annotations[cobra.CommandDisplayNameAnnotation]; got != "pw gh" {
		t.Errorf("Execute() set CommandDisplayNameAnnotation = %q, want %q", got, "pw gh")
	}

	if got := RootCmd.CommandPath(); got != "pw gh" {
		t.Errorf("RootCmd.CommandPath() = %q, want %q", got, "pw gh")
	}

	if !strings.Contains(RootCmd.Long, "pw gh pr list") {
		t.Errorf("Execute() set RootCmd.Long = %s, want substring %q", RootCmd.Long, "pw gh pr list")
	}
}

func TestExecute_InvokedAs_Fallback(t *testing.T) {
	t.Setenv("GH_ISH_INVOKED_AS", "")
	RootCmd.Annotations = nil

	RootCmd.SetArgs([]string{"--help"})
	RootCmd.SetOut(io.Discard)
	if err := Execute(); err != nil {
		t.Fatalf("Execute() with empty GH_ISH_INVOKED_AS failed: %v", err)
	}

	if RootCmd.Use != "gh-ish" {
		t.Errorf("Execute() set RootCmd.Use = %q, want %q", RootCmd.Use, "gh-ish")
	}

	if got := RootCmd.Annotations[cobra.CommandDisplayNameAnnotation]; got != "gh-ish" {
		t.Errorf("Execute() set CommandDisplayNameAnnotation = %q, want %q", got, "gh-ish")
	}

	if got := RootCmd.CommandPath(); got != "gh-ish" {
		t.Errorf("RootCmd.CommandPath() = %q, want %q", got, "gh-ish")
	}

	if !strings.Contains(RootCmd.Long, "gh-ish pr list") {
		t.Errorf("Execute() set RootCmd.Long = %s, want substring %q", RootCmd.Long, "gh-ish pr list")
	}
}

// TestCheatSheet_DocumentsGHDivergences guards the root help text: it must not
// promise that 'gh' flags transfer wholesale, and it must name the spellings
// that silently mean something else here.
func TestCheatSheet_DocumentsGHDivergences(t *testing.T) {
	setupRootCmdHelp()
	long := RootCmd.Long

	// The old text told agents to assume gh flags work. They do not.
	if strings.Contains(long, "assume standard 'gh' flags work") {
		t.Errorf("RootCmd.Long still claims gh flags work; got:\n%s", long)
	}

	for _, want := range []string{
		"WHERE THIS DIFFERS FROM THE REAL 'gh'",
		"Gerrit 'reviewer:'",
		"a Gerrit vote predicate",
		"--json state",
		"Long form only",
	} {
		if !strings.Contains(long, want) {
			t.Errorf("RootCmd.Long missing %q; got:\n%s", want, long)
		}
	}

	// Every shorthand we refuse to bind must have its long form named here,
	// so the unknown-shorthand error has an answer somewhere.
	for _, tc := range ghShorthandCollisions {
		if !strings.Contains(long, "--"+tc.longName) {
			t.Errorf("RootCmd.Long never mentions --%s, but -%s was unbound in its favor",
				tc.longName, tc.shorthand)
		}
	}

	// The help text is assembled with Sprintf; a stray verb would surface as a
	// %!x(...) marker rather than an error.
	if strings.Contains(long, "%!") {
		t.Errorf("RootCmd.Long contains a Sprintf error marker; got:\n%s", long)
	}
}

// ghShorthandCollisions lists shorthands that must stay UNBOUND here because
// the real 'gh' binds them to something else. Binding one makes a 'gh' habit
// succeed with a different meaning, which is strictly worse than failing:
// '-a alice@google.com' would enable auto-submit and silently drop the name.
//
// The 'gh' column was read from the cli/cli source on trunk, not from memory.
// '-q' is gh's --jq on every command that accepts --json; it is listed here
// because the habit is cross-command, and casting a Commit-Queue vote by
// accident is expensive.
var ghShorthandCollisions = []struct {
	path      string // command path under the root, e.g. "pr create"
	shorthand string // the shorthand that must not be bound
	longName  string // the long flag it used to be bound to, which must survive
	ghMeaning string // what the shorthand means in the real gh
}{
	{"pr create", "a", "auto", "--assignee"},
	{"pr push", "a", "auto", "--assignee"},
	{"pr create", "p", "publish", "--project"},
	{"pr push", "p", "publish", "--project"},
	{"pr create", "f", "force", "--fill"},
	{"pr create", "q", "cq", "--jq"},
	{"pr push", "q", "cq", "--jq"},
	{"pr edit", "q", "cq", "--jq"},
	{"pr review", "q", "cq", "--jq"},
	{"pr merge", "q", "cq", "--jq"},
	{"pr edit", "m", "message", "--milestone"},
	{"pr merge", "m", "message", "--merge"},
}

// findCommandForTest resolves a command path such as "pr create".
func findCommandForTest(t *testing.T, path string) *cobra.Command {
	t.Helper()
	cmd, _, err := RootCmd.Find(strings.Fields(path))
	if err != nil {
		t.Fatalf("RootCmd.Find(%q) failed: %v", path, err)
	}
	// Find returns the deepest match, which is the PARENT when the leaf does
	// not exist, and reports no error for that case. Confirm we landed on the
	// command that was actually asked for.
	if !strings.HasSuffix(cmd.CommandPath(), " "+path) {
		t.Fatalf("RootCmd.Find(%q) resolved to %q; command %q does not exist", path, cmd.CommandPath(), path)
	}
	return cmd
}

func TestFlags_GHShorthandCollisionsAreUnbound(t *testing.T) {
	for _, tc := range ghShorthandCollisions {
		t.Run(tc.path+" -"+tc.shorthand, func(t *testing.T) {
			cmd := findCommandForTest(t, tc.path)

			if f := cmd.Flags().ShorthandLookup(tc.shorthand); f != nil {
				t.Errorf("%q binds -%s to --%s, but in the real gh -%s means %s. "+
					"A gh habit would succeed here with the wrong meaning; keep the long form only.",
					tc.path, tc.shorthand, f.Name, tc.shorthand, tc.ghMeaning)
			}

			// Guard against "fixing" the above by deleting the flag outright.
			if cmd.Flags().Lookup(tc.longName) == nil {
				t.Errorf("%q no longer has --%s; unbinding -%s must not remove the flag itself",
					tc.path, tc.longName, tc.shorthand)
			}
		})
	}
}

// TestFlags_RemovedShorthandsFailLoudly covers the user-visible half: the old
// spellings must be rejected during flag parsing, before any command runs, so
// no Gerrit call is ever made with a misread argument.
func TestFlags_RemovedShorthandsFailLoudly(t *testing.T) {
	// Hermetic on purpose. These exact arguments used to PARSE, and running
	// them issued live calls: 'pr merge -m 413992' reached the real submit
	// endpoint. If a shorthand is ever re-bound, this test must fail against a
	// mock rather than act on a production change.
	NewMockGerritServer(t).OnStatus(http.StatusNotFound)
	SetupMockConfig(t, &MockGitRunner{})

	for _, tc := range []struct {
		name string
		args []string
	}{
		{"create -a swallows the assignee and auto-submits", []string{"pr", "create", "-a", "alice@google.com"}},
		{"create -p publishes draft comments", []string{"pr", "create", "-p"}},
		{"create -f skips the duplicate-Change-Id guard", []string{"pr", "create", "-f"}},
		{"create -q casts a Commit-Queue vote", []string{"pr", "create", "-q", "2"}},
		{"edit -m replaces the whole commit message", []string{"pr", "edit", "12345", "-m", "new message"}},
		{"merge -m eats the change number", []string{"pr", "merge", "-m", "413992"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := executeCommand(RootCmd, tc.args...)
			if err == nil {
				t.Fatalf("executeCommand(%q) succeeded; the shorthand must be rejected", tc.args)
			}
			if !strings.Contains(err.Error(), "unknown shorthand flag") {
				t.Errorf("executeCommand(%q) error = %q, want an unknown-shorthand parse error", tc.args, err)
			}
		})
	}
}

func TestGetConfig_NilSafety(t *testing.T) {
	if cfg := GetConfig(nil); cfg != nil {
		t.Errorf("GetConfig(nil) = %v, want nil", cfg)
	}

	cmdWithoutCtx := &cobra.Command{}
	if cfg := GetConfig(cmdWithoutCtx); cfg != nil {
		t.Errorf("GetConfig(cmdWithoutCtx) = %v, want nil", cfg)
	}
}

func TestNewGerritClient_FallbackConfig(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(")]}'\n{\"_account_id\": 1}"))
	}))
	defer server.Close()

	oldHost := HostFlag
	t.Cleanup(func() { HostFlag = oldHost })
	HostFlag = server.URL

	// Command with no config set in context
	cmd := &cobra.Command{}
	client, err := NewGerritClient(context.Background(), cmd)
	if err != nil {
		t.Fatalf("NewGerritClient failed with unconfigured cmd: %v", err)
	}
	if client == nil {
		t.Fatal("Expected non-nil client")
	}
}

func TestRootCmd_UnknownProfileError(t *testing.T) {
	resetAllFlags(RootCmd)
	SetTestProfile(t, "nonexistent_profile")

	RootCmd.SetArgs([]string{"pr", "view", "123"})
	err := RootCmd.Execute()
	if err == nil {
		t.Fatal("expected error when unknown profile specified, got nil")
	}
	if !strings.Contains(err.Error(), "unknown profile") {
		t.Errorf("expected error about unknown profile, got: %v", err)
	}
}

func TestRootCmd_SilenceUsage(t *testing.T) {
	if !RootCmd.SilenceUsage {
		t.Errorf("Expected RootCmd.SilenceUsage to be true, got false")
	}
}

func TestRootAliases(t *testing.T) {
	setupRootAliases()

	expectedRootCommands := []string{"checks", "view", "diff", "status"}
	for _, name := range expectedRootCommands {
		cmd, _, err := RootCmd.Find([]string{name})
		if err != nil || cmd == nil {
			t.Errorf("Expected root alias command %q to be registered on RootCmd, got err: %v", name, err)
		} else if cmd.Name() != name {
			t.Errorf("Expected command name %q, got %q", name, cmd.Name())
		}
	}
}

func TestRootAliases_Execution(t *testing.T) {
	server := NewMockGerritServer(t)
	server.OnJSON("GET", "/changes/12345/revisions/current/files*", http.StatusOK, map[string]any{
		"/COMMIT_MSG":      map[string]any{},
		"pw_ghish/file.go": map[string]any{"lines_inserted": 1, "lines_deleted": 1},
	})
	server.OnJSON("GET", "/changes/12345*", http.StatusOK, map[string]any{
		"id":               "pigweed~main~I12345",
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"_number":          12345,
		"subject":          "Alias Test Change",
		"status":           "NEW",
		"current_revision": "rev1",
		"revisions": map[string]any{
			"rev1": map[string]any{"_number": 1},
		},
	})
	server.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": []map[string]any{
			{
				"id": "111",
				"builder": map[string]any{
					"project": "pigweed",
					"bucket":  "try",
					"builder": "builder-pass",
				},
				"status": "SUCCESS",
			},
		},
	})

	// 1. Root "view 12345"
	outView, err := executeCommand(RootCmd, "view", "12345")
	if err != nil {
		t.Fatalf("root 'view 12345' failed: %v\nOutput: %s", err, outView)
	}
	if !strings.Contains(outView, "Alias Test Change") {
		t.Errorf("Expected 'view 12345' output to contain subject, got:\n%s", outView)
	}

	// 2. Root "checks 12345"
	outChecks, err := executeCommand(RootCmd, "checks", "12345", "--buildbucket-host", server.URL)
	if err != nil {
		t.Fatalf("root 'checks 12345' failed: %v\nOutput: %s", err, outChecks)
	}
	if !strings.Contains(outChecks, "builder-pass") {
		t.Errorf("Expected 'checks 12345' output to contain builder-pass, got:\n%s", outChecks)
	}

	// 3. Root "diff 12345 --name-only"
	outDiff, err := executeCommand(RootCmd, "diff", "12345", "--name-only")
	if err != nil {
		t.Fatalf("root 'diff 12345 --name-only' failed: %v\nOutput: %s", err, outDiff)
	}
	if !strings.Contains(outDiff, "pw_ghish/file.go") {
		t.Errorf("Expected 'diff 12345 --name-only' to list file.go, got:\n%s", outDiff)
	}
}

func TestCleanGerritHost(t *testing.T) {
	tests := []struct {
		input string
		want  string
	}{
		{"https://pigweed-review.googlesource.com/a", "pigweed-review.googlesource.com"},
		{"https://pigweed-review.googlesource.com/a/", "pigweed-review.googlesource.com"},
		{"http://pigweed-review.googlesource.com/", "pigweed-review.googlesource.com"},
		{"https://pigweed-review.googlesource.com/c/pigweed/+/12345", "pigweed-review.googlesource.com"},
		{"pigweed-review.googlesource.com/a", "pigweed-review.googlesource.com"},
		{"pigweed-review.googlesource.com/", "pigweed-review.googlesource.com"},
		{"localhost:8080", "localhost:8080"},
		{"http://localhost:8080/a/", "localhost:8080"},
		{"", ""},
	}

	for _, tc := range tests {
		t.Run(tc.input, func(t *testing.T) {
			if got := CleanGerritHost(tc.input); got != tc.want {
				t.Errorf("CleanGerritHost(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}

func TestConfigGerritHost(t *testing.T) {
	ctx := context.Background()

	t.Run("resolves from Config.Host", func(t *testing.T) {
		cfg := &Config{Host: "https://pigweed-review.googlesource.com/a"}
		if got := cfg.GerritHost(ctx); got != "pigweed-review.googlesource.com" {
			t.Errorf("got %q, want pigweed-review.googlesource.com", got)
		}
	})

	t.Run("falls back to HostFlag when Config is nil", func(t *testing.T) {
		oldFlag := HostFlag
		HostFlag = "https://fuchsia-review.googlesource.com/a/"
		defer func() { HostFlag = oldFlag }()

		var nilCfg *Config
		if got := nilCfg.GerritHost(ctx); got != "fuchsia-review.googlesource.com" {
			t.Errorf("got %q, want fuchsia-review.googlesource.com", got)
		}
	})
}
