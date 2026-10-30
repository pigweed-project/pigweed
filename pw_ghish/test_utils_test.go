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
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"maps"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
	"github.com/spf13/pflag"
)

// resetAllFlags resets every flag in the command tree to its default and
// clears its Changed bit, except the flags in testPreservedFlags: tests set
// their package variables directly (e.g. HostFlag = server.URL or
// SetTestProfile) before executing, so only their Changed bit is cleared here.
// Callers that execute commands use snapshotFlags (via
// executeCommandWithStdin or isolateRootCmdForTest) to undo the execution's
// flag values and Changed bits afterwards, so a parsed command line (e.g.
// `--host`, `--profile fuchsia`) never leaks into later tests that read flag
// state directly (isHostFlagChanged, cmd.Flags().Changed, package flag vars).
func resetAllFlags(cmd *cobra.Command) {
	if !cmd.HasParent() {
		ResetAuthTokenCaches()
	}
	cmd.Flags().VisitAll(func(f *pflag.Flag) {
		if testPreservedFlags[f.Name] {
			f.Changed = false
			return
		}
		if s, ok := f.Value.(pflag.SliceValue); ok {
			_ = s.Replace(nil)
		} else {
			_ = f.Value.Set(f.DefValue)
		}
		f.Changed = false
	})
	cmd.PersistentFlags().VisitAll(func(f *pflag.Flag) {
		if testPreservedFlags[f.Name] {
			f.Changed = false
			return
		}
		if s, ok := f.Value.(pflag.SliceValue); ok {
			_ = s.Replace(nil)
		} else {
			_ = f.Value.Set(f.DefValue)
		}
		f.Changed = false
	})
	cmd.SetContext(context.Background())
	for _, child := range cmd.Commands() {
		resetAllFlags(child)
	}
}

// flagState is the value and Changed bit of one flag at snapshot time.
type flagState struct {
	value   string
	slice   []string
	isSlice bool
	changed bool
}

// snapshotFlags records the value and Changed bit of every local and
// persistent flag on every command in the tree rooted at root and returns a
// function that restores them. Flags shared between commands (inherited
// persistent flags, or several flags bound to one package variable) are
// recorded once per *pflag.Flag.
func snapshotFlags(root *cobra.Command) (restore func()) {
	states := make(map[*pflag.Flag]flagState)
	record := func(f *pflag.Flag) {
		if _, seen := states[f]; seen {
			return
		}
		st := flagState{value: f.Value.String(), changed: f.Changed}
		if sv, ok := f.Value.(pflag.SliceValue); ok {
			st.slice, st.isSlice = slices.Clone(sv.GetSlice()), true
		}
		states[f] = st
	}
	var walk func(*cobra.Command)
	walk = func(cmd *cobra.Command) {
		cmd.Flags().VisitAll(record)
		cmd.PersistentFlags().VisitAll(record)
		for _, child := range cmd.Commands() {
			walk(child)
		}
	}
	walk(root)
	return func() {
		for f, st := range states {
			if f.Value.String() != st.value {
				var err error
				if st.isSlice {
					err = f.Value.(pflag.SliceValue).Replace(st.slice)
				} else {
					err = f.Value.Set(st.value)
				}
				if err != nil {
					panic(fmt.Sprintf("restoring flag --%s to %q: %v", f.Name, st.value, err))
				}
			}
			f.Changed = st.changed
		}
	}
}

// isolateRootCmdForTest resets RootCmd's flags like executeCommand does and
// restores their values, Changed bits, args, and output writers when t
// finishes. Use it in tests that call RootCmd.Execute() directly.
func isolateRootCmdForTest(t *testing.T) {
	t.Helper()
	setupRootAliases()
	resetAllFlags(RootCmd)
	restore := snapshotFlags(RootCmd)
	t.Cleanup(func() {
		restore()
		RootCmd.SetArgs(nil)
		RootCmd.SetOut(nil)
		RootCmd.SetErr(nil)
	})
}

// testPreservedFlags names the flags whose package variables tests assign
// directly; see resetAllFlags.
var testPreservedFlags = map[string]bool{"host": true, "profile": true, "buildbucket-host": true}

func init() {
	setupRootAliases()
	// Pin scheduleNowFn to 19:00 UTC (12:00 PM PDT) in tests so evening
	// times (21:00-23:59) resolve to "today" and post-midnight times
	// (00:00-02:59) resolve to "tomorrow" deterministically regardless of
	// when the test suite runs.
	scheduleNowFn = func() time.Time {
		return time.Date(2026, 9, 30, 19, 0, 0, 0, time.UTC) // 12:00 PM PDT
	}
}

// preserveRootCmdState snapshots the RootCmd state that Execute() mutates
// (Use, Long, and Annotations via setupRootCmdHelp, and the group RunE
// handlers installed by rejectUnknownSubcommands) and restores it when t
// finishes, so tests that call Execute() do not change the command tree seen
// by tests that run after them.
func preserveRootCmdState(t *testing.T) {
	t.Helper()
	use, long := RootCmd.Use, RootCmd.Long
	annotations := maps.Clone(RootCmd.Annotations)
	runEs := make(map[*cobra.Command]func(*cobra.Command, []string) error)
	var walk func(*cobra.Command)
	walk = func(cmd *cobra.Command) {
		runEs[cmd] = cmd.RunE
		for _, child := range cmd.Commands() {
			walk(child)
		}
	}
	walk(RootCmd)
	t.Cleanup(func() {
		RootCmd.Use, RootCmd.Long = use, long
		RootCmd.Annotations = annotations
		for cmd, runE := range runEs {
			cmd.RunE = runE
		}
		RootCmd.SetArgs(nil)
		RootCmd.SetOut(nil)
	})
}

// executeCommand runs a cobra command and captures combined stdout and stderr.
func executeCommand(root *cobra.Command, args ...string) (string, error) {
	return executeCommandWithStdin(root, nil, args...)
}

// executeCommandWithStdin runs a cobra command with the provided stdin reader
// and captures combined stdout and stderr.
func executeCommandWithStdin(root *cobra.Command, stdin io.Reader, args ...string) (string, error) {
	var buf bytes.Buffer
	err := executeCommandIO(root, stdin, &buf, &buf, args...)
	return buf.String(), err
}

// executeCommandIO runs a cobra command with stdin, capturing stdout (including
// direct writes to os.Stdout) into stdout and stderr into stderr. When stderr
// is nil or equal to stdout, stderr shares the stdout pipe so both streams
// merge in order.
func executeCommandIO(root *cobra.Command, stdin io.Reader, stdout, stderr io.Writer, args ...string) error {
	setupRootAliases()
	resetAllFlags(root)
	if MockCWD != "" {
		root.SetContext(context.WithValue(root.Context(), configKey, &Config{CWD: MockCWD}))
	}

	r, w, err := os.Pipe()
	if err != nil {
		return fmt.Errorf("creating stdout pipe: %w", err)
	}
	oldStdout := os.Stdout
	os.Stdout = w

	restoreFlags := snapshotFlags(root)
	savedHostFlag := HostFlag
	oldNewGerritClient := NewGerritClient
	NewGerritClient = func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		if isLoopbackHost(savedHostFlag) {
			return gerrit.NewClient(ctx, savedHostFlag, http.DefaultClient)
		}
		host, _ := cmd.Flags().GetString("host")
		if host == "" {
			host = HostFlag
		}
		if host == "" {
			return gerrit.NewClient(ctx, "https://fuchsia-review.googlesource.com", &http.Client{
				Transport: &mockTransport{
					roundTrip: func(req *http.Request) (*http.Response, error) {
						return nil, fmt.Errorf("no mock Gerrit server configured for %s", req.URL)
					},
				},
			})
		}
		if !strings.HasPrefix(host, "http://") && !strings.HasPrefix(host, "https://") {
			host = "https://" + host
		}
		return gerrit.NewClient(ctx, host, http.DefaultClient)
	}
	defer func() {
		NewGerritClient = oldNewGerritClient
		HostFlag = savedHostFlag
		restoreFlags()
		root.SetIn(nil)
		root.SetOut(nil)
		root.SetErr(nil)
	}()

	errOut := stderr
	if errOut == nil || errOut == stdout {
		errOut = w
	}
	root.SetIn(stdin)
	root.SetOut(w)
	root.SetErr(errOut)
	root.SetArgs(NormalizeCQArgs(args))

	readDone := make(chan struct{})
	go func() {
		_, _ = io.Copy(stdout, r)
		close(readDone)
	}()

	runErr := root.Execute()

	_ = w.Close()
	os.Stdout = oldStdout
	<-readDone
	_ = r.Close()

	return runErr
}

// MockGitRunner implements GitRunner for testing.
type MockGitRunner struct {
	mu                sync.Mutex
	Calls             []string
	CommittedMessages []string
	RunFn             func(ctx context.Context, stdout, stderr io.Writer, args ...string) error
	responses         map[string]string
	errors            map[string]error
	defaultBranch     string
	defaultCommitMsg  string
	mockGitDir        string
}

