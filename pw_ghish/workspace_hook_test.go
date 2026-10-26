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
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

type mockWorkspaceIntegration struct {
	enabled       bool
	pathIssues    map[string]int64
	issueStatuses map[int64]WorkspaceIssueStatus
	developedIDs  []int64
	senseResult   *WorkspaceSenseResult
}

func (m *mockWorkspaceIntegration) IsEnabled() bool {
	return m.enabled
}

func (m *mockWorkspaceIntegration) ResolveIssueIDForPath(cwd string) (int64, bool) {
	if id, ok := m.pathIssues[cwd]; ok && id > 0 {
		return id, true
	}
	return 0, false
}

func (m *mockWorkspaceIntegration) FindWorkspaceForIssue(issueID int64) (WorkspaceIssueStatus, bool) {
	if st, ok := m.issueStatuses[issueID]; ok {
		return st, true
	}
	return WorkspaceIssueStatus{}, false
}

func (m *mockWorkspaceIntegration) DevelopIssueInWorktree(ctx context.Context, issueID int64, title string, customBranch string) (string, string, error) {
	m.developedIDs = append(m.developedIDs, issueID)
	branch := customBranch
	if branch == "" {
		branch = SlugifyBranchName(issueID, title)
	}
	return "/mock/projects/" + branch, "pw-02", nil
}

func (m *mockWorkspaceIntegration) SenseWorktrees(ctx context.Context, cwd string, targetIssueID int64, targetCLNumber int, targetChangeID string, includeFleet bool) (*WorkspaceSenseResult, error) {
	if m.senseResult != nil {
		return m.senseResult, nil
	}
	res := &WorkspaceSenseResult{
		Enabled:        m.enabled,
		ReadyToLand:    []WorkspaceProjectMatch{},
		NeedsAttention: []WorkspaceProjectMatch{},
		MergedProjects: []WorkspaceProjectMatch{},
	}
	if targetIssueID > 0 {
		if st, ok := m.issueStatuses[targetIssueID]; ok {
			res.TargetMatch = &WorkspaceProjectMatch{
				Project:     st.ProjectName,
				Residency:   st.Residency,
				Slot:        st.Slot,
				Branch:      st.Branch,
				IssueID:     targetIssueID,
				SymlinkPath: st.SymlinkPath,
				ProjectUUID: "mock-uuid-" + st.ProjectName,
			}
		}
	}
	return res, nil
}

func TestExtractIssueIDFromBranchName(t *testing.T) {
	cases := []struct {
		branch string
		wantID int64
		wantOK bool
	}{
		{branch: "b-315378787-fix-rpc-framing", wantID: 315378787, wantOK: true},
		{branch: "b-12345-short-slug", wantID: 12345, wantOK: true},
		{branch: "issue-315378787", wantID: 315378787, wantOK: true},
		{branch: "issue/315378787-test", wantID: 315378787, wantOK: true},
		{branch: "b/315378787", wantID: 315378787, wantOK: true},
		{branch: "315378787-fix-rpc", wantID: 315378787, wantOK: true},
		{branch: "main", wantID: 0, wantOK: false},
		{branch: "wt-manager", wantID: 0, wantOK: false},
		{branch: "feature-v2", wantID: 0, wantOK: false},
		{branch: "123-too-short-without-prefix", wantID: 0, wantOK: false},
	}

	for _, tc := range cases {
		gotID, gotOK := ExtractIssueIDFromBranchName(tc.branch)
		if gotOK != tc.wantOK || gotID != tc.wantID {
			t.Errorf("ExtractIssueIDFromBranchName(%q) = (%d, %v), want (%d, %v)",
				tc.branch, gotID, gotOK, tc.wantID, tc.wantOK)
		}
	}
}

