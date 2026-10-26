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
	"cmp"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"

	"github.com/BurntSushi/toml"
	"github.com/spf13/cobra"
)

// ProjectConfig represents the merged declarative configuration from `.ghish.toml`
// files (from the repository root down to the working directory) and `git config`
// (`ghish.*`) overrides.
type ProjectConfig struct {
	Gerrit   GerritProjectConfig   `json:"gerrit"`
	CI       CIProjectConfig       `json:"ci"`
	Issue    IssueProjectConfig    `json:"issue"`
	Worktree WorktreeProjectConfig `json:"worktree"`
	Oncall   OncallProjectConfig   `json:"oncall"`
}

// GerritProjectConfig holds `[gerrit]` configuration settings.
type GerritProjectConfig struct {
	Host               string            `json:"host,omitempty"`
	Project            string            `json:"project,omitempty"`
	Remote             string            `json:"remote,omitempty"`
	DefaultBranch      string            `json:"default_branch,omitempty"`
	Shortlinks         map[string]string `json:"shortlinks,omitempty"`
	ScopeListToProject *bool             `json:"scope_list_to_project,omitempty"`
	SubmodulePolicy    string            `json:"submodule_policy,omitempty"`
	ForbidTopics       bool              `json:"forbid_topics,omitempty"`
}

// CIProjectConfig holds `[ci]` configuration settings.
type CIProjectConfig struct {
	BuildbucketHost        string   `json:"buildbucket_host,omitempty"`
	TryBuckets             []string `json:"try_buckets,omitempty"`
	Providers              []string `json:"providers,omitempty"`
	HideTagFilters         []string `json:"hide_tag_filters,omitempty"`
	SkipRetryTagFilters    []string `json:"skip_retry_tag_filters,omitempty"`
	PreferredLogs          []string `json:"preferred_logs,omitempty"`
	IncludeSummaryMarkdown *bool    `json:"include_summary_markdown,omitempty"`
	LocalPresubmitHint     string   `json:"local_presubmit_hint,omitempty"`
}

// IssueProjectConfig holds `[issue]` (or `[bugs]`) configuration settings.
type IssueProjectConfig struct {
	System              string           `json:"system,omitempty"`
	Prefix              string           `json:"prefix,omitempty"`
	Host                string           `json:"host,omitempty"`
	DefaultComponent    int64            `json:"default_component,omitempty"`
	QuotaProject        string           `json:"quota_project,omitempty"`
	TrailerFormat       string           `json:"trailer_format,omitempty"`
	UseOwnersComponents *bool            `json:"use_owners_components,omitempty"`
	PathComponents      map[string]int64 `json:"path_components,omitempty"`
	Components          map[string]int64 `json:"components,omitempty"`
	Hotlists            map[string]int64 `json:"hotlists,omitempty"`
}

// WorktreeProjectConfig holds `[worktree]` configuration settings.
type WorktreeProjectConfig struct {
	SlotPrefix   string `json:"slot_prefix,omitempty"`
	WarmupDriver string `json:"warmup_driver,omitempty"`
}

// OncallProjectConfig holds `[oncall]` configuration settings.
type OncallProjectConfig struct {
	ScheduleFile string `json:"schedule_file,omitempty"`
}

func cfgBoolPtr(v bool) *bool {
	return &v
}

// DefaultProjectConfig returns a ProjectConfig initialized with default settings.
func DefaultProjectConfig() *ProjectConfig {
	return &ProjectConfig{
		Gerrit: GerritProjectConfig{
			Shortlinks:         make(map[string]string),
			ScopeListToProject: cfgBoolPtr(true),
			SubmodulePolicy:    "allow",
		},
		CI: CIProjectConfig{
			PreferredLogs:          []string{"failure summary", "failure_summary", "stdout", "stderr"},
			IncludeSummaryMarkdown: cfgBoolPtr(true),
		},
		Issue: IssueProjectConfig{
			UseOwnersComponents: cfgBoolPtr(true),
			PathComponents:      make(map[string]int64),
			Components:          make(map[string]int64),
			Hotlists:            make(map[string]int64),
		},
	}
}