func (m *MockGitRunner) cleanupMockGitDir() {
	m.mu.Lock()
	dir := m.mockGitDir
	m.mockGitDir = ""
	m.mu.Unlock()
	if dir != "" {
		_ = os.RemoveAll(dir)
	}
}

func isOptionalDiscoveryProbe(args []string) bool {
	if len(args) == 0 {
		return false
	}
	switch args[0] {
	case "remote", "symbolic-ref", "diff-tree":
		return true
	case "branch":
		return len(args) == 2 && args[1] == "--show-current"
	case "config":
		if len(args) == 3 && args[1] == "--get" {
			k := args[2]
			return strings.HasPrefix(k, "ghish.") || strings.HasPrefix(k, "branch.") || k == "init.defaultBranch"
		}
		if len(args) == 3 && args[1] == "--get-regexp" && args[2] == `^ghish\.` {
			return true
		}
	case "rev-parse":
		if len(args) >= 2 {
			switch args[1] {
			case "--verify", "--show-toplevel", "--git-dir":
				return true
			case "--abbrev-ref":
				return len(args) >= 3 && args[len(args)-1] == "@{u}"
			}
		}
	case "rev-list":
		return len(args) >= 4 && args[1] == "--parents" && args[2] == "-n" && args[3] == "1"
	}
	return false
}

func (m *MockGitRunner) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	m.mu.Lock()
	call := strings.Join(args, " ")
	resp, hasResp := m.responses[call]
	if !hasResp && call == strings.Join(senseHeadLogArgs, " ") {
		resp, hasResp = m.synthesizeSenseHeadLogLocked()
	}
	var errToReturn error
	for pattern, err := range m.errors {
		if call == pattern || strings.HasPrefix(call, pattern) {
			errToReturn = err
			break
		}
	}
	defaultBranch := m.defaultBranch
	defaultCommitMsg := m.defaultCommitMsg
	hasHandler := hasResp || errToReturn != nil || m.RunFn != nil ||
		(defaultBranch != "" && len(args) >= 1 && args[0] == "branch") ||
		(defaultCommitMsg != "" && len(args) >= 2 && args[0] == "log" && args[1] == "-1")
	if !hasHandler && isOptionalDiscoveryProbe(args) {
		var gitDir string
		var gitDirErr error
		if call == "rev-parse --git-dir" {
			if m.mockGitDir == "" {
				m.mockGitDir, gitDirErr = os.MkdirTemp("", "ghish-mock-gitdir-*")
			}
			gitDir = m.mockGitDir
		}
		m.mu.Unlock()
		if gitDirErr != nil {
			return gitDirErr
		}
		if gitDir != "" {
			stdout.Write([]byte(gitDir + "\n"))
		}
		return nil
	}
	m.Calls = append(m.Calls, call)
	m.mu.Unlock()

	if errToReturn != nil {
		return errToReturn
	}
	if len(args) > 0 && args[0] == "commit" {
		for i := 1; i < len(args)-1; i++ {
			switch args[i] {
			case "-F", "--file":
				if data, readErr := os.ReadFile(args[i+1]); readErr == nil {
					m.mu.Lock()
					m.CommittedMessages = append(m.CommittedMessages, string(data))
					m.mu.Unlock()
				}
			case "-m", "--message":
				m.mu.Lock()
				m.CommittedMessages = append(m.CommittedMessages, args[i+1])
				m.mu.Unlock()
			}
		}
	}
	if hasResp {
		stdout.Write([]byte(resp))
		return nil
	}
	if defaultBranch != "" && len(args) >= 1 && args[0] == "branch" {
		stdout.Write([]byte(defaultBranch + "\n"))
		return nil
	}
	if defaultCommitMsg != "" && len(args) >= 2 && args[0] == "log" && args[1] == "-1" {
		stdout.Write([]byte(defaultCommitMsg))
		return nil
	}
	if m.RunFn != nil {
		return m.RunFn(ctx, stdout, stderr, args...)
	}
	return nil
}

// synthesizeSenseHeadLogLocked answers the combined sense HEAD query
// (senseHeadLogArgs) from the per-field responses
// registered for the separate `rev-parse HEAD`, `log -1 --format=%ct HEAD`,
// and `log -1 --format=%B HEAD` commands (or WithCommit's default message),
// so fixtures can keep mocking the individual fields.
func (m *MockGitRunner) synthesizeSenseHeadLogLocked() (string, bool) {
	sha, hasSHA := m.responses["rev-parse HEAD"]
	ct, hasCT := m.responses["log -1 --format=%ct HEAD"]
	msg, hasMsg := m.responses["log -1 --format=%B HEAD"]
	if !hasMsg {
		msg, hasMsg = m.defaultCommitMsg, m.defaultCommitMsg != ""
	}
	if !hasSHA && !hasCT && !hasMsg {
		return "", false
	}
	return strings.TrimSpace(sha) + "\x00" + strings.TrimSpace(ct) + "\x00" + msg, true
}

// OnCommand registers a fixed stdout response for an exact command argument string.
func (m *MockGitRunner) OnCommand(args, stdout string) *MockGitRunner {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.responses == nil {
		m.responses = make(map[string]string)
	}
	m.responses[args] = stdout
	return m
}

// OnError registers an error for a command argument string (supports prefix matching).
func (m *MockGitRunner) OnError(prefix string, err error) *MockGitRunner {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.errors == nil {
		m.errors = make(map[string]error)
	}
	m.errors[prefix] = err
	return m
}

// WithBranch configures git branch queries to return the specified branch name.
func (m *MockGitRunner) WithBranch(branch string) *MockGitRunner {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.defaultBranch = branch
	return m
}

// WithCommit configures git log -1 queries to return the specified commit message.
func (m *MockGitRunner) WithCommit(msg string) *MockGitRunner {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.defaultCommitMsg = msg
	return m
}

// NewMockGit creates a MockGitRunner, registers it with SetMockGit, and returns it.
func NewMockGit(t *testing.T) *MockGitRunner {
	t.Helper()
	runner := &MockGitRunner{
		responses:        make(map[string]string),
		defaultCommitMsg: "Default commit subject\n\nChange-Id: I0000000000000000000000000000000000000001\n",
	}
	SetMockGit(t, runner)
	return runner
}

// LastCommitMessage returns the message passed via -F or -m to the most recent
// simulated `git commit` invocation, or "" if none was recorded.
func (m *MockGitRunner) LastCommitMessage() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	if len(m.CommittedMessages) == 0 {
		return ""
	}
	return m.CommittedMessages[len(m.CommittedMessages)-1]
}

func (m *MockGitRunner) HasCall(substr string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, call := range m.Calls {
		if strings.Contains(call, substr) {
			return true
		}
	}
	return false
}

// SetMockGit configures DefaultGitRunner for the duration of the test.
func SetMockGit(t *testing.T, runner *MockGitRunner) *MockGitRunner {
	t.Helper()
	if runner == nil {
		runner = &MockGitRunner{}
	}
	oldGit := DefaultGitRunner
	DefaultGitRunner = runner
	t.Cleanup(func() {
		DefaultGitRunner = oldGit
		runner.cleanupMockGitDir()
	})
	return runner
}

type defaultGitRunnerWrapper struct {
	inner GitRunner
}

func (w *defaultGitRunnerWrapper) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	var buf bytes.Buffer
	outWriter := stdout
	if len(args) >= 2 && args[0] == "log" && args[1] == "-1" {
		outWriter = io.MultiWriter(stdout, &buf)
	}
	err := w.inner.Run(ctx, outWriter, stderr, args...)
	if err == nil && len(args) >= 2 && args[0] == "log" && args[1] == "-1" && buf.Len() == 0 {
		stdout.Write([]byte("Default commit subject\n\nChange-Id: I0000000000000000000000000000000000000001\n"))
	}
	return err
}

// MockCWD allows tests using SetupMockConfig to override Config.CWD.
var MockCWD string

// SetupMockConfig wraps gitRunner to provide default commit info on git log -1 and configures RootCmd.PersistentPreRun, restoring everything on test cleanup.
func SetupMockConfig(t *testing.T, gitRunner GitRunner) {
	t.Helper()
	oldGit := DefaultGitRunner
	DefaultGitRunner = &defaultGitRunnerWrapper{inner: gitRunner}
	t.Cleanup(func() {
		DefaultGitRunner = oldGit
		if m, ok := gitRunner.(*MockGitRunner); ok {
			m.cleanupMockGitDir()
		}
	})

	oldPreRunE := RootCmd.PersistentPreRunE
	RootCmd.PersistentPreRunE = func(cmd *cobra.Command, args []string) error {
		if oldPreRunE != nil {
			if err := oldPreRunE(cmd, args); err != nil {
				return err
			}
		}
		if MockCWD != "" {
			if cfg := GetConfig(cmd); cfg != nil {
				cfg.CWD = MockCWD
			}
		}
		return nil
	}
	t.Cleanup(func() {
		RootCmd.PersistentPreRunE = oldPreRunE
		MockCWD = ""
	})
}

// mockTransport allows tests to customize RoundTrip behavior.
type mockTransport struct {
	roundTrip func(*http.Request) (*http.Response, error)
}