func TestResolveTargetIssueID_ZeroCommitGap_BranchFallback(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status: "ASSIGNED",
			Title:  "pw_rpc: Fix channel packet framing",
		},
	}, "Description of 315378787.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	// Simulate fresh branch where HEAD has NO Bug: trailer yet (or is still at origin/main)
	gitRunner.OnCommand("log -1 --format=%B HEAD", "Initial commit on main\n")
	gitRunner.OnCommand("branch --show-current", "b-315378787-fix-rpc-framing\n")
	gitRunner.OnCommand("rev-parse --abbrev-ref HEAD", "b-315378787-fix-rpc-framing\n")

	out, err := executeCommand(RootCmd, "issue", "view")
	if err != nil {
		t.Fatalf("expected 'gh issue view' to resolve 315378787 from branch name when HEAD has no trailer, got error: %v", err)
	}
	if !strings.Contains(out, "#315378787") {
		t.Errorf("expected output for issue #315378787, got:\n%s", out)
	}
}

func TestResolveTargetIssueID_ZeroCommitGap_WorktreeHookFallback(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status: "ASSIGNED",
			Title:  "pw_rpc: Fix channel packet framing",
		},
	}, "Description of 315378787.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	MockCWD = "/home/user/wrk/projects/custom-proj"

	mockWT := &mockWorkspaceIntegration{
		enabled: true,
		pathIssues: map[string]int64{
			"/home/user/wrk/projects/custom-proj": 315378787,
		},
	}
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = mockWT
	defer func() { RegisteredWorkspaceIntegration = prevWT }()

	// Neither HEAD commit nor branch name contains the bug ID
	gitRunner.OnCommand("log -1 --format=%B HEAD", "Initial commit\n")
	gitRunner.OnCommand("branch --show-current", "custom-proj\n")
	gitRunner.OnCommand("rev-parse --abbrev-ref HEAD", "custom-proj\n")

	out, err := executeCommand(RootCmd, "issue", "view")
	if err != nil {
		t.Fatalf("expected 'gh issue view' to resolve 315378787 via WorkspaceIntegration hook, got error: %v", err)
	}
	if !strings.Contains(out, "#315378787") {
		t.Errorf("expected output for issue #315378787, got:\n%s", out)
	}
}

func TestIssueDevelop_WithWorktreeFlag_InvokesHook(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status: "ASSIGNED",
			Title:  "pw_rpc: Fix channel packet framing",
		},
	}, "Description.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)

	mockWT := &mockWorkspaceIntegration{enabled: true}
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = mockWT
	defer func() { RegisteredWorkspaceIntegration = prevWT }()

	out, err := executeCommand(RootCmd, "issue", "develop", "315378787", "--worktree")
	if err != nil {
		t.Fatalf("issue develop --worktree failed: %v\nOutput: %s", err, out)
	}
	if len(mockWT.developedIDs) != 1 || mockWT.developedIDs[0] != 315378787 {
		t.Errorf("expected DevelopIssueInWorktree to be called with 315378787, got %v", mockWT.developedIDs)
	}
	if !strings.Contains(out, "pw-02") || !strings.Contains(out, "/mock/projects/b-315378787") {
		t.Errorf("expected output to mention slot pw-02 and worktree path, got:\n%s", out)
	}
}