// LoadCommandProjectConfig loads the repository ProjectConfig for cmd,
// returning DefaultProjectConfig() only when cmd has no Config attached,
// and propagating any .ghish.toml or git config parse error.
func LoadCommandProjectConfig(cmd *cobra.Command) (*ProjectConfig, error) {
	if cmd == nil {
		return DefaultProjectConfig(), nil
	}
	cfg := GetConfig(cmd)
	if cfg == nil {
		cfg = &Config{Git: DefaultGitRunner}
	}
	ctx := cmd.Context()
	if ctx == nil {
		ctx = context.Background()
	}
	return cfg.LoadProjectConfig(ctx)
}

// ScopeListToProject returns whether `gh pr list` should scope queries to the current project.
func (c *ProjectConfig) ScopeListToProject() bool {
	if c == nil || c.Gerrit.ScopeListToProject == nil {
		return true
	}
	return *c.Gerrit.ScopeListToProject
}

// IncludeSummaryMarkdown returns whether CI failure reports should include build SummaryMarkdown.
func (c *ProjectConfig) IncludeSummaryMarkdown() bool {
	if c == nil || c.CI.IncludeSummaryMarkdown == nil {
		return true
	}
	return *c.CI.IncludeSummaryMarkdown
}

// UseOwnersComponents returns whether issue creation should infer components from OWNERS files.
func (c *ProjectConfig) UseOwnersComponents() bool {
	if c == nil || c.Issue.UseOwnersComponents == nil {
		return true
	}
	return *c.Issue.UseOwnersComponents
}

// PreferredLogs returns the ordered list of preferred step log stream names.
func (c *ProjectConfig) PreferredLogs() []string {
	if c == nil || len(c.CI.PreferredLogs) == 0 {
		return []string{"failure summary", "failure_summary", "stdout", "stderr", "full contents"}
	}
	return c.CI.PreferredLogs
}

// EffectiveSkipRetryTagFilters returns the tag filters that should be skipped on `gh run rerun --failed`.
func (c *ProjectConfig) EffectiveSkipRetryTagFilters() []string {
	if c != nil && len(c.CI.SkipRetryTagFilters) > 0 {
		return c.CI.SkipRetryTagFilters
	}
	return []string{"skip-retry-in-gerrit:subbuild"}
}

var (
	customShortlinksMu sync.RWMutex
	customShortlinks   []gerritShortlink
)

// SetCustomShortlinks replaces the active custom shortlink mappings used by ParseChangeTarget.
func SetCustomShortlinks(shortlinks map[string]string) {
	customShortlinksMu.Lock()
	defer customShortlinksMu.Unlock()
	if len(shortlinks) == 0 {
		customShortlinks = nil
		return
	}
	list := make([]gerritShortlink, 0, len(shortlinks))
	for prefix, host := range shortlinks {
		p := strings.TrimSpace(prefix)
		h := CanonicalGerritHost(host)
		if p == "" || h == "" {
			continue
		}
		if !strings.HasSuffix(p, "/") {
			p += "/"
		}
		list = append(list, gerritShortlink{prefix: p, host: h})
	}
	// Sort longest prefix first so more specific prefixes (e.g. "acmer/i/") match before shorter ones ("acmer/").
	slices.SortFunc(list, func(a, b gerritShortlink) int {
		if c := cmp.Compare(len(b.prefix), len(a.prefix)); c != 0 {
			return c
		}
		return cmp.Compare(a.prefix, b.prefix)
	})
	customShortlinks = list
}

func getCustomShortlinks() []gerritShortlink {
	customShortlinksMu.RLock()
	defer customShortlinksMu.RUnlock()
	if len(customShortlinks) == 0 {
		return nil
	}
	out := make([]gerritShortlink, len(customShortlinks))
	copy(out, customShortlinks)
	return out
}