func (m *mockTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	return m.roundTrip(req)
}

// SetMockGerritClient overrides NewGerritClient for the duration of the test.
func SetMockGerritClient(t *testing.T, fn func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error)) {
	t.Helper()
	orig := NewGerritClient
	NewGerritClient = fn
	t.Cleanup(func() { NewGerritClient = orig })
}

// SetTestProfile sets ProfileFlag for the duration of the test and restores it on cleanup.
func SetTestProfile(t *testing.T, profile string) {
	t.Helper()
	oldProfile := ProfileFlag
	ProfileFlag = profile
	t.Cleanup(func() { ProfileFlag = oldProfile })
}

// MockRequest records an incoming HTTP request for test assertions.
type MockRequest struct {
	Method string
	URL    *url.URL
	Path   string
	Body   []byte
	Header http.Header
}

// DecodeRequest unmarshals req.Body as JSON into T, failing the test
// immediately if req is nil or JSON decoding fails.
func DecodeRequest[T any](t testing.TB, req *MockRequest) T {
	t.Helper()
	if req == nil {
		t.Fatal("DecodeRequest: expected recorded request, got nil")
	}
	var out T
	if err := json.Unmarshal(req.Body, &out); err != nil {
		t.Fatalf("DecodeRequest(%s %s): failed to unmarshal %T: %v\nBody: %s", req.Method, req.Path, out, err, string(req.Body))
	}
	return out
}

// AuthResolvers groups the pluggable authentication and token resolution hooks
// used across Gerrit, LUCI, Android Build, and Google Issue Tracker. Tests
// install them with SetAuthResolvers or WithHermeticAuth.
type AuthResolvers struct {
	LookPath                 func(file string) (string, error)
	CommandRunner            func(ctx context.Context, name string, args ...string) ([]byte, error)
	GerritToken              func(ctx context.Context) (string, string, error)
	LUCIToken                func(ctx context.Context) (string, string, error)
	AndroidBuildToken        func(ctx context.Context) (string, error)
	IssueTrackerToken        func(ctx context.Context) (string, string, error)
	IssueTrackerQuotaProject func(ctx context.Context, token string) string
}

// currentAuthResolvers returns the active package authentication resolvers.
func currentAuthResolvers() AuthResolvers {
	return AuthResolvers{
		LookPath:                 LookPathFn,
		CommandRunner:            OAuthTokenCommandRunner,
		GerritToken:              GerritTokenResolver,
		LUCIToken:                LUCITokenResolver,
		AndroidBuildToken:        AndroidBuildTokenResolver,
		IssueTrackerToken:        IssueTrackerTokenResolver,
		IssueTrackerQuotaProject: IssueTrackerQuotaProjectResolver,
	}
}

// SetAuthResolvers replaces each active package authentication resolver with
// the corresponding non-nil field of r.
func SetAuthResolvers(r AuthResolvers) {
	if r.LookPath != nil {
		LookPathFn = r.LookPath
	}
	if r.CommandRunner != nil {
		OAuthTokenCommandRunner = r.CommandRunner
	}
	if r.GerritToken != nil {
		GerritTokenResolver = r.GerritToken
	}
	if r.LUCIToken != nil {
		LUCITokenResolver = r.LUCIToken
	}
	if r.AndroidBuildToken != nil {
		AndroidBuildTokenResolver = r.AndroidBuildToken
	}
	if r.IssueTrackerToken != nil {
		IssueTrackerTokenResolver = r.IssueTrackerToken
	}
	if r.IssueTrackerQuotaProject != nil {
		IssueTrackerQuotaProjectResolver = r.IssueTrackerQuotaProject
	}
}

// hermeticAuthEnvVars are the environment variables that supply credentials or
// select an auth mode. Hermetic tests clear them so the developer's
// environment can't leak into a test.
var hermeticAuthEnvVars = []string{
	"GH_ISH_AUTH_MODE",
	// Removed variable; cleared so a stale export doesn't fail every test.
	"GH_ISH_AUTH_METHOD",
	"GERRIT_TOKEN",
	"GHISH_LUCI_TOKEN",
	"LUCI_TOKEN",
	"GHISH_ANDROID_BUILD_TOKEN",
	"GHISH_ISSUE_TOKEN",
	"BUGANIZER_TOKEN",
	"GHISH_QUOTA_PROJECT",
	"GOOGLE_CLOUD_QUOTA_PROJECT",
	"LUCI_AUTH_CREDENTIAL_HELPER",
}

// productionAuthResolvers holds the production resolvers, captured during
// package initialization before TestMain or any test replaces them.
var productionAuthResolvers = currentAuthResolvers()

// failClosedAuthResolvers returns the production resolvers with credential
// tool lookup and token subprocesses made to fail. The production token
// resolvers fail closed because tool lookup fails, while still honoring
// explicit token environment variables.
func failClosedAuthResolvers() AuthResolvers {
	r := productionAuthResolvers
	r.LookPath = func(file string) (string, error) {
		return "", fmt.Errorf("hermetic test: %s not found in PATH", file)
	}
	r.CommandRunner = func(ctx context.Context, name string, args ...string) ([]byte, error) {
		return nil, fmt.Errorf("hermetic test: unexpected command %s", name)
	}
	return r
}

// WithHermeticAuth clears the credential environment variables, resets auth
// caches, and installs fail-closed tool lookup, the production Gerrit and Issue
// Tracker token resolvers, and stub LUCI, Android Build, and quota-project
// resolvers (overridden by non-nil fields in overrides). It restores the
// previous resolvers and clears auth caches on test cleanup.
//
// A test that assigns LookPathFn directly, rather than through this helper,
// must also call ResetAuthTokenCaches.
// hermeticAuthActive reports whether WithHermeticAuth is currently active in
// the running test, so NewMockGerritServer does not clobber resolvers installed
// by an earlier WithHermeticAuth call.
var hermeticAuthActive bool

// WithHermeticAuth resets auth caches and installs fail-closed tool lookup, the
// production Gerrit and Issue Tracker token resolvers, and stub LUCI, Android
// Build, and quota-project resolvers (overridden by non-nil fields in
// overrides). Ambient credential environment variables are cleared at test
// binary startup by TestMain. WithHermeticAuth restores the previous resolvers
// and clears auth caches on test cleanup.
//
// A test that assigns LookPathFn directly, rather than through this helper,
// must also call ResetAuthTokenCaches.
func WithHermeticAuth(t testing.TB, overrides AuthResolvers) {
	t.Helper()
	prev := currentAuthResolvers()
	prevActive := hermeticAuthActive
	hermeticAuthActive = true

	// Start from the production resolvers rather than prev, so a nested call
	// undoes an earlier direct assignment.
	hermetic := failClosedAuthResolvers()
	hermetic.LUCIToken = func(ctx context.Context) (string, string, error) {
		return "mock-luci-token", "test", nil
	}
	hermetic.AndroidBuildToken = func(ctx context.Context) (string, error) {
		return "mock-android-build-token", nil
	}
	hermetic.IssueTrackerQuotaProject = func(context.Context, string) string {
		return "mock-quota-project"
	}
	ResetAuthTokenCaches()
	SetAuthResolvers(hermetic)
	SetAuthResolvers(overrides)
	t.Cleanup(func() {
		SetAuthResolvers(prev)
		hermeticAuthActive = prevActive
		ResetAuthTokenCaches()
	})
}

// MockRoute defines matching criteria and handler for a route.
type MockRoute struct {
	Method  string // empty matches any method
	Path    string // exact path or prefix (if ends in *)
	Handler http.HandlerFunc
}

// MockGerritServer manages an httptest.Server configured to simulate Gerrit REST responses.
type MockGerritServer struct {
	Server         *httptest.Server
	URL            string
	fallbackStatus int
	fallbackBody   []byte
	mu             sync.Mutex
	routes         []MockRoute
	requests       []MockRequest
	t              *testing.T
}

// NewMockGerritServer creates and starts a mock Gerrit server.
// It automatically configures HostFlag and NewGerritClient to point to this server
// and restores them on test cleanup.
func NewMockGerritServer(t *testing.T) *MockGerritServer {
	t.Helper()
	if !hermeticAuthActive {
		WithHermeticAuth(t, AuthResolvers{})
	}
	s := &MockGerritServer{
		t:              t,
		fallbackStatus: http.StatusNotFound,
	}
	s.Server = httptest.NewServer(http.HandlerFunc(s.handleHTTP))
	s.URL = s.Server.URL
	t.Cleanup(s.Server.Close)

	oldHost := HostFlag
	HostFlag = s.URL
	t.Cleanup(func() { HostFlag = oldHost })

	oldBBHost := buildbucketHost
	buildbucketHost = s.URL
	t.Cleanup(func() { buildbucketHost = oldBBHost })

	origClient := NewGerritClient
	NewGerritClient = func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, s.URL, s.Server.Client())
	}
	t.Cleanup(func() { NewGerritClient = origClient })

	origLUCIClient := getLUCIHTTPClient
	getLUCIHTTPClient = func(ctx context.Context, bbHost string) *http.Client {
		return s.Server.Client()
	}
	origBTClient := getBusytownHTTPClient
	getBusytownHTTPClient = func(ctx context.Context) *http.Client {
		return s.Server.Client()
	}
	origBTAPIBase := BusytownAndroidBuildAPIBase
	BusytownAndroidBuildAPIBase = s.URL
	origBTCIBase := BusytownAndroidCIBase
	BusytownAndroidCIBase = s.URL
	origBTExists := BusytownFileExistsFn
	BusytownFileExistsFn = func(string) bool { return false }
	t.Cleanup(func() {
		getLUCIHTTPClient = origLUCIClient
		getBusytownHTTPClient = origBTClient
		BusytownAndroidBuildAPIBase = origBTAPIBase
		BusytownAndroidCIBase = origBTCIBase
		BusytownFileExistsFn = origBTExists
		MockCWD = ""
	})

	return s
}