func TestIssueStatus_AnnotatesWorktreeResidency(t *testing.T) {
	srv := NewMockIssueTrackerServer(t)
	srv.Install(t)
	srv.SeedIssue(&BuganizerIssue{
		IssueID: 315378787,
		State: BuganizerState{
			Status:   "ASSIGNED",
			Priority: "P1",
			Title:    "pw_rpc: Fix channel packet framing",
			Assignee: &BuganizerUser{EmailAddress: "dev@google.com"},
		},
	}, "Description.")

	gitRunner := &MockGitRunner{}
	SetupMockConfig(t, gitRunner)
	gitRunner.OnCommand("config --get user.email", "dev@google.com\n")

	mockWT := &mockWorkspaceIntegration{
		enabled: true,
		issueStatuses: map[int64]WorkspaceIssueStatus{
			315378787: {
				ProjectName: "b-315378787-fix-rpc",
				Residency:   "MOUNTED",
				Slot:        "pw-02",
				SymlinkPath: "/home/dev/wrk/projects/b-315378787-fix-rpc",
				Branch:      "b-315378787-fix-rpc",
			},
		},
	}
	prevWT := RegisteredWorkspaceIntegration
	RegisteredWorkspaceIntegration = mockWT
	defer func() { RegisteredWorkspaceIntegration = prevWT }()

	out, err := executeCommand(RootCmd, "issue", "status")
	if err != nil {
		t.Fatalf("issue status failed: %v", err)
	}
	if !strings.Contains(out, "MOUNTED") || !strings.Contains(out, "pw-02") {
		t.Errorf("expected issue status to annotate issue #315378787 with MOUNTED (pw-02), got:\n%s", out)
	}
}

func locateRepoRootScript(t *testing.T, name string) string {
	t.Helper()
	candidates := []string{
		filepath.Join("..", name),
	}
	if realTestFile, err := filepath.EvalSymlinks("workspace_hook_test.go"); err == nil {
		candidates = append(candidates, filepath.Join(filepath.Dir(filepath.Dir(realTestFile)), name))
	}
	for _, c := range candidates {
		if st, err := os.Stat(c); err == nil && !st.IsDir() {
			abs, err := filepath.Abs(c)
			if err == nil {
				return abs
			}
			return c
		}
	}
	t.Fatalf("could not locate repository root script %q (tried %v)", name, candidates)
	return ""
}

func setupFakeGhWrapperRepo(t *testing.T) (repoDir string, hashFile string, binDir string, readGitCalls func() []string) {
	t.Helper()
	realGit, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git binary not found in PATH")
	}

	repoDir = t.TempDir()
	src := locateRepoRootScript(t, "gh")
	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatalf("ReadFile(%s): %v", src, err)
	}
	dst := filepath.Join(repoDir, "gh")
	if err := os.WriteFile(dst, data, 0o755); err != nil {
		t.Fatalf("WriteFile(%s): %v", dst, err)
	}

	dirs := []string{
		filepath.Join(repoDir, "pw_ghish"),
		filepath.Join(repoDir, "out", "gh"),
	}
	for _, d := range dirs {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatalf("MkdirAll(%s): %v", d, err)
		}
	}

	files := map[string]string{
		filepath.Join(repoDir, "MODULE.bazel"):            "module()\n",
		filepath.Join(repoDir, "MODULE.bazel.lock"):       "{}\n",
		filepath.Join(repoDir, "pw_ghish", "BUILD.bazel"): "# build\n",
		filepath.Join(repoDir, "pw_ghish", "README.md"):   "# docs\n",
		filepath.Join(repoDir, "pw_ghish", "docs.rst"):    "Docs\n====\n",
		filepath.Join(repoDir, "pw_ghish", "root.go"):     "package pw_ghish\n",
	}
	for path, content := range files {
		if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
			t.Fatalf("WriteFile(%s): %v", path, err)
		}
	}

	runGit := func(args ...string) string {
		t.Helper()
		cmd := exec.Command(realGit, args...)
		cmd.Dir = repoDir
		cmd.Env = append(os.Environ(),
			"GIT_CONFIG_NOSYSTEM=1",
			"GIT_AUTHOR_NAME=Test",
			"GIT_AUTHOR_EMAIL=test@example.com",
			"GIT_COMMITTER_NAME=Test",
			"GIT_COMMITTER_EMAIL=test@example.com",
		)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("git %v failed: %v\n%s", args, err, string(out))
		}
		return strings.TrimSpace(string(out))
	}

	runGit("init", "-q", "--initial-branch=main")
	runGit("config", "commit.gpgsign", "false")
	runGit("add", ".")
	runGit("commit", "-q", "-m", "initial")

	treeHash := runGit("rev-parse", "HEAD:pw_ghish")
	lockHash := runGit("rev-parse", "HEAD:MODULE.bazel.lock")

	cachedBin := filepath.Join(repoDir, "out", "gh", "gh-ish")
	cachedScript := "#!/bin/sh\necho \"CACHED_GH_ISH invoked_as=${GH_ISH_INVOKED_AS:-} args=$*\"\n"
	if err := os.WriteFile(cachedBin, []byte(cachedScript), 0o755); err != nil {
		t.Fatalf("WriteFile(%s): %v", cachedBin, err)
	}

	hashFile = filepath.Join(repoDir, "out", "gh", "git_hash")
	if err := os.WriteFile(hashFile, []byte(treeHash+":"+lockHash+":\n"), 0o644); err != nil {
		t.Fatalf("WriteFile(%s): %v", hashFile, err)
	}

	binDir = t.TempDir()
	gitLogPath := filepath.Join(t.TempDir(), "git.log")
	gitWrapper := "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + gitLogPath + "'\nexec '" + realGit + "' \"$@\"\n"
	if err := os.WriteFile(filepath.Join(binDir, "git"), []byte(gitWrapper), 0o755); err != nil {
		t.Fatalf("WriteFile(git wrapper): %v", err)
	}
	poisonBazelisk := "#!/bin/sh\necho \"UNEXPECTED_BAZELISK_SUBPROCESS\" >&2\nexit 1\n"
	if err := os.WriteFile(filepath.Join(binDir, "bazelisk"), []byte(poisonBazelisk), 0o755); err != nil {
		t.Fatalf("WriteFile(poison bazelisk): %v", err)
	}

	readGitCalls = func() []string {
		t.Helper()
		data, err := os.ReadFile(gitLogPath)
		if err != nil && !os.IsNotExist(err) {
			t.Fatalf("ReadFile(%s): %v", gitLogPath, err)
		}
		_ = os.Remove(gitLogPath)
		var calls []string
		for _, line := range strings.Split(strings.TrimSpace(string(data)), "\n") {
			if line != "" {
				calls = append(calls, line)
			}
		}
		return calls
	}

	return repoDir, hashFile, binDir, readGitCalls
}

