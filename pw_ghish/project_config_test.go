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
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestParseProjectConfigTOML_AllSectionsAndTypes(t *testing.T) {
	tomlData := `
# Top-level comment
[gerrit]
scope_list_to_project = false
submodule_policy = "forbid-manual-rolls"
forbid_topics = true

[gerrit.shortlinks]
"acmerev" = "acme-internal-review.googlesource.com"
"fxr/i" = "turquoise-internal-review.googlesource.com"

[ci]
providers = ["buildbucket"]
hide_tag_filters = [
  "hide-in-gerrit:subbuild",
  "hide-in-gerrit:experimental",
]
skip_retry_tag_filters = ["skip-retry-in-gerrit:subbuild"]
preferred_logs = ["failure summary", "stdout", "stderr"]
include_summary_markdown = false
local_presubmit_hint = "fx test //src/..."

[issue]
default_component = 1234567
quota_project = "acme-quota-project"
trailer_format = "Fixed: {id}"
use_owners_components = false

[issue.path_components]
"src/audio/" = 7654321
"src/video/" = 9998887

[worktree]
slot_prefix = "acme-wt-"
warmup_driver = "bazel"
`

	cfg := DefaultProjectConfig()
	err := ParseProjectConfigTOML("/repo/.ghish.toml", tomlData, cfg)
	if err != nil {
		t.Fatalf("ParseProjectConfigTOML failed: %v", err)
	}

	if cfg.ScopeListToProject() != false {
		t.Errorf("ScopeListToProject() = true, want false")
	}
	if cfg.Gerrit.SubmodulePolicy != "forbid-manual-rolls" {
		t.Errorf("SubmodulePolicy = %q, want %q", cfg.Gerrit.SubmodulePolicy, "forbid-manual-rolls")
	}
	if !cfg.Gerrit.ForbidTopics {
		t.Errorf("ForbidTopics = false, want true")
	}
	wantShortlinks := map[string]string{
		"acmerev": "acme-internal-review.googlesource.com",
		"fxr/i":   "turquoise-internal-review.googlesource.com",
	}
	if !reflect.DeepEqual(cfg.Gerrit.Shortlinks, wantShortlinks) {
		t.Errorf("Shortlinks = %+v, want %+v", cfg.Gerrit.Shortlinks, wantShortlinks)
	}

	if !reflect.DeepEqual(cfg.CI.Providers, []string{"buildbucket"}) {
		t.Errorf("CI.Providers = %v, want [buildbucket]", cfg.CI.Providers)
	}
	wantHide := []string{"hide-in-gerrit:subbuild", "hide-in-gerrit:experimental"}
	if !reflect.DeepEqual(cfg.CI.HideTagFilters, wantHide) {
		t.Errorf("CI.HideTagFilters = %v, want %v", cfg.CI.HideTagFilters, wantHide)
	}
	if !reflect.DeepEqual(cfg.CI.SkipRetryTagFilters, []string{"skip-retry-in-gerrit:subbuild"}) {
		t.Errorf("CI.SkipRetryTagFilters = %v, want [skip-retry-in-gerrit:subbuild]", cfg.CI.SkipRetryTagFilters)
	}
	if !reflect.DeepEqual(cfg.CI.PreferredLogs, []string{"failure summary", "stdout", "stderr"}) {
		t.Errorf("CI.PreferredLogs = %v", cfg.CI.PreferredLogs)
	}
	if cfg.IncludeSummaryMarkdown() != false {
		t.Errorf("IncludeSummaryMarkdown() = true, want false")
	}
	if cfg.CI.LocalPresubmitHint != "fx test //src/..." {
		t.Errorf("CI.LocalPresubmitHint = %q, want %q", cfg.CI.LocalPresubmitHint, "fx test //src/...")
	}

	if cfg.Issue.DefaultComponent != 1234567 {
		t.Errorf("Issue.DefaultComponent = %d, want 1234567", cfg.Issue.DefaultComponent)
	}
	if cfg.Issue.QuotaProject != "acme-quota-project" {
		t.Errorf("Issue.QuotaProject = %q, want %q", cfg.Issue.QuotaProject, "acme-quota-project")
	}
	if cfg.Issue.TrailerFormat != "Fixed: {id}" {
		t.Errorf("Issue.TrailerFormat = %q, want %q", cfg.Issue.TrailerFormat, "Fixed: {id}")
	}
	if cfg.UseOwnersComponents() != false {
		t.Errorf("UseOwnersComponents() = true, want false")
	}
	wantPaths := map[string]int64{
		"src/audio/": 7654321,
		"src/video/": 9998887,
	}
	if !reflect.DeepEqual(cfg.Issue.PathComponents, wantPaths) {
		t.Errorf("Issue.PathComponents = %+v, want %+v", cfg.Issue.PathComponents, wantPaths)
	}

	if cfg.Worktree.SlotPrefix != "acme-wt-" {
		t.Errorf("Worktree.SlotPrefix = %q, want %q", cfg.Worktree.SlotPrefix, "acme-wt-")
	}
	if cfg.Worktree.WarmupDriver != "bazel" {
		t.Errorf("Worktree.WarmupDriver = %q, want %q", cfg.Worktree.WarmupDriver, "bazel")
	}
}