// Client returns a gerrit.Client pointed to this mock server.
func (s *MockGerritServer) Client() *gerrit.Client {
	c, _ := gerrit.NewClient(context.Background(), s.URL, s.Server.Client())
	return c
}

func (s *MockGerritServer) handleHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	body, _ := io.ReadAll(r.Body)
	s.requests = append(s.requests, MockRequest{
		Method: r.Method,
		URL:    r.URL,
		Path:   r.URL.Path,
		Body:   body,
		Header: r.Header.Clone(),
	})

	var matchedHandler, wildcardHandler http.HandlerFunc
	bestWildcardLen := -1
	reqPath := strings.TrimPrefix(r.URL.Path, "/a")
	for i := len(s.routes) - 1; i >= 0; i-- {
		route := s.routes[i]
		if route.Method != "" && route.Method != r.Method {
			continue
		}
		p := strings.TrimPrefix(route.Path, "/a")
		if !strings.ContainsAny(p, "*?[") {
			if p == reqPath {
				matchedHandler = route.Handler
				break
			}
			continue
		}
		litLen := len(strings.ReplaceAll(p, "*", ""))
		if matched, _ := path.Match(p, reqPath); matched {
			litLen += 1000
		} else if prefix, ok := strings.CutSuffix(p, "*"); !ok || !strings.HasPrefix(reqPath, prefix) {
			continue
		}
		if litLen > bestWildcardLen {
			wildcardHandler = route.Handler
			bestWildcardLen = litLen
		}
	}
	if matchedHandler == nil {
		matchedHandler = wildcardHandler
	}
	fallbackStatus := s.fallbackStatus
	fallbackBody := s.fallbackBody
	s.mu.Unlock()

	if matchedHandler != nil {
		r.Body = io.NopCloser(bytes.NewReader(body))
		matchedHandler(w, r)
		return
	}

	w.WriteHeader(fallbackStatus)
	if len(fallbackBody) > 0 {
		w.Write(fallbackBody)
	}
}

// On registers a custom handler for a method and path.
//
// Route matching strips any leading "/a" prefix and selects routes in the
// following order of precedence:
//  1. Exact path match (later registrations override earlier ones).
//  2. Single-segment glob match via path.Match (later registrations win ties,
//     and longer literal lengths take precedence).
//  3. Multi-segment prefix wildcard ending in a trailing "*" (longer literal
//     prefixes take precedence; "*" in the middle of a path only matches within
//     a single path segment via path.Match).
func (s *MockGerritServer) On(method, path string, handler http.HandlerFunc) *MockGerritServer {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.routes = append(s.routes, MockRoute{
		Method:  method,
		Path:    path,
		Handler: handler,
	})
	return s
}

// RespondJSON writes Gerrit's magic prefix and JSON payload to w.
func (s *MockGerritServer) RespondJSON(w http.ResponseWriter, status int, payload any) {
	var jsonBytes []byte
	switch v := payload.(type) {
	case string:
		jsonBytes = []byte(v)
	case []byte:
		jsonBytes = v
	default:
		var err error
		jsonBytes, err = json.Marshal(payload)
		if err != nil {
			s.t.Fatalf("MockGerritServer.RespondJSON: failed to marshal JSON: %v", err)
		}
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write([]byte(")]}'\n"))
	w.Write(jsonBytes)
}

// OnJSON registers a route returning a JSON payload with Gerrit's mandatory ")]}'\n" prefix.
func (s *MockGerritServer) OnJSON(method, path string, status int, payload any) *MockGerritServer {
	return s.On(method, path, func(w http.ResponseWriter, r *http.Request) {
		s.RespondJSON(w, status, payload)
	})
}

// OnString registers a route returning plain string content with the given content type.
func (s *MockGerritServer) OnString(method, path string, status int, contentType, body string) *MockGerritServer {
	return s.On(method, path, func(w http.ResponseWriter, r *http.Request) {
		if contentType != "" {
			w.Header().Set("Content-Type", contentType)
		}
		w.WriteHeader(status)
		w.Write([]byte(body))
	})
}

// OnStatus sets the default fallback status for unmatched requests.
func (s *MockGerritServer) OnStatus(status int) *MockGerritServer {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.fallbackStatus = status
	return s
}

// CallCount returns the number of times a method and path was requested.
func (s *MockGerritServer) CallCount(method, path string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	count := 0
	for _, req := range s.requests {
		if (method == "" || req.Method == method) && (path == "" || req.Path == path) {
			count++
		}
	}
	return count
}

// Requests returns a copy of all recorded requests.
func (s *MockGerritServer) Requests() []MockRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	copied := make([]MockRequest, len(s.requests))
	copy(copied, s.requests)
	return copied
}

// LastRequest returns the most recent request, or nil if no requests were received.
func (s *MockGerritServer) LastRequest() *MockRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.requests) == 0 {
		return nil
	}
	req := s.requests[len(s.requests)-1]
	return &req
}

// -----------------------------------------------------------------------------
// Declarative Gerrit Fixtures
// -----------------------------------------------------------------------------

// DefaultMockChange returns a map representing a typical Gerrit ChangeInfo.
func DefaultMockChange(number int, opts ...func(map[string]any)) map[string]any {
	ch := map[string]any{
		"id":               fmt.Sprintf("pigweed~main~I%d", number),
		"project":          "pigweed/pigweed",
		"branch":           "main",
		"change_id":        fmt.Sprintf("I%d", number),
		"subject":          fmt.Sprintf("Change %d", number),
		"status":           "NEW",
		"current_revision": "rev1",
		"_number":          number,
		"revisions": map[string]any{
			"rev1": map[string]any{
				"_number": 1,
			},
		},
	}
	for _, opt := range opts {
		opt(ch)
	}
	return ch
}

// WithSubject sets the commit subject on a mock change.
func WithSubject(subject string) func(map[string]any) {
	return func(ch map[string]any) { ch["subject"] = subject }
}

// WithStatus sets the status (NEW, MERGED, ABANDONED) on a mock change.
func WithStatus(status string) func(map[string]any) {
	return func(ch map[string]any) { ch["status"] = status }
}

// WithBranch sets the branch name on a mock change.
func WithBranch(branch string) func(map[string]any) {
	return func(ch map[string]any) { ch["branch"] = branch }
}

// WithProjectName sets the project name on a mock change.
func WithProjectName(project string) func(map[string]any) {
	return func(ch map[string]any) { ch["project"] = project }
}

// WithLabels declares the labels a mock change reports, as Gerrit does for the
// LABELS / DETAILED_LABELS options.
func WithLabels(names ...string) func(map[string]any) {
	return func(ch map[string]any) {
		labels := map[string]any{}
		for _, name := range names {
			labels[name] = map[string]any{}
		}
		ch["labels"] = labels
	}
}

// WithChangeID sets the change_id and id on a mock change.
func WithChangeID(changeID string) func(map[string]any) {
	return func(ch map[string]any) {
		ch["change_id"] = changeID
		ch["id"] = fmt.Sprintf("pigweed~main~%s", changeID)
	}
}

func ensureMockRevision(ch map[string]any, revName string) map[string]any {
	revs, ok := ch["revisions"].(map[string]any)
	if !ok {
		revs = make(map[string]any)
		ch["revisions"] = revs
	}
	revMap, ok := revs[revName].(map[string]any)
	if !ok {
		revMap = map[string]any{}
		revs[revName] = revMap
	}
	return revMap
}

// setCurrentMockRevision updates current_revision on ch and carries forward any
// existing commit metadata from the previous current_revision if the target
// revision does not already define one, keeping WithCommitMessage order-independent.
func setCurrentMockRevision(ch map[string]any, revName string) map[string]any {
	prevRev, _ := ch["current_revision"].(string)
	var prevCommit any
	if prevRev != "" && prevRev != revName {
		if revs, ok := ch["revisions"].(map[string]any); ok {
			if prevMap, ok := revs[prevRev].(map[string]any); ok {
				prevCommit = prevMap["commit"]
			}
		}
	}
	ch["current_revision"] = revName
	revMap := ensureMockRevision(ch, revName)
	if prevCommit != nil {
		if _, ok := revMap["commit"]; !ok {
			revMap["commit"] = prevCommit
		}
	}
	return revMap
}