// LoadProjectConfig loads and merges `.ghish.toml` files from the git repository root
// down to startDir, and applies `git config ghish.*` overrides on top.
func LoadProjectConfig(ctx context.Context, git GitRunner, startDir string) (*ProjectConfig, error) {
	cfg := DefaultProjectConfig()

	topLevel := ""
	realGit, isReal := asRealGitRunner(git)
	if isReal {
		if startDir == "" {
			if realGit != nil && realGit.Dir != "" {
				startDir = realGit.Dir
			} else if wd, err := os.Getwd(); err == nil {
				startDir = wd
			}
		}
		if startDir != "" {
			if absStart, err := filepath.Abs(startDir); err == nil {
				startDir = absStart
			}
			cand := findGitTopLevelOnDisk(startDir)
			if _, err := os.Stat(filepath.Join(cand, ".git")); err == nil {
				topLevel = cand
			}
		}
	}

	if topLevel == "" && git != nil {
		var out bytes.Buffer
		if err := git.Run(ctx, &out, io.Discard, "rev-parse", "--show-toplevel"); err == nil {
			topLevel = strings.TrimSpace(out.String())
		}
	}

	if startDir == "" {
		if isReal {
			if realGit != nil && realGit.Dir != "" {
				startDir = realGit.Dir
			} else if wd, err := os.Getwd(); err == nil {
				startDir = wd
			}
		} else if topLevel != "" {
			startDir = topLevel
		}
	} else if git != nil {
		if !isReal && topLevel == "" {
			if wd, err := os.Getwd(); err == nil && filepath.Clean(startDir) == filepath.Clean(wd) {
				startDir = ""
			}
		}
	}

	if startDir != "" {
		if absStart, err := filepath.Abs(startDir); err == nil {
			startDir = absStart
		}
		if topLevel != "" {
			if absTop, err := filepath.Abs(topLevel); err == nil {
				topLevel = absTop
			}
			if rel, err := filepath.Rel(topLevel, startDir); err != nil || strings.HasPrefix(rel, "..") {
				topLevel = findGitTopLevelOnDisk(startDir)
			}
		} else {
			topLevel = findGitTopLevelOnDisk(startDir)
		}

		dirs := collectConfigDirs(topLevel, startDir)
		for _, dir := range dirs {
			tomlPath := filepath.Join(dir, ".ghish.toml")
			data, err := os.ReadFile(tomlPath)
			if err != nil {
				if errors.Is(err, os.ErrNotExist) {
					continue
				}
				return nil, fmt.Errorf("failed to read %s: %w", tomlPath, err)
			}
			if err := ParseProjectConfigTOML(tomlPath, string(data), cfg); err != nil {
				return nil, err
			}
		}
	}

	if err := applyGitConfigFromRunner(ctx, git, cfg); err != nil {
		return nil, err
	}

	return cfg, nil
}

// FindGitTopLevelOnDisk walks upward from startDir to find the nearest ancestor
// containing a .git directory or file, returning startDir if none is found.
func FindGitTopLevelOnDisk(startDir string) string {
	return findGitTopLevelOnDisk(startDir)
}

func findGitTopLevelOnDisk(startDir string) string {
	curr := startDir
	for {
		if _, err := os.Stat(filepath.Join(curr, ".git")); err == nil {
			return curr
		}
		parent := filepath.Dir(curr)
		if parent == curr {
			return startDir
		}
		curr = parent
	}
}

// HasProjectConfigFileOnDisk reports whether a .ghish.toml file exists on disk
// between the git repository root and startDir.
func HasProjectConfigFileOnDisk(startDir string) bool {
	return hasProjectConfigFileOnDisk(startDir)
}

func hasProjectConfigFileOnDisk(startDir string) bool {
	if startDir == "" {
		return false
	}
	if absStart, err := filepath.Abs(startDir); err == nil {
		startDir = absStart
	}
	topLevel := findGitTopLevelOnDisk(startDir)
	for _, dir := range collectConfigDirs(topLevel, startDir) {
		if _, err := os.Stat(filepath.Join(dir, ".ghish.toml")); err == nil {
			return true
		}
	}
	return false
}

func collectConfigDirs(topLevel, startDir string) []string {
	if topLevel == "" {
		return []string{startDir}
	}
	rel, err := filepath.Rel(topLevel, startDir)
	if err != nil || strings.HasPrefix(rel, "..") {
		return []string{startDir}
	}
	if rel == "." {
		return []string{topLevel}
	}
	parts := strings.Split(rel, string(filepath.Separator))
	dirs := make([]string, 0, len(parts)+1)
	curr := topLevel
	dirs = append(dirs, curr)
	for _, part := range parts {
		if part == "" || part == "." {
			continue
		}
		curr = filepath.Join(curr, part)
		dirs = append(dirs, curr)
	}
	return dirs
}