func TestParseProjectConfigTOML_StrictErrors(t *testing.T) {
	tests := []struct {
		name        string
		toml        string
		wantFileStr string
		wantMsg     string
	}{
		{
			name:        "unknown section",
			toml:        "# comment\n[unknown_section]\nfoo = \"bar\"\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "unknown section",
		},
		{
			name:        "unknown key in gerrit",
			toml:        "[gerrit]\nforbid_topics = true\nbogus_key = \"val\"\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "unknown key",
		},
		{
			name:        "key outside section",
			toml:        "forbid_topics = true\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "outside of any [section]",
		},
		{
			name:        "missing equals",
			toml:        "[ci]\nproviders [\"buildbucket\"]\n",
			wantFileStr: "/test/.ghish.toml:2:",
			wantMsg:     "expected",
		},
		{
			name:        "invalid boolean",
			toml:        "[gerrit]\nforbid_topics = \"yes\"\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "forbid_topics",
		},
		{
			name:        "invalid integer",
			toml:        "[issue]\ndefault_component = \"abc\"\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "default_component",
		},
		{
			name:        "invalid submodule_policy",
			toml:        "[gerrit]\nsubmodule_policy = \"invalid-policy\"\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "submodule_policy",
		},
		{
			name:        "unclosed string",
			toml:        "[ci]\nlocal_presubmit_hint = \"unterminated\n",
			wantFileStr: "/test/.ghish.toml:2:",
			wantMsg:     "string",
		},
		{
			name:        "unclosed array",
			toml:        "[ci]\nproviders = [\n  \"buildbucket\"\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "end of file",
		},
		{
			name:        "malformed path_components glob",
			toml:        "[issue.path_components]\n\"src/[unclosed\" = 12345\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "glob",
		},
		{
			name:        "unknown ci provider",
			toml:        "[ci]\nproviders = [\"bogus\"]\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "providers",
		},
		{
			name:        "empty ci provider entry",
			toml:        "[ci]\nproviders = [\"\"]\n",
			wantFileStr: "/test/.ghish.toml:",
			wantMsg:     "providers",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			cfg := DefaultProjectConfig()
			err := ParseProjectConfigTOML("/test/.ghish.toml", tt.toml, cfg)
			if err == nil {
				t.Fatalf("Expected error for %s, got nil", tt.name)
			}
			errStr := err.Error()
			if !strings.Contains(errStr, tt.wantFileStr) {
				t.Errorf("Expected error to contain %q, got: %v", tt.wantFileStr, err)
			}
			if !strings.Contains(strings.ToLower(errStr), strings.ToLower(tt.wantMsg)) {
				t.Errorf("Expected error to contain %q, got: %v", tt.wantMsg, err)
			}
		})
	}
}