// WithCurrentPatchset configures current_revision and revisions map.
func WithCurrentPatchset(patchset int) func(map[string]any) {
	return func(ch map[string]any) {
		revName := fmt.Sprintf("rev%d", patchset)
		revMap := setCurrentMockRevision(ch, revName)
		revMap["_number"] = patchset
	}
}

// WithCommitMessage embeds commit.message on the mock change's current_revision.
// It may be passed before or after WithCurrentPatchset / WithPatchsetKind.
func WithCommitMessage(msg string) func(map[string]any) {
	return func(ch map[string]any) {
		revName, _ := ch["current_revision"].(string)
		if revName == "" {
			revName = "rev1"
			ch["current_revision"] = revName
		}
		revMap := ensureMockRevision(ch, revName)
		if _, ok := revMap["_number"]; !ok {
			revMap["_number"] = 1
		}
		revMap["commit"] = map[string]any{"message": msg}
	}
}

// WithPatchsetKind adds or updates rev<patchset> in revisions with _number and
// kind, and updates current_revision if patchset is the highest revision number.
func WithPatchsetKind(patchset int, kind string) func(map[string]any) {
	return func(ch map[string]any) {
		revName := fmt.Sprintf("rev%d", patchset)
		revMap := ensureMockRevision(ch, revName)
		revMap["_number"] = patchset
		revMap["kind"] = kind
		revs := ch["revisions"].(map[string]any)

		maxNum := -1
		maxRev := ""
		for rName, raw := range revs {
			if rMap, ok := raw.(map[string]any); ok {
				if num, ok := rMap["_number"].(int); ok && num > maxNum {
					maxNum = num
					maxRev = rName
				}
			}
		}
		if maxRev != "" {
			setCurrentMockRevision(ch, maxRev)
		}
	}
}

// OnDefaultChange registers GET /changes/<number> and /changes/<number>/detail returning DefaultMockChange.
func (s *MockGerritServer) OnDefaultChange(number any, opts ...func(map[string]any)) *MockGerritServer {
	num := 1
	if v, ok := number.(int); ok {
		num = v
	}
	ch := DefaultMockChange(num, opts...)
	if v, ok := number.(string); ok && strings.HasPrefix(v, "I") && ch["change_id"] == "I1" {
		ch["change_id"] = v
		if ch["id"] == "pigweed~main~I1" {
			ch["id"] = "pigweed~main~" + v
		}
	}
	s.On("GET", "/changes/", func(w http.ResponseWriter, r *http.Request) {
		if qs := r.URL.Query()["q"]; len(qs) > 1 {
			batches := make([][]map[string]any, len(qs))
			batches[0] = []map[string]any{ch}
			s.RespondJSON(w, http.StatusOK, batches)
			return
		}
		s.RespondJSON(w, http.StatusOK, []map[string]any{ch})
	})
	if cID, ok := ch["change_id"].(string); ok && cID != "" {
		s.OnJSON("GET", fmt.Sprintf("/changes/%s", cID), http.StatusOK, ch)
		s.OnJSON("GET", fmt.Sprintf("/changes/%s/detail", cID), http.StatusOK, ch)
		s.OnJSON("GET", fmt.Sprintf("/changes/*~%s", cID), http.StatusOK, ch)
		s.OnJSON("GET", fmt.Sprintf("/changes/*~%s/detail", cID), http.StatusOK, ch)
	}
	if _, ok := number.(int); ok {
		s.OnJSON("GET", fmt.Sprintf("/changes/%d/flows", num), http.StatusOK, []any{})
		s.OnJSON("GET", fmt.Sprintf("/changes/%d/is-flows-enabled", num), http.StatusOK, map[string]any{"enabled": true})
	}
	s.OnJSON("GET", fmt.Sprintf("/changes/%v/detail", number), http.StatusOK, ch)
	return s.OnJSON("GET", fmt.Sprintf("/changes/%v", number), http.StatusOK, ch)
}

// OnFlowsEnabled registers GET /changes/<number>/is-flows-enabled responding
// with status and, for a 2xx status, {"enabled": enabled}.
func (s *MockGerritServer) OnFlowsEnabled(number int, status int, enabled bool) *MockGerritServer {
	return s.On("GET", fmt.Sprintf("/changes/%d/is-flows-enabled", number), func(w http.ResponseWriter, r *http.Request) {
		if status < 200 || status > 299 {
			w.WriteHeader(status)
			return
		}
		s.RespondJSON(w, status, map[string]any{"enabled": enabled})
	})
}

// OnFlows registers GET /changes/<number>/flows returning flows.
func (s *MockGerritServer) OnFlows(number int, flows ...FlowInfo) *MockGerritServer {
	if flows == nil {
		flows = []FlowInfo{}
	}
	return s.OnJSON("GET", fmt.Sprintf("/changes/%d/flows", number), http.StatusOK, flows)
}

// OnComments registers GET /changes/<number>/comments returning comments.
func (s *MockGerritServer) OnComments(number any, comments any) *MockGerritServer {
	if comments == nil {
		comments = map[string][]CommentInfo{}
	}
	return s.OnJSON("GET", fmt.Sprintf("/changes/%v/comments", number), http.StatusOK, comments)
}

// OnDrafts registers GET /changes/<number>/drafts returning drafts.
func (s *MockGerritServer) OnDrafts(number any, drafts any) *MockGerritServer {
	if drafts == nil {
		drafts = map[string][]CommentInfo{}
	}
	return s.OnJSON("GET", fmt.Sprintf("/changes/%v/drafts", number), http.StatusOK, drafts)
}

// OnReview registers POST /changes/<number>/revisions/current/review.
func (s *MockGerritServer) OnReview(number any, payload ...any) *MockGerritServer {
	var resp any = map[string]any{}
	if len(payload) > 0 {
		resp = payload[0]
	}
	return s.OnJSON("POST", fmt.Sprintf("/changes/%v/revisions/current/review", number), http.StatusOK, resp)
}

// OnAccountSelf registers GET /accounts/self returning the specified account ID.
func (s *MockGerritServer) OnAccountSelf(accountID int) *MockGerritServer {
	return s.OnJSON("GET", "/accounts/self", http.StatusOK, map[string]any{
		"_account_id": accountID,
	})
}

// -----------------------------------------------------------------------------
// Declarative LUCI / Buildbucket Fixtures
// -----------------------------------------------------------------------------

// FakeBuild constructs a bbBuild fixture for testing.
func FakeBuild(id, builder, status string, opts ...func(*bbBuild)) bbBuild {
	b := bbBuild{
		ID: id,
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "try",
			Builder: builder,
		},
		Status:    status,
		StartTime: "2026-05-26T20:23:35.000000000Z",
		EndTime:   "2026-05-26T20:24:35.000000000Z",
	}
	for _, opt := range opts {
		opt(&b)
	}
	return b
}

// WithBucket sets the bucket for a fake build.
func WithBucket(bucket string) func(*bbBuild) {
	return func(b *bbBuild) { b.Builder.Bucket = bucket }
}

// WithBuildProject sets the project for a fake build.
func WithBuildProject(project string) func(*bbBuild) {
	return func(b *bbBuild) { b.Builder.Project = project }
}

// WithSummary sets the summary markdown for a fake build.
func WithSummary(summary string) func(*bbBuild) {
	return func(b *bbBuild) { b.SummaryMarkdown = summary }
}

// WithTimes sets the start and end RFC3339 timestamps for a fake build.
func WithTimes(start, end string) func(*bbBuild) {
	return func(b *bbBuild) {
		b.StartTime = start
		b.EndTime = end
	}
}

// WithCritical sets the critical field ("NO" marks non-blocking).
func WithCritical(critical string) func(*bbBuild) {
	return func(b *bbBuild) { b.Critical = critical }
}

// WithExperiments sets experiments in the input struct.
func WithExperiments(experiments ...string) func(*bbBuild) {
	return func(b *bbBuild) {
		b.Input = &bbInput{Experiments: experiments}
	}
}

// WithTags appends tags to the build.
func WithTags(tags ...bbTag) func(*bbBuild) {
	return func(b *bbBuild) {
		b.Tags = append(b.Tags, tags...)
	}
}

// WithCQExperimental sets the cq_experimental tag.
func WithCQExperimental(exp bool) func(*bbBuild) {
	val := "false"
	if exp {
		val = "true"
	}
	return WithTags(bbTag{Key: "cq_experimental", Value: val})
}

// OnSearchBuilds registers a SearchBuilds mock responding with the provided builds.
func (s *MockGerritServer) OnSearchBuilds(builds ...bbBuild) *MockGerritServer {
	if builds == nil {
		builds = []bbBuild{}
	}
	return s.OnJSON("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", http.StatusOK, map[string]any{
		"builds": builds,
	})
}