type tomlProjectConfig struct {
	Gerrit   tomlGerritConfig   `toml:"gerrit"`
	CI       tomlCIConfig       `toml:"ci"`
	Issue    tomlIssueConfig    `toml:"issue"`
	Bugs     tomlIssueConfig    `toml:"bugs"`
	Worktree tomlWorktreeConfig `toml:"worktree"`
	Oncall   tomlOncallConfig   `toml:"oncall"`
}

type tomlGerritConfig struct {
	Host               *string           `toml:"host"`
	Project            *string           `toml:"project"`
	Remote             *string           `toml:"remote"`
	DefaultBranch      *string           `toml:"default_branch"`
	Shortlinks         map[string]string `toml:"shortlinks"`
	ScopeListToProject *bool             `toml:"scope_list_to_project"`
	SubmodulePolicy    *string           `toml:"submodule_policy"`
	ForbidTopics       *bool             `toml:"forbid_topics"`
}

type tomlCIConfig struct {
	BuildbucketHost        *string   `toml:"buildbucket_host"`
	TryBuckets             *[]string `toml:"try_buckets"`
	Providers              *[]string `toml:"providers"`
	HideTagFilters         *[]string `toml:"hide_tag_filters"`
	SkipRetryTagFilters    *[]string `toml:"skip_retry_tag_filters"`
	PreferredLogs          *[]string `toml:"preferred_logs"`
	IncludeSummaryMarkdown *bool     `toml:"include_summary_markdown"`
	LocalPresubmitHint     *string   `toml:"local_presubmit_hint"`
}

type tomlIssueConfig struct {
	System              *string          `toml:"system"`
	Prefix              *string          `toml:"prefix"`
	Host                *string          `toml:"host"`
	DefaultComponent    any              `toml:"default_component"`
	DefaultComponentID  any              `toml:"default_component_id"`
	QuotaProject        *string          `toml:"quota_project"`
	TrailerFormat       *string          `toml:"trailer_format"`
	RequireTrailer      any              `toml:"require_trailer"`
	UseOwnersComponents *bool            `toml:"use_owners_components"`
	PathComponents      map[string]int64 `toml:"path_components"`
	Components          map[string]int64 `toml:"components"`
	Hotlists            map[string]int64 `toml:"hotlists"`
}

type tomlWorktreeConfig struct {
	SlotPrefix   *string `toml:"slot_prefix"`
	WarmupDriver *string `toml:"warmup_driver"`
}

type tomlOncallConfig struct {
	ScheduleFile *string `toml:"schedule_file"`
}

func validatePathComponentPattern(pattern string) error {
	for _, seg := range strings.Split(filepath.ToSlash(pattern), "/") {
		if _, err := filepath.Match(seg, "test"); err != nil {
			return fmt.Errorf("invalid path_components glob pattern %q: %w", pattern, err)
		}
	}
	if _, err := filepath.Match(filepath.ToSlash(pattern), "test"); err != nil {
		return fmt.Errorf("invalid path_components glob pattern %q: %w", pattern, err)
	}
	return nil
}

func validateSubmodulePolicy(policy string) error {
	switch policy {
	case "allow", "warn-unpushed", "require-pushed", "forbid-manual-rolls":
		return nil
	default:
		return fmt.Errorf("must be one of \"allow\", \"warn-unpushed\", \"require-pushed\", or \"forbid-manual-rolls\", got %q", policy)
	}
}

func validateCIProviders(providers []string) error {
	for _, p := range providers {
		norm := strings.ToLower(strings.TrimSpace(p))
		switch norm {
		case "auto", "buildbucket", "luci", "busytown", "android-build", "android_build", "treehugger", "treetop", "gerrit":
		default:
			return fmt.Errorf("must be one of \"auto\", \"buildbucket\", \"busytown\", or \"gerrit\", got %q", p)
		}
	}
	return nil
}

func validateWarmupDriver(driver string) error {
	switch strings.ToLower(strings.TrimSpace(driver)) {
	case "", "bazel", "none", "gn":
		return nil
	default:
		return fmt.Errorf("must be one of \"bazel\" or \"none\", got %q", driver)
	}
}

func parseComponentValue(val any) (int64, error) {
	switch v := val.(type) {
	case int64:
		return v, nil
	case string:
		cleaned := strings.ReplaceAll(strings.TrimSpace(v), "_", "")
		n, err := strconv.ParseInt(cleaned, 10, 64)
		if err != nil {
			return 0, fmt.Errorf("expected integer, got %q", v)
		}
		return n, nil
	default:
		return 0, fmt.Errorf("expected integer, got %T", val)
	}
}