func TestLoadProjectConfig_HierarchyAndGitConfigOverrides(t *testing.T) {
	ctx := context.Background()
	repoRoot := t.TempDir()
	if err := os.Mkdir(filepath.Join(repoRoot, ".git"), 0755); err != nil {
		t.Fatalf("failed to create .git dir: %v", err)
	}
	subDir := filepath.Join(repoRoot, "components", "sensor")
	if err := os.MkdirAll(subDir, 0755); err != nil {
		t.Fatalf("failed to create subdir: %v", err)
	}

	rootTOML := `
[gerrit]
shortlinks = { "acmerev" = "acme-internal-review.googlesource.com" }
forbid_topics = false
submodule_policy = "warn-unpushed"

[ci]
hide_tag_filters = ["hide-in-gerrit:subbuild"]
local_presubmit_hint = "./pw presubmit"

[issue]
default_component = 1000
path_components = { "components/" = 2000 }
`
	if err := os.WriteFile(filepath.Join(repoRoot, ".ghish.toml"), []byte(rootTOML), 0644); err != nil {
		t.Fatalf("failed to write root .ghish.toml: %v", err)
	}

	subTOML := `
[gerrit]
shortlinks = { "sensorrev" = "sensor-review.googlesource.com" }
forbid_topics = true

[issue]
default_component = 3000
path_components = { "components/sensor/" = 3001 }
`
	if err := os.WriteFile(filepath.Join(subDir, ".ghish.toml"), []byte(subTOML), 0644); err != nil {
		t.Fatalf("failed to write subdir .ghish.toml: %v", err)
	}

	mockGit := &MockGitRunner{
		RunFn: func(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
			if len(args) >= 2 && args[0] == "rev-parse" && args[1] == "--show-toplevel" {
				stdout.Write([]byte(repoRoot + "\n"))
				return nil
			}
			if len(args) >= 3 && args[0] == "config" && args[1] == "--get-regexp" {
				stdout.Write([]byte(strings.Join([]string{
					"ghish.ci.localpresubmithint bazelisk test //components/sensor/...",
					"ghish.gerrit.shortlinks.gitrev git-override-review.googlesource.com",
					"ghish.issue.defaultcomponent 4000",
				}, "\n") + "\n"))
				return nil
			}
			return nil
		},
	}

	projCfg, err := LoadProjectConfig(ctx, mockGit, subDir)
	if err != nil {
		t.Fatalf("LoadProjectConfig failed: %v", err)
	}

	// Root submodule_policy preserved
	if projCfg.Gerrit.SubmodulePolicy != "warn-unpushed" {
		t.Errorf("SubmodulePolicy = %q, want warn-unpushed", projCfg.Gerrit.SubmodulePolicy)
	}
	// Subdir forbid_topics overrides root
	if !projCfg.Gerrit.ForbidTopics {
		t.Errorf("ForbidTopics = false, want true from subdir override")
	}
	// Shortlinks merged from root, subdir, and git config
	wantShortlinks := map[string]string{
		"acmerev":   "acme-internal-review.googlesource.com",
		"sensorrev": "sensor-review.googlesource.com",
		"gitrev":    "git-override-review.googlesource.com",
	}
	if !reflect.DeepEqual(projCfg.Gerrit.Shortlinks, wantShortlinks) {
		t.Errorf("Shortlinks = %+v, want %+v", projCfg.Gerrit.Shortlinks, wantShortlinks)
	}
	// Git config overrides local_presubmit_hint and default_component
	if projCfg.CI.LocalPresubmitHint != "bazelisk test //components/sensor/..." {
		t.Errorf("LocalPresubmitHint = %q, want git config override", projCfg.CI.LocalPresubmitHint)
	}
	if projCfg.Issue.DefaultComponent != 4000 {
		t.Errorf("DefaultComponent = %d, want 4000 from git config override", projCfg.Issue.DefaultComponent)
	}
	// PathComponents merged from root and subdir
	wantPaths := map[string]int64{
		"components/":        2000,
		"components/sensor/": 3001,
	}
	if !reflect.DeepEqual(projCfg.Issue.PathComponents, wantPaths) {
		t.Errorf("PathComponents = %+v, want %+v", projCfg.Issue.PathComponents, wantPaths)
	}
}