// OnSearchBuildsSequence registers a SearchBuilds mock that returns each slice
// in polls sequentially across requests (repeating the final slice once
// exhausted). Requests may arrive concurrently, so the poll counter is atomic.
func (s *MockGerritServer) OnSearchBuildsSequence(polls ...[]bbBuild) *MockGerritServer {
	var count atomic.Int64
	return s.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		idx := int(count.Add(1) - 1)
		if idx >= len(polls) {
			idx = len(polls) - 1
		}
		var builds []bbBuild
		if idx >= 0 && len(polls) > 0 {
			builds = polls[idx]
		}
		if builds == nil {
			builds = []bbBuild{}
		}
		s.RespondJSON(w, http.StatusOK, map[string]any{"builds": builds})
	})
}

// OnSearchBuildsByPatchset registers a SearchBuilds mock that dispatches by
// the single requested Gerrit patchset number in the Buildbucket predicate.
// Requests with len(gerritChanges) != 1 or an unlisted patchset fail the test
// with HTTP 400; callers expecting an empty result for a patchset should list
// `ps: nil` explicitly.
func (s *MockGerritServer) OnSearchBuildsByPatchset(byPS map[int][]bbBuild) *MockGerritServer {
	return s.On("POST", "/prpc/buildbucket.v2.Builds/SearchBuilds", func(w http.ResponseWriter, r *http.Request) {
		var req bbSearchBuildsRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			s.t.Errorf("OnSearchBuildsByPatchset: failed to decode request body: %v", err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if len(req.Predicate.GerritChanges) != 1 {
			s.t.Errorf("OnSearchBuildsByPatchset: got %d gerritChanges, want 1", len(req.Predicate.GerritChanges))
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		ps := req.Predicate.GerritChanges[0].Patchset
		builds, ok := byPS[ps]
		if !ok {
			s.t.Errorf("OnSearchBuildsByPatchset: unexpected SearchBuilds call for patchset %d", ps)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if builds == nil {
			builds = []bbBuild{}
		}
		s.RespondJSON(w, http.StatusOK, map[string]any{"builds": builds})
	})
}

// FakeBuildDetails creates a LUCIBuildDetails fixture for testing.
func FakeBuildDetails(id, builder, status string, opts ...func(*LUCIBuildDetails)) *LUCIBuildDetails {
	d := &LUCIBuildDetails{
		ID: id,
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "try",
			Builder: builder,
		},
		Status: status,
	}
	for _, opt := range opts {
		opt(d)
	}
	return d
}

// WithBuildSummary sets the summary markdown on a build details fixture.
func WithBuildSummary(summary string) func(*LUCIBuildDetails) {
	return func(d *LUCIBuildDetails) { d.SummaryMarkdown = summary }
}

// WithSteps sets the steps on a build details fixture.
func WithSteps(steps ...LUCIStep) func(*LUCIBuildDetails) {
	return func(d *LUCIBuildDetails) { d.Steps = append(d.Steps, steps...) }
}

// FakeStep creates a LUCIStep fixture for testing.
func FakeStep(name, status string, opts ...func(*LUCIStep)) LUCIStep {
	s := LUCIStep{
		Name:   name,
		Status: status,
	}
	for _, opt := range opts {
		opt(&s)
	}
	return s
}

// WithStepSummary sets the summary markdown on a step fixture.
func WithStepSummary(summary string) func(*LUCIStep) {
	return func(s *LUCIStep) { s.SummaryMarkdown = summary }
}

// WithStepLogs appends logs to a step fixture.
func WithStepLogs(logs ...LUCILog) func(*LUCIStep) {
	return func(s *LUCIStep) { s.Logs = append(s.Logs, logs...) }
}

// FakeLog creates a LUCILog fixture.
func FakeLog(name, viewURL string) LUCILog {
	return LUCILog{Name: name, ViewURL: viewURL}
}

// OnGetBuild registers a GetBuild mock responding with the provided build details.
// If details is a map (e.g. map[string]*LUCIBuildDetails or map[string]any), it
// responds with the entry matching the request's build ID.
func (s *MockGerritServer) OnGetBuild(details any) *MockGerritServer {
	switch m := details.(type) {
	case map[string]*LUCIBuildDetails:
		return s.On("POST", "/prpc/buildbucket.v2.Builds/GetBuild", func(w http.ResponseWriter, r *http.Request) {
			var req struct {
				ID string `json:"id"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil || m[req.ID] == nil {
				w.WriteHeader(http.StatusNotFound)
				return
			}
			s.RespondJSON(w, http.StatusOK, m[req.ID])
		})
	case map[string]any:
		return s.On("POST", "/prpc/buildbucket.v2.Builds/GetBuild", func(w http.ResponseWriter, r *http.Request) {
			var req struct {
				ID string `json:"id"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil || m[req.ID] == nil {
				w.WriteHeader(http.StatusNotFound)
				return
			}
			s.RespondJSON(w, http.StatusOK, m[req.ID])
		})
	default:
		return s.OnJSON("POST", "/prpc/buildbucket.v2.Builds/GetBuild", http.StatusOK, details)
	}
}

// MockIssueTrackerServer provides an in-memory Google Issue Tracker v1 REST server for testing.
type MockIssueTrackerServer struct {
	Server          *httptest.Server
	mu              sync.Mutex
	Issues          map[int64]*BuganizerIssue
	Comments        map[int64][]BuganizerComment
	NextIssueID     int64
	Calls           []string
	LastQuery       string
	ForceStatus     int
	CommentPageSize int
}

// NewMockIssueTrackerServer creates and starts a new MockIssueTrackerServer.
func NewMockIssueTrackerServer(t *testing.T) *MockIssueTrackerServer {
	t.Helper()
	s := &MockIssueTrackerServer{
		Issues:      make(map[int64]*BuganizerIssue),
		Comments:    make(map[int64][]BuganizerComment),
		NextIssueID: 300001,
	}
	s.Server = httptest.NewServer(http.HandlerFunc(s.serveHTTP))
	t.Cleanup(func() {
		s.Server.Close()
	})
	return s
}

// Client returns an IssueTrackerClient configured to talk to this mock server.
func (s *MockIssueTrackerServer) Client() *IssueTrackerClient {
	c := NewIssueTrackerClient(s.Server.URL+"/v1", s.Server.Client())
	c.TokenProvider = func(context.Context) (string, error) {
		return "test-mock-token", nil
	}
	c.QuotaProjectProvider = func(context.Context, string) string {
		return "mock-quota-project"
	}
	return c
}

// Install hooks NewIssueTrackerClientForCommand to return this mock server's client for the duration of the test.
func (s *MockIssueTrackerServer) Install(t *testing.T) {
	t.Helper()
	old := NewIssueTrackerClientForCommand
	NewIssueTrackerClientForCommand = func(ctx context.Context, cmd *cobra.Command) (*IssueTrackerClient, error) {
		return s.Client(), nil
	}
	t.Cleanup(func() {
		NewIssueTrackerClientForCommand = old
	})
}

// SeedIssue adds an issue and optional initial description comment to the mock server.
func (s *MockIssueTrackerServer) SeedIssue(issue *BuganizerIssue, description string) *BuganizerIssue {
	s.mu.Lock()
	defer s.mu.Unlock()

	if issue.IssueID == 0 {
		issue.IssueID = FlexInt64(s.NextIssueID)
		s.NextIssueID++
	}
	id := int64(issue.IssueID)
	if issue.CreatedTime.IsZero() {
		issue.CreatedTime = time.Date(2026, 9, 15, 12, 0, 0, 0, time.UTC)
	}
	if issue.ModifiedTime.IsZero() {
		issue.ModifiedTime = issue.CreatedTime
	}
	if description != "" {
		c := BuganizerComment{
			CommentNumber: 1,
			Comment:       description,
			Author:        issue.State.Reporter,
			CreatedTime:   issue.CreatedTime,
		}
		s.Comments[id] = []BuganizerComment{c}
		issue.Description = &c
	}
	s.Issues[id] = issue
	return issue
}

func (s *MockIssueTrackerServer) serveHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.Calls = append(s.Calls, fmt.Sprintf("%s %s", r.Method, r.URL.Path))

	if s.ForceStatus != 0 {
		w.WriteHeader(s.ForceStatus)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"error": map[string]any{
				"code":    s.ForceStatus,
				"message": fmt.Sprintf("mock forced HTTP %d", s.ForceStatus),
			},
		})
		return
	}

	if r.Header.Get("Authorization") == "" {
		w.WriteHeader(http.StatusUnauthorized)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"error": map[string]any{
				"code":    http.StatusUnauthorized,
				"message": "missing Authorization Bearer header",
			},
		})
		return
	}

	w.Header().Set("Content-Type", "application/json")

	// POST /v1/issues
	if r.Method == http.MethodPost && r.URL.Path == "/v1/issues" {
		var req CreateIssueRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			w.WriteHeader(http.StatusBadRequest)
			_ = json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
			return
		}
		id := s.NextIssueID
		s.NextIssueID++
		now := time.Now().UTC()
		issue := &BuganizerIssue{
			IssueID:      FlexInt64(id),
			CreatedTime:  now,
			ModifiedTime: now,
			State:        req.IssueState,
		}
		if req.IssueComment != nil && req.IssueComment.Comment != "" {
			c := BuganizerComment{
				CommentNumber:  1,
				Comment:        req.IssueComment.Comment,
				FormattingMode: req.IssueComment.FormattingMode,
				Author:         req.IssueState.Reporter,
				CreatedTime:    now,
			}
			s.Comments[id] = []BuganizerComment{c}
			issue.Description = &c
		}
		s.Issues[id] = issue
		_ = json.NewEncoder(w).Encode(issue)
		return
	}

	// GET /v1/issues
	if r.Method == http.MethodGet && r.URL.Path == "/v1/issues" {
		query := r.URL.Query().Get("query")
		s.LastQuery = query
		var matched []*BuganizerIssue
		for _, id := range slices.Sorted(maps.Keys(s.Issues)) {
			iss := s.Issues[id]
			if !matchesIssueQuery(iss, query) {
				continue
			}
			copyIss := *iss
			if len(s.Comments[id]) > 0 {
				copyIss.Description = &s.Comments[id][0]
			}
			matched = append(matched, &copyIss)
		}
		_ = json.NewEncoder(w).Encode(ListIssuesResponse{Issues: matched})
		return
	}

	// Sub-routes under /v1/issues/{id}...
	if strings.HasPrefix(r.URL.Path, "/v1/issues/") {
		rest := strings.TrimPrefix(r.URL.Path, "/v1/issues/")

		// POST /v1/issues/{id}:modify
		if r.Method == http.MethodPost && strings.HasSuffix(rest, ":modify") {
			idStr := strings.TrimSuffix(rest, ":modify")
			id, err := strconv.ParseInt(idStr, 10, 64)
			if err != nil || s.Issues[id] == nil {
				w.WriteHeader(http.StatusNotFound)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": "issue not found"})
				return
			}
			var req ModifyIssueRequest
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				w.WriteHeader(http.StatusBadRequest)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
				return
			}
			if strings.Contains(req.AddMask, "hotlistIds") || strings.Contains(req.RemoveMask, "hotlistIds") {
				w.WriteHeader(http.StatusBadRequest)
				_ = json.NewEncoder(w).Encode(map[string]any{
					"error": map[string]any{
						"code":    http.StatusBadRequest,
						"message": "hotlist_ids cannot be modified using ModifyIssue",
					},
				})
				return
			}
			issue := s.Issues[id]
			applyIssueModify(issue, &req)
			issue.ModifiedTime = time.Now().UTC()
			if req.IssueComment != nil && req.IssueComment.Comment != "" {
				num := len(s.Comments[id]) + 1
				c := BuganizerComment{
					CommentNumber:  num,
					Comment:        req.IssueComment.Comment,
					FormattingMode: req.IssueComment.FormattingMode,
					CreatedTime:    issue.ModifiedTime,
				}
				s.Comments[id] = append(s.Comments[id], c)
			}
			if len(s.Comments[id]) > 0 {
				issue.Description = &s.Comments[id][0]
			}
			_ = json.NewEncoder(w).Encode(issue)
			return
		}

		// GET or POST /v1/issues/{id}/comments
		if strings.HasSuffix(rest, "/comments") {
			idStr := strings.TrimSuffix(rest, "/comments")
			id, err := strconv.ParseInt(idStr, 10, 64)
			if err != nil || s.Issues[id] == nil {
				w.WriteHeader(http.StatusNotFound)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": "issue not found"})
				return
			}
			if r.Method == http.MethodGet {
				all := s.Comments[id]
				offset := 0
				if tok := r.URL.Query().Get("pageToken"); tok != "" {
					if idx, err := strconv.Atoi(tok); err == nil && idx >= 0 && idx < len(all) {
						offset = idx
					}
				}
				limit := len(all)
				if s.CommentPageSize > 0 {
					limit = s.CommentPageSize
				} else if ps := r.URL.Query().Get("pageSize"); ps != "" {
					if parsedPS, err := strconv.Atoi(ps); err == nil && parsedPS > 0 {
						limit = parsedPS
					}
				}
				end := offset + limit
				var nextTok string
				if end < len(all) {
					nextTok = strconv.Itoa(end)
				} else {
					end = len(all)
				}
				_ = json.NewEncoder(w).Encode(ListIssueCommentsResponse{
					IssueComments: all[offset:end],
					NextPageToken: nextTok,
				})
				return
			}
			if r.Method == http.MethodPost {
				var req BuganizerComment
				if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
					w.WriteHeader(http.StatusBadRequest)
					_ = json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
					return
				}
				req.CommentNumber = len(s.Comments[id]) + 1
				req.CreatedTime = time.Now().UTC()
				s.Comments[id] = append(s.Comments[id], req)
				_ = json.NewEncoder(w).Encode(req)
				return
			}
		}

		// GET /v1/issues/{id}
		if r.Method == http.MethodGet {
			id, err := strconv.ParseInt(rest, 10, 64)
			if err != nil || s.Issues[id] == nil {
				w.WriteHeader(http.StatusNotFound)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": "issue not found"})
				return
			}
			issue := *s.Issues[id]
			if len(s.Comments[id]) > 0 {
				issue.Description = &s.Comments[id][0]
			}
			_ = json.NewEncoder(w).Encode(issue)
			return
		}
	}

	// Sub-routes under /v1/hotlists/{hotlistID}/entries...
	if strings.HasPrefix(r.URL.Path, "/v1/hotlists/") {
		rest := strings.TrimPrefix(r.URL.Path, "/v1/hotlists/")
		if (r.Method == http.MethodPost || r.Method == http.MethodPut) && strings.HasSuffix(rest, "/entries") {
			hidStr := strings.TrimSuffix(rest, "/entries")
			hid, err := strconv.ParseInt(hidStr, 10, 64)
			if err != nil || hid <= 0 {
				w.WriteHeader(http.StatusBadRequest)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": "invalid hotlist ID"})
				return
			}
			var payload struct {
				HotlistEntry struct {
					IssueID FlexInt64 `json:"issueId"`
				} `json:"hotlistEntry"`
			}
			if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
				w.WriteHeader(http.StatusBadRequest)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
				return
			}
			iss := s.Issues[int64(payload.HotlistEntry.IssueID)]
			if iss == nil {
				w.WriteHeader(http.StatusNotFound)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": "issue not found"})
				return
			}
			already := false
			for _, existing := range iss.State.HotlistIDs {
				if int64(existing) == hid {
					already = true
					break
				}
			}
			if !already {
				iss.State.HotlistIDs = append(iss.State.HotlistIDs, FlexInt64(hid))
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"issueId": payload.HotlistEntry.IssueID})
			return
		}
		if r.Method == http.MethodDelete && strings.Contains(rest, "/entries/") {
			parts := strings.SplitN(rest, "/entries/", 2)
			hid, err1 := strconv.ParseInt(parts[0], 10, 64)
			iid, err2 := strconv.ParseInt(parts[1], 10, 64)
			if err1 != nil || err2 != nil || s.Issues[iid] == nil {
				w.WriteHeader(http.StatusNotFound)
				_ = json.NewEncoder(w).Encode(map[string]string{"error": "hotlist entry not found"})
				return
			}
			iss := s.Issues[iid]
			var kept []FlexInt64
			for _, existing := range iss.State.HotlistIDs {
				if int64(existing) != hid {
					kept = append(kept, existing)
				}
			}
			iss.State.HotlistIDs = kept
			_ = json.NewEncoder(w).Encode(map[string]any{})
			return
		}
	}

	w.WriteHeader(http.StatusNotFound)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": "unhandled mock route: " + r.URL.Path})
}