func applyTOMLIssueSection(filePath, section string, src tomlIssueConfig, dst *IssueProjectConfig) error {
	if src.System != nil {
		dst.System = *src.System
	}
	if src.Prefix != nil {
		dst.Prefix = *src.Prefix
	}
	if src.Host != nil {
		dst.Host = *src.Host
	}
	if src.DefaultComponent != nil {
		n, err := parseComponentValue(src.DefaultComponent)
		if err != nil {
			return fmt.Errorf("%s: invalid value for %s.default_component: %w", filePath, section, err)
		}
		dst.DefaultComponent = n
	}
	if src.DefaultComponentID != nil {
		n, err := parseComponentValue(src.DefaultComponentID)
		if err != nil {
			return fmt.Errorf("%s: invalid value for %s.default_component_id: %w", filePath, section, err)
		}
		dst.DefaultComponent = n
	}
	if src.QuotaProject != nil {
		dst.QuotaProject = *src.QuotaProject
	}
	if src.TrailerFormat != nil {
		dst.TrailerFormat = *src.TrailerFormat
	}
	if src.RequireTrailer != nil {
		switch v := src.RequireTrailer.(type) {
		case bool:
			if v && dst.TrailerFormat == "" {
				dst.TrailerFormat = "Bug: b/{id}"
			}
		case string:
			dst.TrailerFormat = v
		default:
			return fmt.Errorf("%s: invalid value for %s.require_trailer: expected boolean or string, got %T", filePath, section, src.RequireTrailer)
		}
	}
	if src.UseOwnersComponents != nil {
		dst.UseOwnersComponents = cfgBoolPtr(*src.UseOwnersComponents)
	}
	for k, v := range src.PathComponents {
		if err := validatePathComponentPattern(k); err != nil {
			return fmt.Errorf("%s: invalid key for %s.path_components: %w", filePath, section, err)
		}
		dst.PathComponents[k] = v
	}
	for k, v := range src.Components {
		alias := strings.TrimSpace(k)
		if alias == "" || v <= 0 {
			return fmt.Errorf("%s: invalid %s.components entry %q=%d: alias must be non-empty and ID must be positive", filePath, section, k, v)
		}
		dst.Components[alias] = v
	}
	for k, v := range src.Hotlists {
		alias := strings.TrimSpace(k)
		if alias == "" || v <= 0 {
			return fmt.Errorf("%s: invalid %s.hotlists entry %q=%d: alias must be non-empty and ID must be positive", filePath, section, k, v)
		}
		dst.Hotlists[alias] = v
	}
	return nil
}