func TestParseChangeTarget_CustomShortlinksFromProjectConfig(t *testing.T) {
	ctx := context.Background()
	repoRoot := t.TempDir()
	tomlContent := `
[gerrit]
shortlinks = { "acmerev" = "acme-internal-review.googlesource.com", "acmer/i/" = "acme-corp-review.googlesource.com" }
`
	if err := os.WriteFile(filepath.Join(repoRoot, ".ghish.toml"), []byte(tomlContent), 0644); err != nil {
		t.Fatalf("failed to write .ghish.toml: %v", err)
	}

	cfg := &Config{
		CWD: repoRoot,
		Git: &MockGitRunner{},
	}
	t.Cleanup(func() {
		SetCustomShortlinks(nil)
	})

	projCfg, err := cfg.LoadProjectConfig(ctx)
	if err != nil {
		t.Fatalf("cfg.LoadProjectConfig failed: %v", err)
	}
	if projCfg == nil {
		t.Fatal("expected non-nil ProjectConfig")
	}

	got1 := ParseChangeTarget("acmerev/54321/3")
	want1 := ParsedChangeTarget{
		Host:     "acme-internal-review.googlesource.com",
		ChangeID: "54321",
		Revision: "3",
	}
	if got1 != want1 {
		t.Errorf("ParseChangeTarget(acmerev/54321/3) = %+v, want %+v", got1, want1)
	}

	got2 := ParseChangeTarget("acmer/i/98765")
	want2 := ParsedChangeTarget{
		Host:     "acme-corp-review.googlesource.com",
		ChangeID: "98765",
		Revision: "current",
	}
	if got2 != want2 {
		t.Errorf("ParseChangeTarget(acmer/i/98765) = %+v, want %+v", got2, want2)
	}

	if !isChangeIdentifier("acmerev/54321") {
		t.Errorf("isChangeIdentifier(acmerev/54321) = false, want true")
	}
}

func TestParseProjectConfigTOML_ExtraSectionKeys(t *testing.T) {
	cfg := DefaultProjectConfig()
	tomlContent := `
[gerrit]
host = "acme-review.googlesource.com"
project = "acme/firmware"
default_branch = "dev"

[ci]
buildbucket_host = "cr-buildbucket.appspot.com"
try_buckets = ["acme/try", "acme/ci"]

[bugs]
system = "buganizer"
prefix = "b/"
host = "issues.acme.dev"
default_component_id = 98765
require_trailer = true
`
	if err := ParseProjectConfigTOML("/repo/.ghish.toml", tomlContent, cfg); err != nil {
		t.Fatalf("ParseProjectConfigTOML failed: %v", err)
	}
	if cfg.Gerrit.Host != "acme-review.googlesource.com" || cfg.Gerrit.Project != "acme/firmware" || cfg.Gerrit.DefaultBranch != "dev" {
		t.Errorf("unexpected Gerrit config: %+v", cfg.Gerrit)
	}
	if cfg.CI.BuildbucketHost != "cr-buildbucket.appspot.com" || !reflect.DeepEqual(cfg.CI.TryBuckets, []string{"acme/try", "acme/ci"}) {
		t.Errorf("unexpected CI config: %+v", cfg.CI)
	}
	if cfg.Issue.System != "buganizer" || cfg.Issue.Prefix != "b/" || cfg.Issue.Host != "issues.acme.dev" || cfg.Issue.DefaultComponent != 98765 || cfg.Issue.TrailerFormat != "Bug: b/{id}" {
		t.Errorf("unexpected Issue config: %+v", cfg.Issue)
	}
}

func TestMalformedProjectConfig_FailsFastOnCommands(t *testing.T) {
	repoRoot := t.TempDir()
	if err := os.WriteFile(filepath.Join(repoRoot, ".ghish.toml"), []byte("[gerrit]\nunknown_key = true\n"), 0644); err != nil {
		t.Fatalf("failed to write .ghish.toml: %v", err)
	}

	SetMockGit(t, nil)
	oldMockCWD := MockCWD
	MockCWD = repoRoot
	t.Cleanup(func() {
		MockCWD = oldMockCWD
	})

	_, err := executeCommand(RootCmd, "pr", "view", "12345")
	if err == nil {
		t.Fatal("expected error from 'pr view 12345' when .ghish.toml is malformed, got nil")
	}
	if !strings.Contains(err.Error(), "unknown key") {
		t.Errorf("expected error to mention 'unknown key', got: %v", err)
	}
}

func TestParseProjectConfigTOML_GerritRemoteAndRequirePushedPolicy(t *testing.T) {
	cfg := DefaultProjectConfig()
	tomlContent := `
[gerrit]
remote = "goog"
submodule_policy = "require-pushed"
`
	if err := ParseProjectConfigTOML("/repo/.ghish.toml", tomlContent, cfg); err != nil {
		t.Fatalf("ParseProjectConfigTOML failed: %v", err)
	}
	if cfg.Gerrit.Remote != "goog" {
		t.Errorf("Gerrit.Remote = %q, want %q", cfg.Gerrit.Remote, "goog")
	}
	if cfg.Gerrit.SubmodulePolicy != "require-pushed" {
		t.Errorf("Gerrit.SubmodulePolicy = %q, want %q", cfg.Gerrit.SubmodulePolicy, "require-pushed")
	}

	if err := applyGitConfigOverrides("ghish.gerrit.remote partner\n", cfg); err != nil {
		t.Fatalf("applyGitConfigOverrides failed: %v", err)
	}
	if cfg.Gerrit.Remote != "partner" {
		t.Errorf("Gerrit.Remote after git config override = %q, want %q", cfg.Gerrit.Remote, "partner")
	}
}