func matchesSingleIssueQueryToken(iss *BuganizerIssue, isOpen bool, tok string) bool {
	switch {
	case tok == "status:open":
		return isOpen
	case tok == "status:closed":
		return !isOpen
	case strings.HasPrefix(tok, "assignee:"):
		want := strings.TrimPrefix(tok, "assignee:")
		return iss.State.Assignee != nil && strings.EqualFold(iss.State.Assignee.EmailAddress, want)
	case strings.HasPrefix(tok, "reporter:"):
		want := strings.TrimPrefix(tok, "reporter:")
		return iss.State.Reporter != nil && strings.EqualFold(iss.State.Reporter.EmailAddress, want)
	case strings.HasPrefix(tok, "priority:"):
		want := strings.TrimPrefix(tok, "priority:")
		return strings.EqualFold(iss.State.Priority, want)
	case strings.HasPrefix(tok, "severity:"):
		want := strings.TrimPrefix(tok, "severity:")
		return strings.EqualFold(iss.State.Severity, want)
	case strings.HasPrefix(tok, "type:"):
		want := strings.TrimPrefix(tok, "type:")
		return strings.EqualFold(iss.State.Type, want)
	case strings.HasPrefix(tok, "componentid:"):
		// The mock has no component hierarchy, so "N+" (N and its
		// descendants) matches only N.
		want := strings.TrimSuffix(strings.TrimPrefix(tok, "componentid:"), "+")
		return fmt.Sprintf("%d", iss.State.ComponentID) == want
	case strings.HasPrefix(tok, "hotlistid:"):
		want := strings.TrimPrefix(tok, "hotlistid:")
		for _, hid := range iss.State.HotlistIDs {
			if fmt.Sprintf("%d", hid) == want {
				return true
			}
		}
		return false
	default:
		clean := strings.Trim(tok, "\"")
		return strings.Contains(strings.ToLower(iss.State.Title), strings.ToLower(clean))
	}
}