// ParseProjectConfigTOML parses a `.ghish.toml` document into dst, returning an
// error if any syntax error, unknown section/key, or invalid value type is
// encountered.
func ParseProjectConfigTOML(filePath, content string, dst *ProjectConfig) error {
	if dst == nil {
		return fmt.Errorf("%s: destination ProjectConfig is nil", filePath)
	}
	if dst.Gerrit.Shortlinks == nil {
		dst.Gerrit.Shortlinks = make(map[string]string)
	}
	if dst.Issue.PathComponents == nil {
		dst.Issue.PathComponents = make(map[string]int64)
	}
	if dst.Issue.Components == nil {
		dst.Issue.Components = make(map[string]int64)
	}
	if dst.Issue.Hotlists == nil {
		dst.Issue.Hotlists = make(map[string]int64)
	}

	var raw tomlProjectConfig
	md, err := toml.Decode(content, &raw)
	if err != nil {
		var pe toml.ParseError
		if errors.As(err, &pe) && pe.Position.Line > 0 {
			return fmt.Errorf("%s:%d: %w", filePath, pe.Position.Line, err)
		}
		return fmt.Errorf("%s: %w", filePath, err)
	}

	if undecoded := md.Undecoded(); len(undecoded) > 0 {
		k := undecoded[0]
		switch k[0] {
		case "gerrit", "ci", "issue", "bugs", "worktree", "oncall":
			return fmt.Errorf("%s: unknown key %q in section [%s]", filePath, strings.Join(k[1:], "."), k[0])
		default:
			if len(k) == 1 && md.Type(k[0]) != "Hash" {
				return fmt.Errorf("%s: key %q outside of any [section]", filePath, k[0])
			}
			return fmt.Errorf("%s: unknown section [%s]", filePath, k[0])
		}
	}

	if raw.Gerrit.Host != nil {
		dst.Gerrit.Host = *raw.Gerrit.Host
	}
	if raw.Gerrit.Project != nil {
		dst.Gerrit.Project = *raw.Gerrit.Project
	}
	if raw.Gerrit.Remote != nil {
		dst.Gerrit.Remote = *raw.Gerrit.Remote
	}
	if raw.Gerrit.DefaultBranch != nil {
		dst.Gerrit.DefaultBranch = *raw.Gerrit.DefaultBranch
	}
	for k, v := range raw.Gerrit.Shortlinks {
		dst.Gerrit.Shortlinks[k] = v
	}
	if raw.Gerrit.ScopeListToProject != nil {
		dst.Gerrit.ScopeListToProject = cfgBoolPtr(*raw.Gerrit.ScopeListToProject)
	}
	if raw.Gerrit.SubmodulePolicy != nil {
		if err := validateSubmodulePolicy(*raw.Gerrit.SubmodulePolicy); err != nil {
			return fmt.Errorf("%s: invalid value for gerrit.submodule_policy: %w", filePath, err)
		}
		dst.Gerrit.SubmodulePolicy = *raw.Gerrit.SubmodulePolicy
	}
	if raw.Gerrit.ForbidTopics != nil {
		dst.Gerrit.ForbidTopics = *raw.Gerrit.ForbidTopics
	}

	if raw.CI.BuildbucketHost != nil {
		dst.CI.BuildbucketHost = *raw.CI.BuildbucketHost
	}
	if raw.CI.TryBuckets != nil {
		dst.CI.TryBuckets = *raw.CI.TryBuckets
	}
	if raw.CI.Providers != nil {
		if err := validateCIProviders(*raw.CI.Providers); err != nil {
			return fmt.Errorf("%s: invalid value for ci.providers: %w", filePath, err)
		}
		dst.CI.Providers = *raw.CI.Providers
	}
	if raw.CI.HideTagFilters != nil {
		dst.CI.HideTagFilters = *raw.CI.HideTagFilters
	}
	if raw.CI.SkipRetryTagFilters != nil {
		dst.CI.SkipRetryTagFilters = *raw.CI.SkipRetryTagFilters
	}
	if raw.CI.PreferredLogs != nil {
		dst.CI.PreferredLogs = *raw.CI.PreferredLogs
	}
	if raw.CI.IncludeSummaryMarkdown != nil {
		dst.CI.IncludeSummaryMarkdown = cfgBoolPtr(*raw.CI.IncludeSummaryMarkdown)
	}
	if raw.CI.LocalPresubmitHint != nil {
		dst.CI.LocalPresubmitHint = *raw.CI.LocalPresubmitHint
	}

	if err := applyTOMLIssueSection(filePath, "issue", raw.Issue, &dst.Issue); err != nil {
		return err
	}
	if err := applyTOMLIssueSection(filePath, "bugs", raw.Bugs, &dst.Issue); err != nil {
		return err
	}

	if raw.Worktree.SlotPrefix != nil {
		dst.Worktree.SlotPrefix = *raw.Worktree.SlotPrefix
	}
	if raw.Worktree.WarmupDriver != nil {
		if err := validateWarmupDriver(*raw.Worktree.WarmupDriver); err != nil {
			return fmt.Errorf("%s: invalid value for worktree.warmup_driver: %w", filePath, err)
		}
		dst.Worktree.WarmupDriver = *raw.Worktree.WarmupDriver
	}

	if raw.Oncall.ScheduleFile != nil {
		dst.Oncall.ScheduleFile = *raw.Oncall.ScheduleFile
	}

	return nil
}

func normalizeGitConfigSubkey(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	s = strings.ReplaceAll(s, "_", "")
	s = strings.ReplaceAll(s, "-", "")
	return s
}