func TestParseProjectConfigTOML_CIProvidersValidation(t *testing.T) {
	validCases := []string{
		`providers = ["auto"]`,
		`providers = ["buildbucket"]`,
		`providers = ["busytown"]`,
		`providers = ["buildbucket", "busytown"]`,
		`providers = ["luci", "treehugger", "android-build"]`,
	}
	for _, tc := range validCases {
		cfg := DefaultProjectConfig()
		if err := ParseProjectConfigTOML("/repo/.ghish.toml", "[ci]\n"+tc+"\n", cfg); err != nil {
			t.Errorf("expected %s to succeed, got error: %v", tc, err)
		}
	}

	cfg := DefaultProjectConfig()
	if err := applyGitConfigOverrides("ghish.ci.providers bogus\n", cfg); err == nil {
		t.Errorf("expected git config ghish.ci.providers=bogus to fail validation, got nil")
	}
}

func TestParseProjectConfigTOML_WarmupDriverValidationAndAngleBracketPlaceholder(t *testing.T) {
	for _, validDriver := range []string{"bazel", "none", "gn"} {
		cfg := DefaultProjectConfig()
		if err := ParseProjectConfigTOML("/repo/.ghish.toml", "[worktree]\nwarmup_driver = \""+validDriver+"\"\n", cfg); err != nil {
			t.Errorf("expected warmup_driver=%q to succeed, got: %v", validDriver, err)
		}
	}

	cfg := DefaultProjectConfig()
	if err := ParseProjectConfigTOML("/repo/.ghish.toml", "[worktree]\nwarmup_driver = \"cmake-bogus\"\n", cfg); err == nil {
		t.Errorf("expected invalid worktree.warmup_driver to fail validation, got nil")
	}
	if err := applyGitConfigOverrides("ghish.worktree.warmupDriver cmake-bogus\n", cfg); err == nil {
		t.Errorf("expected invalid git config ghish.worktree.warmupDriver to fail validation, got nil")
	}

	if got := FormatBugIDWithFormat(570137899, "Bug: b/<id>"); got != "b/570137899" {
		t.Errorf("FormatBugIDWithFormat(570137899, \"Bug: b/<id>\") = %q, want \"b/570137899\"", got)
	}
	if got := FormatBugIDWithFormat(570137899, "fxbug.dev/<id>"); got != "fxbug.dev/570137899" {
		t.Errorf("FormatBugIDWithFormat(570137899, \"fxbug.dev/<id>\") = %q, want \"fxbug.dev/570137899\"", got)
	}
}