func matchesIssueQuery(iss *BuganizerIssue, query string) bool {
	if query == "" {
		return true
	}
	isOpen := iss.State.Status == "NEW" || iss.State.Status == "ASSIGNED" || iss.State.Status == "ACCEPTED"
	tokens := strings.Fields(query)
	for _, tok := range tokens {
		negated := false
		if strings.HasPrefix(tok, "-") && len(tok) > 1 {
			negated = true
			tok = tok[1:]
		}
		matched := matchesSingleIssueQueryToken(iss, isOpen, tok)
		if negated && matched {
			return false
		}
		if !negated && !matched {
			return false
		}
	}
	return true
}

func applyIssueModify(issue *BuganizerIssue, req *ModifyIssueRequest) {
	if req.Add != nil && req.AddMask != "" {
		for _, field := range strings.Split(req.AddMask, ",") {
			switch strings.TrimSpace(field) {
			case "title":
				issue.State.Title = req.Add.Title
			case "status":
				issue.State.Status = req.Add.Status
			case "priority":
				issue.State.Priority = req.Add.Priority
			case "severity":
				issue.State.Severity = req.Add.Severity
			case "type":
				issue.State.Type = req.Add.Type
			case "componentId":
				issue.State.ComponentID = req.Add.ComponentID
			case "assignee":
				issue.State.Assignee = req.Add.Assignee
			case "canonicalIssueId":
				issue.State.CanonicalIssueID = req.Add.CanonicalIssueID
			case "ccs":
				issue.State.CCs = append(issue.State.CCs, req.Add.CCs...)
			}
		}
	}
	if req.Remove != nil && req.RemoveMask != "" {
		for _, field := range strings.Split(req.RemoveMask, ",") {
			switch strings.TrimSpace(field) {
			case "assignee":
				issue.State.Assignee = nil
			case "ccs":
				var kept []BuganizerUser
				for _, existing := range issue.State.CCs {
					remove := false
					for _, rem := range req.Remove.CCs {
						if strings.EqualFold(existing.EmailAddress, rem.EmailAddress) {
							remove = true
							break
						}
					}
					if !remove {
						kept = append(kept, existing)
					}
				}
				issue.State.CCs = kept
			}
		}
	}
}

func TestMockGerritServer_HelpersAndRoutePrecedence(t *testing.T) {
	t.Run("route precedence and /a prefix stripping", func(t *testing.T) {
		s := NewMockGerritServer(t)
		s.OnString("GET", "/changes/*", http.StatusOK, "text/plain", "short-prefix")
		s.OnString("GET", "/changes/100/revisions/*", http.StatusOK, "text/plain", "longer-prefix")
		s.OnString("GET", "/changes/*/detail", http.StatusOK, "text/plain", "glob-early")
		s.OnString("GET", "/changes/*/detail", http.StatusOK, "text/plain", "glob-late")
		s.OnString("GET", "/changes/100/detail", http.StatusOK, "text/plain", "exact-early")
		s.OnString("GET", "/a/changes/100/detail", http.StatusOK, "text/plain", "exact-late")
		s.OnString("GET", "/changes/*", http.StatusOK, "text/plain", "trailing-wildcard-after-exact")

		getBody := func(reqPath string) string {
			t.Helper()
			resp, err := s.Server.Client().Get(s.URL + reqPath)
			if err != nil {
				t.Fatalf("GET %s error: %v", reqPath, err)
			}
			defer resp.Body.Close()
			b, err := io.ReadAll(resp.Body)
			if err != nil {
				t.Fatalf("ReadAll(%s) error: %v", reqPath, err)
			}
			return string(b)
		}

		// Exact match beats both earlier and later wildcard routes, and later
		// exact registration overrides earlier exact registration across /a.
		if got := getBody("/changes/100/detail"); got != "exact-late" {
			t.Errorf("GET /changes/100/detail = %q, want exact-late", got)
		}
		if got := getBody("/a/changes/100/detail"); got != "exact-late" {
			t.Errorf("GET /a/changes/100/detail = %q, want exact-late", got)
		}
		// Single-segment path.Match glob beats multi-segment trailing-* prefix,
		// and later identical glob registration wins (reverse-order scan).
		if got := getBody("/changes/200/detail"); got != "glob-late" {
			t.Errorf("GET /changes/200/detail = %q, want glob-late", got)
		}
		// Longer literal prefix beats shorter prefix.
		if got := getBody("/changes/100/revisions/rev1/files"); got != "longer-prefix" {
			t.Errorf("GET /changes/100/revisions/rev1/files = %q, want longer-prefix", got)
		}
	})

	t.Run("WithPatchsetKind and WithCommitMessage order independence", func(t *testing.T) {
		ch := DefaultMockChange(42,
			WithCommitMessage("Subject\n\nChange-Id: I42"),
			WithPatchsetKind(1, "REWORK"),
			WithPatchsetKind(3, "TRIVIAL_REBASE"),
			WithPatchsetKind(2, "NO_CODE_CHANGE"),
		)
		if got := ch["current_revision"]; got != "rev3" {
			t.Errorf("current_revision = %v, want rev3", got)
		}
		revs := ch["revisions"].(map[string]any)
		rev3 := revs["rev3"].(map[string]any)
		if got := rev3["kind"]; got != "TRIVIAL_REBASE" {
			t.Errorf("rev3.kind = %v, want TRIVIAL_REBASE", got)
		}
		commitMap, ok := rev3["commit"].(map[string]any)
		if !ok || commitMap["message"] != "Subject\n\nChange-Id: I42" {
			t.Errorf("rev3.commit = %#v, want preserved commit message", rev3["commit"])
		}
	})

	t.Run("OnSearchBuildsSequence", func(t *testing.T) {
		s := NewMockGerritServer(t)
		s.OnSearchBuildsSequence(
			[]bbBuild{FakeBuild("1", "b1", "STARTED")},
			[]bbBuild{FakeBuild("1", "b1", "SUCCESS")},
		)
		for i, wantStatus := range []string{"STARTED", "SUCCESS", "SUCCESS"} {
			resp, err := s.Server.Client().Post(s.URL+"/prpc/buildbucket.v2.Builds/SearchBuilds", "application/json", strings.NewReader(`{}`))
			if err != nil {
				t.Fatalf("poll %d error: %v", i, err)
			}
			raw, _ := io.ReadAll(resp.Body)
			resp.Body.Close()
			var decoded bbSearchBuildsResponse
			if err := json.Unmarshal(stripXSSIPrefix(raw), &decoded); err != nil {
				t.Fatalf("poll %d decode error: %v", i, err)
			}
			if len(decoded.Builds) != 1 || decoded.Builds[0].Status != wantStatus {
				t.Errorf("poll %d builds = %+v, want status %q", i, decoded.Builds, wantStatus)
			}
		}
	})

	t.Run("OnSearchBuildsByPatchset", func(t *testing.T) {
		s := NewMockGerritServer(t)
		s.OnSearchBuildsByPatchset(map[int][]bbBuild{
			2: {FakeBuild("20", "linux", "SUCCESS")},
			3: nil,
		})

		postSearch := func(body string) (int, []bbBuild) {
			t.Helper()
			resp, err := s.Server.Client().Post(s.URL+"/prpc/buildbucket.v2.Builds/SearchBuilds", "application/json", strings.NewReader(body))
			if err != nil {
				t.Fatalf("Post error: %v", err)
			}
			defer resp.Body.Close()
			raw, _ := io.ReadAll(resp.Body)
			if resp.StatusCode != http.StatusOK {
				return resp.StatusCode, nil
			}
			var decoded bbSearchBuildsResponse
			if err := json.Unmarshal(stripXSSIPrefix(raw), &decoded); err != nil {
				t.Fatalf("decode error: %v", err)
			}
			return resp.StatusCode, decoded.Builds
		}

		status, builds := postSearch(`{"predicate":{"gerritChanges":[{"patchset":2}]}}`)
		if status != http.StatusOK || len(builds) != 1 || builds[0].ID != "20" {
			t.Errorf("patchset 2 = (%d, %+v), want 200 with build 20", status, builds)
		}
		status, builds = postSearch(`{"predicate":{"gerritChanges":[{"patchset":3}]}}`)
		if status != http.StatusOK || builds == nil || len(builds) != 0 {
			t.Errorf("listed nil patchset 3 = (%d, %#v), want 200 with empty non-nil slice", status, builds)
		}

		for _, badBody := range []string{
			`{malformed-json`,
			`{"predicate":{}}`,
			`{"predicate":{"gerritChanges":[{"patchset":99}]}}`,
		} {
			origT := s.t
			fakeT := &testing.T{}
			s.t = fakeT
			status, _ = postSearch(badBody)
			s.t = origT
			if status != http.StatusBadRequest || !fakeT.Failed() {
				t.Errorf("postSearch(%s) = (status %d, failed=%v), want (400, true)", badBody, status, fakeT.Failed())
			}
		}
	})
}