func splitCSV(val string) []string {
	parts := strings.Split(val, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

// applyGitConfigOverrides applies line-format `git config --get-regexp`
// output ("key value" per line) on top of dst.
func applyGitConfigOverrides(output string, dst *ProjectConfig) error {
	return applyGitConfigEntries(parseGitConfigLines(output), dst)
}

// applyGitConfigEntries applies `ghish.*` git config entries on top of dst.
func applyGitConfigEntries(entries []gitConfigEntry, dst *ProjectConfig) error {
	if dst.Issue.PathComponents == nil {
		dst.Issue.PathComponents = make(map[string]int64)
	}
	if dst.Issue.Components == nil {
		dst.Issue.Components = make(map[string]int64)
	}
	if dst.Issue.Hotlists == nil {
		dst.Issue.Hotlists = make(map[string]int64)
	}
	for _, e := range entries {
		key, val := e.key, strings.TrimSpace(e.val)
		if !strings.HasPrefix(key, "ghish.") {
			continue
		}
		rest := strings.TrimPrefix(key, "ghish.")

		if strings.HasPrefix(rest, "gerrit.shortlinks.") {
			prefix := strings.TrimPrefix(rest, "gerrit.shortlinks.")
			if prefix != "" && val != "" {
				dst.Gerrit.Shortlinks[prefix] = val
			}
			continue
		}
		if strings.HasPrefix(rest, "shortlink.") {
			prefix := strings.TrimPrefix(rest, "shortlink.")
			if prefix != "" && val != "" {
				dst.Gerrit.Shortlinks[prefix] = val
			}
			continue
		}
		for _, pfx := range []string{"issue.pathcomponents.", "issue.path_components.", "issue.path-components.", "bugs.pathcomponents.", "bugs.path_components.", "bugs.path-components."} {
			if strings.HasPrefix(rest, pfx) {
				pathKey := strings.TrimPrefix(rest, pfx)
				if err := validatePathComponentPattern(pathKey); err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				n, err := strconv.ParseInt(val, 10, 64)
				if err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.Issue.PathComponents[pathKey] = n
				rest = ""
				break
			}
		}
		if rest == "" {
			continue
		}
		for _, pfx := range []string{"issue.components.", "issue.component.", "bugs.components.", "bugs.component."} {
			if strings.HasPrefix(rest, pfx) {
				alias := strings.TrimSpace(strings.TrimPrefix(rest, pfx))
				n, err := strconv.ParseInt(val, 10, 64)
				if err != nil || n <= 0 || alias == "" {
					return fmt.Errorf("invalid git config %s=%q: component alias ID must be a positive integer", key, val)
				}
				dst.Issue.Components[alias] = n
				rest = ""
				break
			}
		}
		if rest == "" {
			continue
		}
		for _, pfx := range []string{"issue.hotlists.", "issue.hotlist.", "bugs.hotlists.", "bugs.hotlist."} {
			if strings.HasPrefix(rest, pfx) {
				alias := strings.TrimSpace(strings.TrimPrefix(rest, pfx))
				n, err := strconv.ParseInt(val, 10, 64)
				if err != nil || n <= 0 || alias == "" {
					return fmt.Errorf("invalid git config %s=%q: hotlist alias ID must be a positive integer", key, val)
				}
				dst.Issue.Hotlists[alias] = n
				rest = ""
				break
			}
		}
		if rest == "" {
			continue
		}

		section, subkey, found := strings.Cut(rest, ".")
		if !found {
			continue
		}
		normKey := normalizeGitConfigSubkey(subkey)

		switch strings.ToLower(section) {
		case "gerrit":
			switch normKey {
			case "host":
				dst.Gerrit.Host = val
			case "project":
				dst.Gerrit.Project = val
			case "remote":
				dst.Gerrit.Remote = val
			case "defaultbranch":
				dst.Gerrit.DefaultBranch = val
			case "scopelisttoproject":
				b, err := strconv.ParseBool(val)
				if err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.Gerrit.ScopeListToProject = cfgBoolPtr(b)
			case "submodulepolicy":
				if err := validateSubmodulePolicy(val); err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.Gerrit.SubmodulePolicy = val
			case "forbidtopics":
				b, err := strconv.ParseBool(val)
				if err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.Gerrit.ForbidTopics = b
			}

		case "ci":
			switch normKey {
			case "buildbuckethost":
				dst.CI.BuildbucketHost = val
			case "trybuckets":
				dst.CI.TryBuckets = splitCSV(val)
			case "providers":
				arr := splitCSV(val)
				if err := validateCIProviders(arr); err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.CI.Providers = arr
			case "hidetagfilters":
				dst.CI.HideTagFilters = splitCSV(val)
			case "skipretrytagfilters":
				dst.CI.SkipRetryTagFilters = splitCSV(val)
			case "preferredlogs":
				dst.CI.PreferredLogs = splitCSV(val)
			case "includesummarymarkdown":
				b, err := strconv.ParseBool(val)
				if err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.CI.IncludeSummaryMarkdown = cfgBoolPtr(b)
			case "localpresubmithint":
				dst.CI.LocalPresubmitHint = val
			}

		case "issue", "bugs":
			switch normKey {
			case "system":
				dst.Issue.System = val
			case "prefix":
				dst.Issue.Prefix = val
			case "host":
				dst.Issue.Host = val
			case "defaultcomponent", "defaultcomponentid":
				n, err := strconv.ParseInt(val, 10, 64)
				if err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.Issue.DefaultComponent = n
			case "quotaproject":
				dst.Issue.QuotaProject = val
			case "trailerformat", "requiretrailer":
				dst.Issue.TrailerFormat = val
			case "useownerscomponents":
				b, err := strconv.ParseBool(val)
				if err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.Issue.UseOwnersComponents = cfgBoolPtr(b)
			}

		case "worktree":
			switch normKey {
			case "slotprefix":
				dst.Worktree.SlotPrefix = val
			case "warmupdriver":
				if err := validateWarmupDriver(val); err != nil {
					return fmt.Errorf("invalid git config %s=%q: %w", key, val, err)
				}
				dst.Worktree.WarmupDriver = val
			}

		case "oncall":
			switch normKey {
			case "schedulefile":
				dst.Oncall.ScheduleFile = val
			}
		}
	}
	return nil
}

func lookupIssueAliasID(aliases map[string]int64, name string) (int64, bool) {
	if len(aliases) == 0 || name == "" {
		return 0, false
	}
	if id, ok := aliases[name]; ok && id > 0 {
		return id, true
	}
	for k, id := range aliases {
		if id > 0 && strings.EqualFold(k, name) {
			return id, true
		}
	}
	return 0, false
}

// ExpandIssueLabel resolves any configured [issue.hotlists] or [issue.components]
// alias in a label string (such as "hotlist:triage", "-hotlist:triage", or
// "component:rpc") into its numeric ID form before label parsing.
func (c *ProjectConfig) ExpandIssueLabel(label string) string {
	if c == nil {
		return label
	}
	clean := strings.TrimSpace(label)
	negPrefix := ""
	if strings.HasPrefix(clean, "-") || strings.HasPrefix(clean, "!") {
		negPrefix = clean[:1]
		clean = strings.TrimSpace(clean[1:])
	}
	lower := strings.ToLower(clean)
	if strings.HasPrefix(lower, "hotlist:") {
		raw := strings.TrimSpace(clean[len("hotlist:"):])
		if id, ok := lookupIssueAliasID(c.Issue.Hotlists, raw); ok {
			return fmt.Sprintf("%shotlist:%d", negPrefix, id)
		}
	} else if strings.HasPrefix(lower, "component:") {
		raw := strings.TrimSpace(clean[len("component:"):])
		if id, ok := lookupIssueAliasID(c.Issue.Components, raw); ok {
			return fmt.Sprintf("%scomponent:%d", negPrefix, id)
		}
	}
	return label
}

const defaultBugTrailerFormat = "Bug: b/{id}"

// ResolveBugTrailerFormat returns the configured bug trailer template from
// `.ghish.toml` / `git config` (`issue.trailer_format`, then `issue.prefix`),
// falling back to `"Bug: b/{id}"` when unset.
func ResolveBugTrailerFormat(ctx context.Context, cfg *Config) (string, error) {
	pcfg, err := cfg.LoadProjectConfig(ctx)
	if err != nil {
		return "", err
	}
	if pcfg != nil {
		if fmtStr := strings.TrimSpace(pcfg.Issue.TrailerFormat); fmtStr != "" {
			return fmtStr, nil
		}
		if prefix := strings.TrimSpace(pcfg.Issue.Prefix); prefix != "" {
			return fmt.Sprintf("Bug: %s{id}", prefix), nil
		}
	}
	return defaultBugTrailerFormat, nil
}