func TestGhWrapper_WarmFastPathAndRebuildDetection(t *testing.T) {
	repoDir, hashFile, binDir, readGitCalls := setupFakeGhWrapperRepo(t)

	baseEnv := append(os.Environ(),
		"PATH="+binDir+":"+os.Getenv("PATH"),
		"PW_GH_FORCE_BUILD=",
		"PW_GH_USE_CIPD=",
		"GH_ISH_INVOKED_AS=",
	)

	// 1. Warm fast-path: spawns at most 2 fast read-only git calls (rev-parse + diff-index) and 0 bazelisk calls.
	cmd := exec.Command(filepath.Join(repoDir, "gh"), "--help")
	cmd.Dir = repoDir
	cmd.Env = baseEnv
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("./gh --help on warm fast-path failed: %v\nOutput:\n%s", err, string(out))
	}
	if !strings.Contains(string(out), "CACHED_GH_ISH invoked_as= args=--help") {
		t.Errorf("unexpected warm fast-path output: %s", string(out))
	}
	if calls := readGitCalls(); len(calls) > 2 {
		t.Errorf("warm fast-path spawned %d git calls, want <= 2: %v", len(calls), calls)
	}

	// 2. Modifying .md or .rst files in pw_ghish/ must NOT trigger a rebuild.
	for _, docFile := range []string{"README.md", "docs.rst"} {
		docPath := filepath.Join(repoDir, "pw_ghish", docFile)
		if err := os.WriteFile(docPath, []byte("updated documentation\n"), 0o644); err != nil {
			t.Fatalf("WriteFile(%s): %v", docPath, err)
		}
	}
	cmdDoc := exec.Command(filepath.Join(repoDir, "gh"), "--version")
	cmdDoc.Dir = repoDir
	cmdDoc.Env = baseEnv
	outDoc, err := cmdDoc.CombinedOutput()
	if err != nil {
		t.Fatalf("./gh --version after modifying .md/.rst failed: %v\nOutput:\n%s", err, string(outDoc))
	}
	if !strings.Contains(string(outDoc), "CACHED_GH_ISH invoked_as= args=--version") {
		t.Errorf("expected cached binary when only .md/.rst changed, got: %s", string(outDoc))
	}
	if calls := readGitCalls(); len(calls) > 2 {
		t.Errorf("warm fast-path with dirty .md/.rst spawned %d git calls, want <= 2: %v", len(calls), calls)
	}

	// 3. Modifying a tracked .go file in pw_ghish/ MUST trigger a rebuild.
	goFile := filepath.Join(repoDir, "pw_ghish", "root.go")
	if err := os.WriteFile(goFile, []byte("package pw_ghish\n// modified\n"), 0o644); err != nil {
		t.Fatalf("WriteFile(%s): %v", goFile, err)
	}

	fakeBazelisk := `#!/bin/sh
mkdir -p bazel-bin/pw_ghish/gh-ish_
cat > bazel-bin/pw_ghish/gh-ish_/gh-ish << 'EOF'
#!/bin/sh
echo "REBUILT_GH_ISH args=$*"
EOF
chmod +x bazel-bin/pw_ghish/gh-ish_/gh-ish
`
	if err := os.WriteFile(filepath.Join(binDir, "bazelisk"), []byte(fakeBazelisk), 0o755); err != nil {
		t.Fatalf("WriteFile(fake bazelisk): %v", err)
	}

	cmdRebuild := exec.Command(filepath.Join(repoDir, "gh"), "pr", "status")
	cmdRebuild.Dir = repoDir
	cmdRebuild.Env = baseEnv
	outRebuild, err := cmdRebuild.CombinedOutput()
	if err != nil {
		t.Fatalf("./gh after modifying .go file failed: %v\nOutput:\n%s", err, string(outRebuild))
	}
	if !strings.Contains(string(outRebuild), "REBUILT_GH_ISH args=pr status") {
		t.Errorf("expected rebuild when tracked .go file is modified, got: %s", string(outRebuild))
	}
	if _, err := os.Stat(hashFile); !os.IsNotExist(err) {
		t.Errorf("expected git_hash to be removed after dirty rebuild, got err=%v", err)
	}
}