func TestParseProjectConfigTOML_DocumentedCookbookRecipes(t *testing.T) {
	recipes := map[string]string{
		"luci_monorepo": `
[gerrit]
host = "pigweed-review.googlesource.com"
project = "pigweed/pigweed"
remote = "origin"
default_branch = "main"
scope_list_to_project = true
submodule_policy = "warn-unpushed"

[gerrit.shortlinks]
"pwrev" = "pigweed-review.googlesource.com"
"pwrev/i" = "pigweed-internal-review.googlesource.com"

[ci]
providers = ["buildbucket"]
buildbucket_host = "cr-buildbucket.appspot.com"
try_buckets = ["pigweed/try"]
hide_tag_filters = ["hide-in-gerrit:subbuild"]
skip_retry_tag_filters = ["skip-retry-in-gerrit:subbuild"]
preferred_logs = ["failure summary", "stdout", "stderr"]
include_summary_markdown = true
local_presubmit_hint = "./pw presubmit"

[issue]
system = "buganizer"
prefix = "b/"
host = "issues.pigweed.dev"
default_component = 1194524
trailer_format = "Bug: b/{id}"
use_owners_components = true

[issue.path_components]
"pw_bluetooth_sapphire/**" = 1456789
"pw_kernel/**" = 1456790

[worktree]
slot_prefix = "pw-"
warmup_driver = "bazel"
`,
		"android_busytown": `
[gerrit]
host = "googleplex-android-review.googlesource.com"
project = "platform/external/myproject"
remote = "goog"
default_branch = "main"
scope_list_to_project = true
submodule_policy = "forbid-manual-rolls"

[gerrit.shortlinks]
"ag" = "googleplex-android-review.googlesource.com"

[ci]
providers = ["busytown"]
include_summary_markdown = true
local_presubmit_hint = "atest MyProjectTests"

[issue]
system = "buganizer"
prefix = "b/"
host = "issuetracker.google.com"
default_component = 1357924
trailer_format = "Bug: {id}"
use_owners_components = true

[worktree]
slot_prefix = "and-"
warmup_driver = "none"
`,
		"dual_stack": `
[gerrit]
host = "partner-android-review.googlesource.com"
project = "device/google/firmware"
remote = "partner"
default_branch = "main"
scope_list_to_project = true

[ci]
providers = ["buildbucket", "busytown"]
buildbucket_host = "cr-buildbucket.appspot.com"
try_buckets = ["firmware/try"]
hide_tag_filters = ["hide-in-gerrit:subbuild"]
skip_retry_tag_filters = ["skip-retry-in-gerrit:subbuild"]

[issue]
default_component = 1928374
trailer_format = "Bug: b/{id}"
`,
		"superproject_subdir": `
[gerrit]
remote = "partner"
forbid_topics = false

[ci]
local_presubmit_hint = "bazelisk test //vendor/sensor/..."

[issue]
default_component = 2000200
[issue.path_components]
"vendor/sensor/dsp/**" = 2000201
`,
	}

	for name, content := range recipes {
		t.Run(name, func(t *testing.T) {
			cfg := DefaultProjectConfig()
			if err := ParseProjectConfigTOML("/repo/.ghish.toml", content, cfg); err != nil {
				t.Fatalf("ParseProjectConfigTOML(%s) failed: %v", name, err)
			}
		})
	}
}

func TestResolveBugTrailerFormat(t *testing.T) {
	ctx := t.Context()

	t.Run("nil config", func(t *testing.T) {
		got, err := ResolveBugTrailerFormat(ctx, nil)
		if err != nil {
			t.Fatalf("ResolveBugTrailerFormat(nil) unexpected error: %v", err)
		}
		if got != "Bug: b/{id}" {
			t.Errorf("ResolveBugTrailerFormat(nil) = %q, want %q", got, "Bug: b/{id}")
		}
	})

	t.Run("empty config", func(t *testing.T) {
		got, err := ResolveBugTrailerFormat(ctx, &Config{ProjectConfig: &ProjectConfig{}})
		if err != nil {
			t.Fatalf("ResolveBugTrailerFormat(empty) unexpected error: %v", err)
		}
		if got != "Bug: b/{id}" {
			t.Errorf("ResolveBugTrailerFormat(empty) = %q, want %q", got, "Bug: b/{id}")
		}
	})

	t.Run("issue prefix fallback", func(t *testing.T) {
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, ".ghish.toml"), []byte("[issue]\nprefix = \"fxb/\"\n"), 0644); err != nil {
			t.Fatalf("failed to write .ghish.toml: %v", err)
		}
		got, err := ResolveBugTrailerFormat(ctx, &Config{CWD: dir, Git: &MockGitRunner{}})
		if err != nil {
			t.Fatalf("ResolveBugTrailerFormat(prefix) unexpected error: %v", err)
		}
		if got != "Bug: fxb/{id}" {
			t.Errorf("ResolveBugTrailerFormat(prefix) = %q, want %q", got, "Bug: fxb/{id}")
		}
	})

	t.Run("trailer_format overrides prefix", func(t *testing.T) {
		dir := t.TempDir()
		tomlData := "[issue]\nprefix = \"fxb/\"\ntrailer_format = \"Bug: fxbug.dev/{id}\"\n"
		if err := os.WriteFile(filepath.Join(dir, ".ghish.toml"), []byte(tomlData), 0644); err != nil {
			t.Fatalf("failed to write .ghish.toml: %v", err)
		}
		got, err := ResolveBugTrailerFormat(ctx, &Config{CWD: dir, Git: &MockGitRunner{}})
		if err != nil {
			t.Fatalf("ResolveBugTrailerFormat(trailer_format+prefix) unexpected error: %v", err)
		}
		if got != "Bug: fxbug.dev/{id}" {
			t.Errorf("ResolveBugTrailerFormat(trailer_format+prefix) = %q, want %q", got, "Bug: fxbug.dev/{id}")
		}
	})

	t.Run("malformed .ghish.toml returns error", func(t *testing.T) {
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, ".ghish.toml"), []byte("[issue]\nunknown_key = true\n"), 0644); err != nil {
			t.Fatalf("failed to write .ghish.toml: %v", err)
		}
		got, err := ResolveBugTrailerFormat(ctx, &Config{CWD: dir, Git: &MockGitRunner{}})
		if err == nil {
			t.Fatalf("ResolveBugTrailerFormat(malformed) = %q, want error", got)
		}
	})
}

func TestLoadProjectConfig_RealGitRunnerUsesFilesystemTopLevel(t *testing.T) {
	r := newTestGitRepo(t)
	subDir := filepath.Join(r.WorkDir, "nested", "module")
	if err := os.MkdirAll(subDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(r.WorkDir, ".ghish.toml"), []byte("[gerrit]\nremote = \"goog\"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(subDir, ".ghish.toml"), []byte("[gerrit]\ndefault_branch = \"dev\"\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	projCfg, err := LoadProjectConfig(t.Context(), &RealGitRunner{Dir: subDir}, subDir)
	if err != nil {
		t.Fatalf("LoadProjectConfig with RealGitRunner failed: %v", err)
	}
	if projCfg.Gerrit.Remote != "goog" {
		t.Errorf("Gerrit.Remote = %q, want goog", projCfg.Gerrit.Remote)
	}
	if projCfg.Gerrit.DefaultBranch != "dev" {
		t.Errorf("Gerrit.DefaultBranch = %q, want dev", projCfg.Gerrit.DefaultBranch)
	}
}

// TestGitConfigOnlyProjectConfig_ReusesPrefetch checks that the issue
// component lookup reads ghish.* overrides from the shared NUL-delimited
// prefetch (no extra subprocess once it ran), so a value containing a newline
// cannot inject a second key the way the line format could.
func TestGitConfigOnlyProjectConfig_ReusesPrefetch(t *testing.T) {
	isolateGitEnv(t)
	dir := initTestRepo(t)
	runTestGit(t, dir, "config", "ghish.issue.defaultcomponent", "123")
	// In line format this value would parse as a second, injected key.
	runTestGit(t, dir, "config", "ghish.gerrit.project", "proj\nghish.issue.defaultcomponent 999")
	dg := NewGitClientInDir(dir).(*defaultGitClient)
	if _, seeded := dg.ensurePrefetched(t.Context()); !seeded {
		t.Fatal("test setup: config prefetch did not run")
	}

	pcfg := DefaultProjectConfig()
	var applied bool
	var err error
	calls := recordGitCalls(t, func() { applied, err = dg.applyGitConfigToProjectConfig(t.Context(), pcfg) })
	if err != nil {
		t.Fatalf("applyGitConfigToProjectConfig() failed: %v", err)
	}
	if len(calls) != 0 {
		t.Errorf("applyGitConfigToProjectConfig() spawned %q, want no subprocesses after the prefetch", calls)
	}
	if !applied {
		t.Fatal("applyGitConfigToProjectConfig() = false, want the ghish.* overrides applied")
	}
	if got := pcfg.Issue.DefaultComponent; got != 123 {
		t.Errorf("Issue.DefaultComponent = %d, want 123 (a newline in another value must not inject a key)", got)
	}
}

// TestGitConfigOnlyProjectConfig_NoOverrides checks that a repository without
// ghish.* keys reports applied=false, so later component tiers apply.
func TestGitConfigOnlyProjectConfig_NoOverrides(t *testing.T) {
	isolateGitEnv(t)
	dir := initTestRepo(t)
	dg := NewGitClientInDir(dir).(*defaultGitClient)
	pcfg := DefaultProjectConfig()
	applied, err := dg.applyGitConfigToProjectConfig(t.Context(), pcfg)
	if err != nil {
		t.Fatalf("applyGitConfigToProjectConfig() failed: %v", err)
	}
	if applied {
		t.Errorf("applyGitConfigToProjectConfig() = true, want false without ghish.* keys")
	}
}