// The binary passes the wrapper's environment to every subprocess, including
// git hooks run by `./gh pr create`. Pigweed's environment parser rejects
// unrecognized PW_ variables, so the wrapper must not export any.
func TestGhWrapper_ExportsNoPwVariables(t *testing.T) {
	repoDir, _, binDir, _ := setupFakeGhWrapperRepo(t)

	// Replace the cached binary with one that prints its environment.
	cachedBin := filepath.Join(repoDir, "out", "gh", "gh-ish")
	if err := os.WriteFile(cachedBin, []byte("#!/bin/sh\nenv\n"), 0o755); err != nil {
		t.Fatalf("WriteFile(%s): %v", cachedBin, err)
	}

	// Start without PW_ variables so that any in the output came from the
	// wrapper.
	var env []string
	for _, kv := range os.Environ() {
		if !strings.HasPrefix(kv, "PW_") {
			env = append(env, kv)
		}
	}
	env = append(env, "PATH="+binDir+":"+os.Getenv("PATH"))

	cmd := exec.Command(filepath.Join(repoDir, "gh"), "pr", "status")
	cmd.Dir = repoDir
	cmd.Env = env
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("./gh pr status failed: %v\nOutput:\n%s", err, string(out))
	}
	for _, line := range strings.Split(strings.TrimSpace(string(out)), "\n") {
		if strings.HasPrefix(line, "PW_") {
			t.Errorf("gh wrapper exported %q to the binary's environment", line)
		}
	}
}
