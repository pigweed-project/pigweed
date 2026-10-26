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
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
)

// GitRunner executes git commands.
type GitRunner interface {
	Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error
}

// RealGitRunner implements GitRunner using git subprocesses spawned via
// ExecCommandContext.
type RealGitRunner struct {
	Dir string
}

const gitEnvKey contextKey = "gitEnv"

func withGitEnv(ctx context.Context, env ...string) context.Context {
	if ctx == nil {
		ctx = context.Background()
	}
	return context.WithValue(ctx, gitEnvKey, slices.Concat(gitEnvFromContext(ctx), env))
}

func withGitConfigEnv(ctx context.Context, kv ...string) context.Context {
	var count int
	for _, entry := range slices.Concat(os.Environ(), gitEnvFromContext(ctx)) {
		if v, ok := strings.CutPrefix(entry, "GIT_CONFIG_COUNT="); ok {
			if n, err := strconv.Atoi(v); err == nil && n >= 0 {
				count = n
			}
		}
	}
	var env []string
	for i := 0; i+1 < len(kv); i += 2 {
		env = append(env,
			fmt.Sprintf("GIT_CONFIG_KEY_%d=%s", count, kv[i]),
			fmt.Sprintf("GIT_CONFIG_VALUE_%d=%s", count, kv[i+1]),
		)
		count++
	}
	existing := slices.DeleteFunc(slices.Clone(gitEnvFromContext(ctx)), func(e string) bool {
		return strings.HasPrefix(e, "GIT_CONFIG_COUNT=")
	})
	newEnv := append([]string{fmt.Sprintf("GIT_CONFIG_COUNT=%d", count)}, existing...)
	newEnv = append(newEnv, env...)
	if ctx == nil {
		ctx = context.Background()
	}
	return context.WithValue(ctx, gitEnvKey, newEnv)
}

func gitEnvFromContext(ctx context.Context) []string {
	if ctx == nil {
		return nil
	}
	env, _ := ctx.Value(gitEnvKey).([]string)
	return env
}

func (r *RealGitRunner) buildEnv(ctx context.Context, args []string) []string {
	baseEnv := os.Environ()
	if r != nil && r.Dir != "" {
		baseEnv = scrubRepoLocalGitEnv(baseEnv)
	}
	extraEnv := []string{"GH_ISH_ACTIVE=1"}
	if isReadOnlyGitQuery(args) {
		extraEnv = append(extraEnv, "GIT_CONFIG_NOSYSTEM=1", "GIT_OPTIONAL_LOCKS=0")
	}
	return slices.Concat(baseEnv, extraEnv, gitEnvFromContext(ctx))
}

func (r *RealGitRunner) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	if ctx == nil {
		ctx = context.Background()
	}
	cmd := ExecCommandContext(ctx, "git", args...)
	if r != nil && r.Dir != "" {
		cmd.Dir = r.Dir
	}
	cmd.Env = r.buildEnv(ctx, args)
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	return runBoundingOrphanedPipes(cmd)
}

func asRealGitRunner(git GitRunner) (*RealGitRunner, bool) {
	switch g := git.(type) {
	case *RealGitRunner:
		return g, true
	case *defaultGitClient:
		if r, ok := g.runner.(*RealGitRunner); ok {
			return r, true
		}
	}
	return nil, false
}

const prefetchedGitConfigRegexp = `^(ghish|remote|branch|user|init|http|extensions)\.`

func isRepoLocalGitEnvKey(key string) bool {
	switch key {
	case "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
		"GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
		"GIT_COMMON_DIR", "GIT_PREFIX",
		"GIT_CONFIG", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT":
		return true
	}
	return false
}

// ScrubRepoLocalGitEnv returns a copy of env with repository-local Git
// environment overrides removed so git subprocesses executing in an explicit
// directory operate on that directory rather than an inherited outer repo
// (such as during `git rebase -x` or git hook execution).
func ScrubRepoLocalGitEnv(env []string) []string {
	return scrubRepoLocalGitEnv(env)
}

func scrubRepoLocalGitEnv(env []string) []string {
	filtered := make([]string, 0, len(env))
	for _, kv := range env {
		k, _, _ := strings.Cut(kv, "=")
		if isRepoLocalGitEnvKey(k) {
			continue
		}
		filtered = append(filtered, kv)
	}
	return filtered
}

// StackCommit represents a single commit in a local stack ahead of a target branch.
type StackCommit struct {
	Hash     string
	Subject  string
	Body     string
	ChangeID string
}

// GitClient defines high-level Git queries and operations on top of GitRunner.
type GitClient interface {
	GitRunner
	CurrentBranch(ctx context.Context) (string, error)
	HeadCommitMessage(ctx context.Context) (string, error)
	CommitMessage(ctx context.Context, ref ...string) (string, error)
	ConfigGet(ctx context.Context, key string) (string, error)
	RevParse(ctx context.Context, args ...string) (string, error)
	VerifyRef(ctx context.Context, ref string) (bool, error)
	GitDir(ctx context.Context) (string, error)
	CountCommitsAhead(ctx context.Context, branch string) (int, error)
	StackCommits(ctx context.Context, branch string) ([]StackCommit, error)
	Fetch(ctx context.Context, remote, ref string, stdout, stderr io.Writer) error
	Checkout(ctx context.Context, ref string, stdout, stderr io.Writer) error
	CherryPick(ctx context.Context, ref string, stdout, stderr io.Writer) error
	// HasStagedChanges reports whether the Git index differs from HEAD.
	HasStagedChanges(ctx context.Context) (bool, error)
	// CommitNewAll commits only staged index changes if HasStagedChanges is
	// true (leaving unstaged edits untouched), or passes -a to stage modified
	// tracked files when nothing is staged yet, reporting whether -a was used.
	CommitNewAll(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) (autoStaged bool, err error)
	// CommitAmendMessage rewrites HEAD's commit message via a temporary file
	// without altering HEAD's tree or staged index changes.
	CommitAmendMessage(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) error
	// CommitAmendNoEdit runs commit-msg hooks on HEAD without altering the commit
	// tree or staged index entries.
	CommitAmendNoEdit(ctx context.Context, stdout, stderr io.Writer) error
	// UpdateHeadRef atomically moves HEAD to targetSHA only if HEAD currently
	// resolves to expectedCurrentSHA, leaving index and working tree untouched.
	UpdateHeadRef(ctx context.Context, targetSHA, expectedCurrentSHA, reason string) error
	IsHeadRemoteTip(ctx context.Context) (bool, []string, error)
	HeadAuthorEmail(ctx context.Context) (string, error)
	UserEmail(ctx context.Context) (string, error)
	// CheckAmendAllowed verifies that HEAD is safe to amend locally (not on a
	// remote tracking tip, merge commit, or active sequencer operation).
	CheckAmendAllowed(ctx context.Context) error
	ResolveRemote(ctx context.Context) string
	ResolveDefaultBranch(ctx context.Context) string
}

type configCacheEntry struct {
	val string
	err error
}

// defaultGitClient implements GitClient by executing commands via an underlying GitRunner.
type defaultGitClient struct {
	runner                   GitRunner
	mu                       sync.Mutex
	gitDir                   string
	resolvedRemote           string
	resolvedDefaultBranch    string
	currentBranch            string
	currentBranchCached      bool
	configCache              map[string]configCacheEntry
	prefetchedCommonSections bool
	prefetchedRemotes        []string
	// prefetchedEntries are the `git config --get-regexp` results that
	// seeded configCache, reused by LoadProjectConfig so the pass runs once.
	prefetchedEntries []gitConfigEntry
	// prefetchAttempted records that the lazy prefetch already ran (even if
	// it failed) so a failing git does not retry it on every lookup.
	prefetchAttempted bool
	// defaultBranchProbe holds the output of a for-each-ref over
	// defaultBranchRefCandidates(resolvedRemote) that already ran as part
	// of probeRemoteBranch, so ResolveDefaultBranch's fallback reuses it
	// instead of listing the same refs again. Valid when
	// defaultBranchProbeKnown.
	defaultBranchProbe      string
	defaultBranchProbeKnown bool
	hasProjCfgCached        bool
	hasProjCfgCWD           string
	hasProjCfgVal           bool
}

func isGitConfigNoMatchError(err error) bool {
	var exitErr *exec.ExitError
	return errors.As(err, &exitErr) && exitErr.ExitCode() == 1
}

func hasPrefetchedSectionPrefix(key string) bool {
	dot := strings.IndexByte(key, '.')
	if dot <= 0 {
		return false
	}
	switch strings.ToLower(key[:dot]) {
	case "ghish", "remote", "branch", "user", "init", "http", "extensions":
		return true
	default:
		return false
	}
}

func sequencerMarkerExists(ctx context.Context, runner GitRunner, gitDir, name string) bool {
	_, err := os.Stat(filepath.Join(gitDir, name))
	return err == nil
}

func splitGitSubcommand(args []string) (string, []string) {
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if arg == "--" {
			if i+1 < len(args) {
				return args[i+1], args[i+2:]
			}
			return "", nil
		}
		if !strings.HasPrefix(arg, "-") {
			return arg, args[i+1:]
		}
		switch arg {
		case "-C", "-c", "--git-dir", "--work-tree", "--namespace", "--exec-path", "--config-env":
			i++
		}
	}
	return "", nil
}

// IsReadOnlyGitQuery reports whether git args invoke a read-only plumbing or
// inspection subcommand that neither mutates repository state nor requires
// index locks or system-level git config.
func IsReadOnlyGitQuery(args []string) bool {
	return isReadOnlyGitQuery(args)
}

func isReadOnlyGitQuery(args []string) bool {
	sub, subArgs := splitGitSubcommand(args)
	switch sub {
	case "rev-parse", "log", "status", "diff", "diff-index", "rev-list", "ls-files",
		"show", "cat-file", "for-each-ref", "name-rev", "describe", "shortlog", "diff-tree", "show-ref",
		// merge-tree --write-tree only writes unreferenced objects; it never
		// touches refs, HEAD, or config cached by the client.
		"merge-tree":
		return true
	case "worktree":
		return len(subArgs) > 0 && subArgs[0] == "list"
	case "config":
		if len(subArgs) == 1 && !strings.HasPrefix(subArgs[0], "-") {
			return true
		}
		for _, a := range subArgs {
			switch a {
			case "--get", "--get-all", "--get-regexp", "--list", "-l", "--get-urlmatch":
				return true
			}
		}
		return false
	case "branch":
		if len(subArgs) == 0 {
			return true
		}
		switch subArgs[0] {
		case "--show-current", "--list", "-l", "-a", "--all", "-r", "--remotes",
			"--contains", "--no-contains", "--merged", "--no-merged", "-v", "-vv":
			return true
		}
		return false
	case "remote":
		for len(subArgs) > 0 && (subArgs[0] == "-v" || subArgs[0] == "--verbose") {
			subArgs = subArgs[1:]
		}
		if len(subArgs) == 0 {
			return true
		}
		switch subArgs[0] {
		case "get-url", "show":
			return true
		}
		return false
	case "symbolic-ref":
		if slices.Contains(subArgs, "-d") || slices.Contains(subArgs, "--delete") {
			return false
		}
		nonFlags := 0
		for _, a := range subArgs {
			if !strings.HasPrefix(a, "-") {
				nonFlags++
			}
		}
		return nonFlags <= 1
	default:
		return false
	}
}

func (c *defaultGitClient) invalidateCacheLocked() {
	c.gitDir = ""
	c.resolvedRemote = ""
	c.resolvedDefaultBranch = ""
	c.currentBranch = ""
	c.currentBranchCached = false
	c.configCache = nil
	c.prefetchedCommonSections = false
	c.prefetchedRemotes = nil
	c.prefetchedEntries = nil
	c.prefetchAttempted = false
	c.defaultBranchProbe = ""
	c.defaultBranchProbeKnown = false
	c.hasProjCfgCached = false
	c.hasProjCfgCWD = ""
	c.hasProjCfgVal = false
}

func (c *defaultGitClient) invalidateCache() {
	c.mu.Lock()
	c.invalidateCacheLocked()
	c.mu.Unlock()
}

var errGitConfigKeyUnset = errors.New("git config key not set")

// gitConfigEntry is one key/value pair reported by `git config --get-regexp`.
type gitConfigEntry struct {
	key string
	val string
}

// parseGitConfigLines parses newline-delimited `git config --get-regexp`
// output ("key value" per line). Values containing newlines cannot be
// represented; real git prefetches use parseGitConfigNUL instead.
func parseGitConfigLines(raw string) []gitConfigEntry {
	var entries []gitConfigEntry
	for _, rawLine := range strings.Split(raw, "\n") {
		line := strings.TrimSpace(rawLine)
		if line == "" {
			continue
		}
		key, val, _ := strings.Cut(line, " ")
		key = strings.TrimSpace(key)
		if key == "" {
			continue
		}
		entries = append(entries, gitConfigEntry{key: key, val: strings.TrimSpace(val)})
	}
	return entries
}

// parseGitConfigNUL parses `git config -z --get-regexp` output: records are
// NUL-terminated, and the key is separated from its value by the first
// newline (a valueless key has no newline). Values may themselves contain
// newlines, which the line format would misparse as extra keys.
func parseGitConfigNUL(raw string) []gitConfigEntry {
	var entries []gitConfigEntry
	for _, rec := range strings.Split(raw, "\x00") {
		if rec == "" {
			continue
		}
		key, val, _ := strings.Cut(rec, "\n")
		key = strings.TrimSpace(key)
		if key == "" {
			continue
		}
		entries = append(entries, gitConfigEntry{key: key, val: strings.TrimSpace(val)})
	}
	return entries
}

// canonicalGitConfigKey normalizes key the way git compares config names:
// the section and variable names are case-insensitive, but the subsection
// (between the first and last dot, e.g. the branch in branch.<name>.remote)
// is case-sensitive.
func canonicalGitConfigKey(key string) string {
	first := strings.IndexByte(key, '.')
	last := strings.LastIndexByte(key, '.')
	if first < 0 || first == last {
		return strings.ToLower(key)
	}
	return strings.ToLower(key[:first]) + key[first:last] + strings.ToLower(key[last:])
}

// seedPrefetchedGitConfig seeds the cache from line-format
// `git config --get-regexp` output.
func (c *defaultGitClient) seedPrefetchedGitConfig(rawOutput string) {
	c.seedPrefetchedGitConfigEntries(parseGitConfigLines(rawOutput))
}

// seedPrefetchedGitConfigEntries seeds the cache from a completed prefetch.
func (c *defaultGitClient) seedPrefetchedGitConfigEntries(entries []gitConfigEntry) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.seedPrefetchedGitConfigLocked(entries)
}

func (c *defaultGitClient) seedPrefetchedGitConfigLocked(entries []gitConfigEntry) {
	if c.configCache == nil {
		c.configCache = make(map[string]configCacheEntry)
	}
	remoteSeen := make(map[string]bool)
	for _, r := range c.prefetchedRemotes {
		remoteSeen[r] = true
	}
	for _, e := range entries {
		key := canonicalGitConfigKey(e.key)
		c.configCache[key] = configCacheEntry{val: e.val}
		if strings.HasPrefix(key, "remote.") {
			rest := key[len("remote."):]
			if lastDot := strings.LastIndexByte(rest, '.'); lastDot > 0 {
				remName := rest[:lastDot]
				if isValidRemoteName(remName) && !remoteSeen[remName] {
					remoteSeen[remName] = true
					c.prefetchedRemotes = append(c.prefetchedRemotes, remName)
				}
			}
		}
	}
	c.prefetchedCommonSections = true
	c.prefetchAttempted = true
	c.prefetchedEntries = slices.Clone(entries)
}

// ensurePrefetchedLocked runs the shared `git config -z --get-regexp`
// prefetch (prefetchedGitConfigRegexp) the first time a prefetched-section
// key is needed, so remote/branch/user lookups issued before
// LoadProjectConfig (e.g. GetProfile or auth-mode detection) cost one
// subprocess in total instead of one per key. It returns the prefetched
// entries and whether the cache is seeded. Only real git runners prefetch;
// mock runners keep per-key lookups.
//
// Best-effort contract: if the prefetch fails, lookups fall back to per-key
// `git config --get` and the prefetch is not retried until the cache is
// invalidated.
func (c *defaultGitClient) ensurePrefetchedLocked(ctx context.Context) ([]gitConfigEntry, bool) {
	if c.prefetchedCommonSections {
		return c.prefetchedEntries, true
	}
	if c.prefetchAttempted {
		return nil, false
	}
	if _, isReal := c.runner.(*RealGitRunner); !isReal {
		return nil, false
	}
	c.prefetchAttempted = true
	var out, stderr bytes.Buffer
	err := c.runner.Run(ctx, &out, &stderr, "config", "-z", "--get-regexp", prefetchedGitConfigRegexp)
	if err != nil && !isGitConfigNoMatchError(err) {
		// OK: the prefetch is an optimization; per-key lookups still work.
		slog.Debug("git config prefetch failed; falling back to per-key lookups",
			"err", err, "stderr", strings.TrimSpace(stderr.String()))
		return nil, false
	}
	c.seedPrefetchedGitConfigLocked(parseGitConfigNUL(out.String()))
	return c.prefetchedEntries, true
}

// ensurePrefetched is the locking wrapper of ensurePrefetchedLocked.
func (c *defaultGitClient) ensurePrefetched(ctx context.Context) ([]gitConfigEntry, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.ensurePrefetchedLocked(ctx)
}

func (c *defaultGitClient) hasProjectConfigFile(cwd string) bool {
	if cwd == "" {
		return false
	}
	c.mu.Lock()
	if c.hasProjCfgCached && c.hasProjCfgCWD == cwd {
		val := c.hasProjCfgVal
		c.mu.Unlock()
		return val
	}
	c.mu.Unlock()
	val := hasProjectConfigFileOnDisk(cwd)
	c.mu.Lock()
	c.hasProjCfgCached = true
	c.hasProjCfgCWD = cwd
	c.hasProjCfgVal = val
	c.mu.Unlock()
	return val
}

// applyGitConfigToProjectConfig applies the repository's `ghish.*` git config
// overrides onto cfg, returning true when at least one `ghish.*` entry was
// present. It reuses the client's shared NUL-delimited config prefetch when
// running real git, and otherwise lists `ghish.*` keys itself.
//
// Best-effort contract: when the fallback listing fails (no ghish.* keys, or
// no repository), it returns false, nil so callers can fall through to other
// tiers; invalid override values still fail.
func (c *defaultGitClient) applyGitConfigToProjectConfig(ctx context.Context, cfg *ProjectConfig) (bool, error) {
	entries, seeded := c.ensurePrefetched(ctx)
	if !seeded {
		out, err := c.runOutput(ctx, "config", "--get-regexp", `^ghish\.`)
		if err != nil {
			slog.Debug("listing ghish.* git config failed", "err", err)
			return false, nil
		}
		entries = parseGitConfigLines(out)
	}
	if !slices.ContainsFunc(entries, func(e gitConfigEntry) bool { return strings.HasPrefix(e.key, "ghish.") }) {
		return false, nil
	}
	if err := applyGitConfigEntries(entries, cfg); err != nil {
		return false, err
	}
	return true, nil
}

func applyGitConfigFromRunner(ctx context.Context, git GitRunner, cfg *ProjectConfig) error {
	if git == nil {
		return nil
	}
	_, isReal := asRealGitRunner(git)
	if dc, ok := git.(*defaultGitClient); ok && isReal {
		if entries, seeded := dc.ensurePrefetched(ctx); seeded {
			return applyGitConfigEntries(entries, cfg)
		}
	}
	var out bytes.Buffer
	args := []string{"config", "--get-regexp", `^ghish\.`}
	if isReal {
		args = []string{"config", "-z", "--get-regexp", prefetchedGitConfigRegexp}
	}
	runErr := git.Run(ctx, &out, io.Discard, args...)
	var entries []gitConfigEntry
	if isReal {
		entries = parseGitConfigNUL(out.String())
	} else {
		entries = parseGitConfigLines(out.String())
	}
	if isReal && (runErr == nil || isGitConfigNoMatchError(runErr)) {
		if dc, ok := git.(*defaultGitClient); ok {
			dc.seedPrefetchedGitConfigEntries(entries)
		}
	}
	if runErr == nil {
		return applyGitConfigEntries(entries, cfg)
	}
	return nil
}

func (c *defaultGitClient) seedProjectConfigDefaults(projCfg *ProjectConfig) {
	if projCfg == nil {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.configCache == nil {
		c.configCache = make(map[string]configCacheEntry)
	}
	if _, ok := c.configCache["ghish.gerrit.remote"]; !ok && strings.TrimSpace(projCfg.Gerrit.Remote) == "" {
		c.configCache["ghish.gerrit.remote"] = configCacheEntry{err: errGitConfigKeyUnset}
	}
	if _, ok := c.configCache["ghish.gerrit.defaultbranch"]; !ok && strings.TrimSpace(projCfg.Gerrit.DefaultBranch) == "" {
		c.configCache["ghish.gerrit.defaultbranch"] = configCacheEntry{err: errGitConfigKeyUnset}
	}
}

func (c *defaultGitClient) configuredDefaultBranch(ctx context.Context) string {
	if v, err := c.configGetCached(ctx, "ghish.gerrit.defaultbranch"); err == nil {
		v = strings.TrimSpace(v)
		if v != "" && !strings.Contains(v, "://") {
			return v
		}
	}
	return ""
}

// NewGitClient wraps runner into a GitClient. If runner is nil, DefaultGitRunner is used.
func NewGitClient(runner GitRunner) GitClient {
	if runner == nil {
		runner = DefaultGitRunner
	}
	if gc, ok := runner.(GitClient); ok {
		return gc
	}
	return &defaultGitClient{runner: runner}
}

// NewGitClientInDir returns a GitClient configured to execute git commands in dir.
func NewGitClientInDir(dir string) GitClient {
	return NewGitClient(&RealGitRunner{Dir: dir})
}

// sharedGitClientForRunner returns the GitClient of the command Config carried
// by ctx when that Config runs git through runner (nil means
// DefaultGitRunner), so helpers that only receive a runner still share the
// Config's config prefetch and caches instead of re-running them on a fresh
// client. Otherwise it returns NewGitClient(runner).
func sharedGitClientForRunner(ctx context.Context, runner GitRunner) GitClient {
	if runner == nil {
		runner = DefaultGitRunner
	}
	if ctx != nil {
		if cfg, ok := ctx.Value(configKey).(*Config); ok && cfg != nil && cfg.Git != nil && cfg.Git == runner {
			return cfg.GitClient()
		}
	}
	return NewGitClient(runner)
}

// GitClient returns a GitClient for the configuration, falling back to DefaultGitRunner if unconfigured.
func (c *Config) GitClient() GitClient {
	if c == nil {
		return NewGitClient(DefaultGitRunner)
	}
	runner := c.Git
	if runner == nil {
		runner = DefaultGitRunner
	}
	configMu.Lock()
	defer configMu.Unlock()
	if c.gitClient != nil && c.gitClientRunner == runner {
		return c.gitClient
	}
	c.gitClient = NewGitClient(runner)
	c.gitClientRunner = runner
	return c.gitClient
}

func (c *defaultGitClient) runLocked(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	if stdout == nil {
		stdout = io.Discard
	}
	if stderr == nil {
		stderr = io.Discard
	}
	readOnly := isReadOnlyGitQuery(args)
	if !readOnly {
		c.invalidateCacheLocked()
	}
	err := c.runner.Run(ctx, stdout, stderr, args...)
	if !readOnly {
		c.invalidateCacheLocked()
	}
	return err
}

func (c *defaultGitClient) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	if stdout == nil {
		stdout = io.Discard
	}
	if stderr == nil {
		stderr = io.Discard
	}
	readOnly := isReadOnlyGitQuery(args)
	if !readOnly {
		c.invalidateCache()
	}
	err := c.runner.Run(ctx, stdout, stderr, args...)
	if !readOnly {
		c.invalidateCache()
	}
	return err
}

func (c *defaultGitClient) runOutputLocked(ctx context.Context, args ...string) (string, error) {
	if len(args) == 0 {
		return "", fmt.Errorf("git command arguments cannot be empty")
	}
	var stdout, stderr bytes.Buffer
	err := c.runLocked(ctx, &stdout, &stderr, args...)
	if err != nil {
		errStr := strings.TrimSpace(stderr.String())
		if errStr == "" {
			errStr = strings.TrimSpace(stdout.String())
		}
		cmdStr := strings.Join(args, " ")
		if errStr != "" {
			return "", fmt.Errorf("git %s failed: %w: %s", cmdStr, err, errStr)
		}
		return "", fmt.Errorf("git %s failed: %w", cmdStr, err)
	}
	return strings.TrimSpace(stdout.String()), nil
}

func (c *defaultGitClient) runOutput(ctx context.Context, args ...string) (string, error) {
	if len(args) == 0 {
		return "", fmt.Errorf("git command arguments cannot be empty")
	}
	var stdout, stderr bytes.Buffer
	err := c.Run(ctx, &stdout, &stderr, args...)
	if err != nil {
		errStr := strings.TrimSpace(stderr.String())
		if errStr == "" {
			errStr = strings.TrimSpace(stdout.String())
		}
		cmdStr := strings.Join(args, " ")
		if errStr != "" {
			return "", fmt.Errorf("git %s failed: %w: %s", cmdStr, err, errStr)
		}
		return "", fmt.Errorf("git %s failed: %w", cmdStr, err)
	}
	return strings.TrimSpace(stdout.String()), nil
}

func isValidRemoteName(s string) bool {
	s = strings.TrimSpace(s)
	if s == "" || s == "." {
		return false
	}
	return !strings.Contains(s, "://") && !strings.Contains(s, "/") && !strings.ContainsAny(s, " \t\r\n")
}

// ResolveRemote resolves the Gerrit git remote name in priority order:
// 1. git config ghish.gerrit.remote
// 2. current branch tracking remote (branch.<cur>.remote or @{u} prefix)
// 3. "origin" if present in `git remote`
// 4. common Gerrit remotes ("goog", "aosp", "partner") or first remote in `git remote`
// 5. fallback "origin"
func (c *defaultGitClient) ResolveRemote(ctx context.Context) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.resolveRemoteLocked(ctx)
}

func (c *defaultGitClient) resolveRemoteLocked(ctx context.Context) string {
	if c.resolvedRemote != "" {
		return c.resolvedRemote
	}
	rem := c.resolveRemoteUncachedLocked(ctx)
	c.resolvedRemote = rem
	return rem
}

func (c *defaultGitClient) resolveRemoteUncachedLocked(ctx context.Context) string {
	// One `git config --get-regexp` pass answers every config question below
	// (ghish.gerrit.remote, branch.<cur>.remote, and the remote.* list).
	c.ensurePrefetchedLocked(ctx)
	if v, err := c.configGetLocked(ctx, "ghish.gerrit.remote"); err == nil && isValidRemoteName(v) {
		return strings.TrimSpace(v)
	}

	if curBranch, err := c.currentBranchLocked(ctx); err == nil {
		curBranch = strings.TrimSpace(curBranch)
		if curBranch != "" && curBranch != "HEAD" && !strings.Contains(curBranch, "://") {
			if r, err := c.configGetLocked(ctx, fmt.Sprintf("branch.%s.remote", curBranch)); err == nil && isValidRemoteName(r) {
				return strings.TrimSpace(r)
			}
		}
	}

	// With branch.* prefetched, the current branch's upstream remote is
	// exactly branch.<cur>.remote (checked above), and a detached HEAD has no
	// upstream, so `@{u}` can only add information without the prefetch.
	if !c.prefetchedCommonSections {
		if upstream, err := c.runOutputLocked(ctx, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"); err == nil {
			upstream = strings.TrimSpace(upstream)
			if !strings.Contains(upstream, "://") {
				if rem, _, ok := strings.Cut(upstream, "/"); ok && isValidRemoteName(rem) && rem != "refs" {
					return rem
				}
			}
		}
	}

	if c.prefetchedCommonSections && len(c.prefetchedRemotes) > 0 {
		remoteSet := make(map[string]bool, len(c.prefetchedRemotes))
		for _, r := range c.prefetchedRemotes {
			remoteSet[r] = true
		}
		for _, preferred := range []string{"origin", "goog", "aosp", "partner"} {
			if remoteSet[preferred] {
				return preferred
			}
		}
		return c.prefetchedRemotes[0]
	}

	if out, err := c.runOutputLocked(ctx, "remote"); err == nil && strings.TrimSpace(out) != "" {
		var remotes []string
		remoteSet := make(map[string]bool)
		for _, line := range strings.Split(out, "\n") {
			r := strings.TrimSpace(line)
			if isValidRemoteName(r) {
				remotes = append(remotes, r)
				remoteSet[r] = true
			}
		}
		if len(remotes) > 0 {
			for _, preferred := range []string{"origin", "goog", "aosp", "partner"} {
				if remoteSet[preferred] {
					return preferred
				}
			}
			return remotes[0]
		}
	}

	return "origin"
}

// ResolveDefaultBranch resolves the repository's default upstream branch in priority order:
// 1. git config ghish.gerrit.defaultbranch
// 2. current branch's tracked upstream merge target (branch.<cur>.merge when branch.<cur>.remote is set and target != cur)
// 3. git symbolic-ref refs/remotes/<remote>/HEAD
// 4. git config init.defaultBranch
// 5. refs/heads/main, refs/remotes/<remote>/main, and legacy default branch refs
// 6. fallback "main"
func (c *defaultGitClient) ResolveDefaultBranch(ctx context.Context) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.resolveDefaultBranchLocked(ctx)
}

func (c *defaultGitClient) resolveDefaultBranchLocked(ctx context.Context) string {
	if c.resolvedDefaultBranch != "" {
		return c.resolvedDefaultBranch
	}
	b := c.resolveDefaultBranchUncachedLocked(ctx)
	c.resolvedDefaultBranch = b
	return b
}

func (c *defaultGitClient) resolveDefaultBranchUncachedLocked(ctx context.Context) string {
	if v, err := c.configGetLocked(ctx, "ghish.gerrit.defaultbranch"); err == nil {
		v = strings.TrimSpace(v)
		if v != "" && !strings.Contains(v, "://") {
			return v
		}
	}

	if curBranch, err := c.currentBranchLocked(ctx); err == nil {
		curBranch = strings.TrimSpace(curBranch)
		if curBranch != "" && curBranch != "HEAD" && !strings.Contains(curBranch, "://") {
			if r, rErr := c.configGetLocked(ctx, fmt.Sprintf("branch.%s.remote", curBranch)); rErr == nil && isValidRemoteName(r) {
				if mergeRef, mErr := c.configGetLocked(ctx, fmt.Sprintf("branch.%s.merge", curBranch)); mErr == nil {
					mergeBranch := strings.TrimPrefix(strings.TrimSpace(mergeRef), "refs/heads/")
					if mergeBranch != "" && mergeBranch != "HEAD" && mergeBranch != curBranch && !strings.Contains(mergeBranch, "://") {
						return mergeBranch
					}
				}
			}
		}
	}

	remote := c.resolveRemoteLocked(ctx)
	symRefTarget := fmt.Sprintf("refs/remotes/%s/HEAD", remote)
	if sym, err := c.runOutputLocked(ctx, "symbolic-ref", symRefTarget); err == nil {
		sym = strings.TrimSpace(sym)
		prefix := fmt.Sprintf("refs/remotes/%s/", remote)
		if strings.HasPrefix(sym, prefix) {
			if b := strings.TrimPrefix(sym, prefix); b != "" && b != "HEAD" {
				return b
			}
		}
	}

	if v, err := c.configGetLocked(ctx, "init.defaultBranch"); err == nil {
		v = strings.TrimSpace(v)
		if v != "" && !strings.Contains(v, "://") {
			return v
		}
	}

	candidates := defaultBranchRefCandidates(remote)
	refs := make([]string, 0, len(candidates))
	for _, cand := range candidates {
		refs = append(refs, cand.ref)
	}
	// One for-each-ref answers every candidate; it lists only refs that
	// resolve, like the `rev-parse --verify` probes it replaces.
	// probeRemoteBranch may already have listed them in its own pass.
	out := c.defaultBranchProbe
	if !c.defaultBranchProbeKnown {
		var err error
		out, err = c.runOutputLocked(ctx, append([]string{"for-each-ref", "--format=%(refname)"}, refs...)...)
		if err != nil {
			return "main"
		}
	}
	if b, ok := pickDefaultBranchCandidate(candidates, out); ok {
		return b
	}
	return "main"
}

// revParseDWIMRefs returns the full refnames `git rev-parse --verify <name>`
// tries for a short ref name, in its order (git's ref_rev_parse_rules), less
// the leading "<name>" rule, which only matches pseudo-refs at the top of the
// git directory (e.g. FETCH_HEAD) and which for-each-ref cannot list.
func revParseDWIMRefs(name string) []string {
	return []string{
		"refs/" + name,
		"refs/tags/" + name,
		"refs/heads/" + name,
		"refs/remotes/" + name,
		"refs/remotes/" + name + "/HEAD",
	}
}

// parseRefnameLines returns the set of refnames in for-each-ref
// `--format=%(refname)` output (one per line).
func parseRefnameLines(out string) map[string]bool {
	present := make(map[string]bool)
	for _, line := range strings.Split(out, "\n") {
		if ref := strings.TrimSpace(line); ref != "" {
			present[ref] = true
		}
	}
	return present
}

// runsRealGit reports whether the client runs the real git binary (as
// opposed to a scripted test runner).
func (c *defaultGitClient) runsRealGit() bool {
	_, isReal := c.runner.(*RealGitRunner)
	return isReal
}

// probeRemoteBranch reports whether <remote>/<branch> resolves, as
// `git rev-parse --verify <remote>/<branch>` (VerifyRef) would report it,
// using one for-each-ref over the refnames rev-parse would try. While the
// default branch is unresolved, the same call also lists ResolveDefaultBranch's
// fallback candidates, so a caller that falls back to the default branch
// (resolvePushBranch) pays one subprocess for both probes instead of two.
// Exact refname matching neutralizes for-each-ref's prefix and glob
// matching, as in pickDefaultBranchCandidate.
func (c *defaultGitClient) probeRemoteBranch(ctx context.Context, remote, branch string) (bool, error) {
	remote, branch = strings.TrimSpace(remote), strings.TrimSpace(branch)
	if remote == "" || branch == "" {
		return false, fmt.Errorf("remote and branch cannot be empty")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	dwim := revParseDWIMRefs(remote + "/" + branch)
	patterns := slices.Clone(dwim)
	withDefaults := c.resolvedDefaultBranch == "" && !c.defaultBranchProbeKnown
	if withDefaults {
		for _, cand := range defaultBranchRefCandidates(c.resolveRemoteLocked(ctx)) {
			patterns = append(patterns, cand.ref)
		}
	}
	out, err := c.runOutputLocked(ctx, append([]string{"for-each-ref", "--format=%(refname)"}, patterns...)...)
	if err != nil {
		return false, err
	}
	if withDefaults {
		c.defaultBranchProbe = out
		c.defaultBranchProbeKnown = true
	}
	present := parseRefnameLines(out)
	for _, ref := range dwim {
		if present[ref] {
			return true, nil
		}
	}
	return false, nil
}

// defaultBranchRefCandidate is a ref probed by ResolveDefaultBranch and the
// branch name it implies.
type defaultBranchRefCandidate struct {
	ref    string
	branch string
}

// defaultBranchRefCandidates returns the fallback default-branch refs for
// remote in priority order. Glob characters in a remote name would widen the
// for-each-ref patterns, but pickDefaultBranchCandidate only accepts exact
// refname matches, which neutralizes them.
func defaultBranchRefCandidates(remote string) []defaultBranchRefCandidate {
	return []defaultBranchRefCandidate{
		{ref: "refs/heads/main", branch: "main"},
		{ref: fmt.Sprintf("refs/remotes/%s/main", remote), branch: "main"},
		{ref: fmt.Sprintf("refs/remotes/%s/master", remote), branch: "master"}, // inclusive-language: ignore
		{ref: "refs/heads/master", branch: "master"},                           // inclusive-language: ignore
	}
}

// pickDefaultBranchCandidate returns the branch of the highest-priority
// candidate whose ref appears verbatim in forEachRefOut (one refname per
// line). for-each-ref patterns also match refs nested below a pattern (e.g.
// refs/heads/main/x), so only exact refname matches count.
func pickDefaultBranchCandidate(candidates []defaultBranchRefCandidate, forEachRefOut string) (string, bool) {
	present := parseRefnameLines(forEachRefOut)
	for _, cand := range candidates {
		if present[cand.ref] {
			return cand.branch, true
		}
	}
	return "", false
}

func (c *defaultGitClient) currentBranchLocked(ctx context.Context) (string, error) {
	if c.currentBranchCached {
		return c.currentBranch, nil
	}
	b, err := c.runOutputLocked(ctx, "branch", "--show-current")
	if err != nil {
		return "", err
	}
	c.currentBranch = b
	c.currentBranchCached = true
	return b, nil
}

func (c *defaultGitClient) CurrentBranch(ctx context.Context) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.currentBranchLocked(ctx)
}

func (c *defaultGitClient) HeadCommitMessage(ctx context.Context) (string, error) {
	return c.CommitMessage(ctx)
}

func (c *defaultGitClient) CommitMessage(ctx context.Context, ref ...string) (string, error) {
	if len(ref) > 1 {
		return "", fmt.Errorf("git log -1 accepts at most one ref, got %d", len(ref))
	}
	args := []string{"log", "-1", "--format=%B"}
	if len(ref) == 1 {
		trimmed := strings.TrimSpace(ref[0])
		if trimmed == "" {
			return "", fmt.Errorf("commit ref cannot be empty")
		}
		args = append(args, trimmed)
	}
	return c.runOutput(ctx, args...)
}

func (c *defaultGitClient) configGetLocked(ctx context.Context, key string) (string, error) {
	key = strings.TrimSpace(key)
	if key == "" {
		return "", fmt.Errorf("git config key cannot be empty")
	}
	cacheKey := canonicalGitConfigKey(key)
	if entry, ok := c.configCache[cacheKey]; ok {
		return entry.val, entry.err
	}
	if hasPrefetchedSectionPrefix(key) && !c.prefetchedCommonSections {
		if _, ok := c.ensurePrefetchedLocked(ctx); ok {
			if entry, ok := c.configCache[cacheKey]; ok {
				return entry.val, entry.err
			}
		}
	}
	if c.prefetchedCommonSections && hasPrefetchedSectionPrefix(key) {
		err := fmt.Errorf("git config --get %s: key not found in prefetched config", key)
		if c.configCache == nil {
			c.configCache = make(map[string]configCacheEntry)
		}
		c.configCache[cacheKey] = configCacheEntry{err: err}
		return "", err
	}
	val, err := c.runOutputLocked(ctx, "config", "--get", key)
	if c.configCache == nil {
		c.configCache = make(map[string]configCacheEntry)
	}
	c.configCache[cacheKey] = configCacheEntry{val: val, err: err}
	return val, err
}

func (c *defaultGitClient) configGetCached(ctx context.Context, key string) (string, error) {
	key = strings.TrimSpace(key)
	if key == "" {
		return "", fmt.Errorf("git config key cannot be empty")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.configGetLocked(ctx, key)
}

func (c *defaultGitClient) ConfigGet(ctx context.Context, key string) (string, error) {
	return c.configGetCached(ctx, key)
}

func (c *defaultGitClient) RevParse(ctx context.Context, args ...string) (string, error) {
	if len(args) == 0 {
		return "", fmt.Errorf("git rev-parse requires at least one argument")
	}
	for i, arg := range args {
		if strings.TrimSpace(arg) == "" {
			return "", fmt.Errorf("git rev-parse argument %d cannot be empty", i)
		}
	}
	return c.runOutput(ctx, append([]string{"rev-parse"}, args...)...)
}

func (c *defaultGitClient) VerifyRef(ctx context.Context, ref string) (bool, error) {
	if err := ctx.Err(); err != nil {
		return false, err
	}
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return false, fmt.Errorf("ref cannot be empty")
	}

	var stderr bytes.Buffer
	err := c.Run(ctx, io.Discard, &stderr, "rev-parse", "--verify", ref)
	if err == nil {
		return true, nil
	}
	errStr := strings.TrimSpace(stderr.String())
	errMsg := err.Error()
	if strings.Contains(errStr, "Needed a single revision") ||
		strings.Contains(errStr, "Not a valid object name") ||
		strings.Contains(errStr, "not found") ||
		strings.Contains(errMsg, "not found") ||
		strings.Contains(errMsg, "Needed a single revision") {
		return false, nil
	}
	if strings.Contains(errStr, "not a git repository") || strings.Contains(errMsg, "not a git repository") {
		return false, fmt.Errorf("not a git repository: %w", err)
	}
	if errStr != "" {
		return false, fmt.Errorf("git rev-parse --verify %s failed: %w: %s", ref, err, errStr)
	}
	return false, fmt.Errorf("git rev-parse --verify %s failed: %w", ref, err)
}

func (c *defaultGitClient) cachedGitDir() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.gitDir
}

func (c *defaultGitClient) setGitDir(dir string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.gitDir = dir
}

func (c *defaultGitClient) GitDir(ctx context.Context) (string, error) {
	if dir := c.cachedGitDir(); dir != "" {
		return dir, nil
	}
	dir, err := c.RevParse(ctx, "--git-dir")
	if err != nil {
		return "", err
	}
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return "", fmt.Errorf("git rev-parse --git-dir returned empty path")
	}
	if !filepath.IsAbs(dir) {
		if realRunner, ok := c.runner.(*RealGitRunner); ok && realRunner.Dir != "" {
			dir = filepath.Join(realRunner.Dir, dir)
		}
	}
	c.setGitDir(dir)
	return dir, nil
}

func (c *defaultGitClient) CountCommitsAhead(ctx context.Context, branch string) (int, error) {
	branch = strings.TrimSpace(branch)
	if branch == "" {
		return 0, fmt.Errorf("cannot count commits ahead: branch name is empty")
	}
	return CountCommitsAhead(ctx, c, branch)
}

func (c *defaultGitClient) StackCommits(ctx context.Context, branch string) ([]StackCommit, error) {
	branch = strings.TrimSpace(branch)
	if branch == "" {
		return nil, fmt.Errorf("cannot list stack commits: branch name is empty")
	}
	remote := c.ResolveRemote(ctx)
	primaryRef := fmt.Sprintf("%s/%s", remote, branch)

	candidates := []string{
		primaryRef,
		fmt.Sprintf("refs/remotes/%s/%s", remote, branch),
		"HEAD@{upstream}",
		branch,
	}
	baseRef := ""
	for _, cand := range candidates {
		if err := c.Run(ctx, io.Discard, io.Discard, "rev-parse", "--verify", cand); err == nil {
			baseRef = cand
			break
		}
	}
	if baseRef == "" {
		return nil, fmt.Errorf(
			"could not resolve upstream base branch %q on remote %q (tried %s, refs/remotes/%s/%s, HEAD@{upstream}, and %s)\n\n"+
				"Why: Stack inspection requires a valid upstream reference to bound the commit range and never falls back to walking full repository history.\n"+
				"Fix: Fetch the remote branch or specify an explicit base branch:\n"+
				"  git fetch %s %s\n"+
				"  gh pr create --base <branch> --stack",
			branch, remote, primaryRef, remote, branch, branch, remote, branch,
		)
	}

	return c.stackCommitsFrom(ctx, baseRef)
}

// countCommitsAheadRef is CountCommitsAhead, also returning the
// <remote>/<branch> ref it verified (see stackCommitsFrom).
func (c *defaultGitClient) countCommitsAheadRef(ctx context.Context, branch string) (int, string, error) {
	return countCommitsAhead(ctx, c, branch)
}

// stackCommitsFrom lists the commits in baseRef..HEAD, oldest first, like
// StackCommits but without probing for the base: callers pass a ref they
// already verified, namely the <remote>/<branch> ref returned by
// countCommitsAheadRef, which is StackCommits' first candidate.
func (c *defaultGitClient) stackCommitsFrom(ctx context.Context, baseRef string) ([]StackCommit, error) {
	rangeSpec := fmt.Sprintf("%s..HEAD", baseRef)
	var outBuf, errBuf bytes.Buffer
	if err := c.Run(ctx, &outBuf, &errBuf, "log", "--reverse", "--format=%h%x00%s%x00%B%x1e", rangeSpec); err != nil {
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
		return nil, fmt.Errorf("git log %s failed: %w", rangeSpec, err)
	}
	raw := outBuf.String()
	if strings.TrimSpace(raw) == "" {
		return nil, nil
	}
	records := strings.Split(raw, "\x1e")
	var commits []StackCommit
	for _, rec := range records {
		rec = strings.TrimLeft(rec, "\r\n")
		if strings.TrimSpace(rec) == "" {
			continue
		}
		parts := strings.SplitN(rec, "\x00", 3)
		if len(parts) < 3 {
			continue
		}
		hash := strings.TrimSpace(parts[0])
		subj := strings.TrimSpace(parts[1])
		body := parts[2]
		commits = append(commits, StackCommit{
			Hash:     hash,
			Subject:  subj,
			Body:     body,
			ChangeID: ExtractChangeID(body),
		})
	}
	return commits, nil
}

func (c *defaultGitClient) Fetch(ctx context.Context, remote, ref string, stdout, stderr io.Writer) error {
	remote = strings.TrimSpace(remote)
	if remote == "" {
		return fmt.Errorf("git fetch requires a remote")
	}
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return fmt.Errorf("git fetch requires a ref")
	}
	return c.Run(ctx, stdout, stderr, "fetch", remote, ref)
}

func (c *defaultGitClient) Checkout(ctx context.Context, ref string, stdout, stderr io.Writer) error {
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return fmt.Errorf("git checkout requires a ref")
	}
	return c.Run(ctx, stdout, stderr, "checkout", ref)
}

func (c *defaultGitClient) CherryPick(ctx context.Context, ref string, stdout, stderr io.Writer) error {
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return fmt.Errorf("git cherry-pick requires a ref")
	}
	return c.Run(ctx, stdout, stderr, "cherry-pick", ref)
}

func (c *defaultGitClient) IsHeadRemoteTip(ctx context.Context) (bool, []string, error) {
	var stdout, stderr bytes.Buffer
	if err := c.Run(ctx, &stdout, &stderr, "for-each-ref", "--points-at=HEAD", "--format=%(refname)", "refs/remotes/"); err != nil {
		return false, nil, fmt.Errorf("failed to check remote tracking branches: %w (stderr: %s)", err, stderr.String())
	}
	var remotes []string
	for _, line := range strings.Split(stdout.String(), "\n") {
		line = strings.TrimSpace(line)
		if line != "" {
			remotes = append(remotes, line)
		}
	}
	return len(remotes) > 0, remotes, nil
}

func (c *defaultGitClient) HeadAuthorEmail(ctx context.Context) (string, error) {
	var stdout, stderr bytes.Buffer
	if err := c.Run(ctx, &stdout, &stderr, "log", "-1", "--format=%ae", "HEAD"); err != nil {
		return "", fmt.Errorf("failed to get HEAD author email: %w (stderr: %s)", err, stderr.String())
	}
	return strings.TrimSpace(stdout.String()), nil
}

func (c *defaultGitClient) UserEmail(ctx context.Context) (string, error) {
	return c.ConfigGet(ctx, "user.email")
}

type sequencerMarker struct {
	name      string
	operation string
	fix       string
}

var activeSequencerMarkers = []sequencerMarker{
	{
		name:      "rebase-merge",
		operation: "interactive rebase",
		fix:       "  git rebase --continue\n  # or abort:\n  git rebase --abort",
	},
	{
		name:      "rebase-apply",
		operation: "rebase / patch application",
		fix:       "  git rebase --continue\n  # or abort:\n  git rebase --abort",
	},
	{
		name:      "MERGE_HEAD",
		operation: "merge",
		fix:       "  git merge --continue\n  # or abort:\n  git merge --abort",
	},
	{
		name:      "CHERRY_PICK_HEAD",
		operation: "cherry-pick",
		fix:       "  git cherry-pick --continue\n  # or abort:\n  git cherry-pick --abort",
	},
	{
		name:      "REVERT_HEAD",
		operation: "revert",
		fix:       "  git revert --continue\n  # or abort:\n  git revert --abort",
	},
	{
		name:      "BISECT_LOG",
		operation: "bisect",
		fix:       "  git bisect reset",
	},
}

// CheckAmendAllowed verifies that HEAD is safe to amend locally, rejecting
// active sequencer operations, multi-parent merge commits, and remote tips.
func (c *defaultGitClient) CheckAmendAllowed(ctx context.Context) error {
	gitDir, err := c.GitDir(ctx)
	if err != nil {
		return err
	}
	for _, m := range activeSequencerMarkers {
		if sequencerMarkerExists(ctx, c, gitDir, m.name) {
			return fmt.Errorf(
				"cannot amend HEAD commit: a git %s is currently in progress (%s exists in %s).\n\n"+
					"Why: Amending during an in-progress %s rewrites intermediate commit state and can corrupt your branch history.\n"+
					"To finish or cancel the in-progress operation first:\n%s\n\n"+
					"To inspect current repository status:\n  git status",
				m.operation, m.name, gitDir, m.operation, m.fix,
			)
		}
	}

	parentsOut, err := c.runOutput(ctx, "rev-list", "--parents", "-n", "1", "HEAD")
	if err != nil {
		return err
	}
	if fields := strings.Fields(parentsOut); len(fields) > 2 {
		return fmt.Errorf(
			"cannot amend HEAD commit: HEAD (%s) is a merge commit with %d parents.\n\n"+
				"Why: Amending a merge commit rewrites multi-parent merge ancestry, whereas Gerrit changes are single-parent linear commits.\n"+
				"To linearize your commit history onto the target branch instead:\n"+
				"  git rebase %s/main\n\n"+
				"To inspect recent commit ancestry:\n"+
				"  git log --graph -n 5",
			fields[0], len(fields)-1, c.ResolveRemote(ctx),
		)
	}

	isTip, remotes, err := c.IsHeadRemoteTip(ctx)
	if err != nil {
		return err
	}
	if isTip {
		return fmt.Errorf("cannot amend HEAD commit: HEAD matches remote tracking branch tip (%s).\n\n"+
			"This usually occurs after 'git reset --hard' to upstream or checking out a remote branch.\n"+
			"Amending now will hijack upstream history and original author's identity.\n"+
			"Please stage your changes and create a new commit first (e.g. 'git commit').",
			strings.Join(remotes, ", "))
	}
	return nil
}

// HasStagedChanges reports whether the Git index differs from HEAD.
func (c *defaultGitClient) HasStagedChanges(ctx context.Context) (bool, error) {
	var stderr bytes.Buffer
	err := c.Run(ctx, io.Discard, &stderr, "diff", "--cached", "--quiet")
	if err == nil {
		return false, nil
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) && exitErr.ExitCode() == 1 {
		return true, nil
	}
	if errStr := strings.TrimSpace(stderr.String()); errStr != "" {
		return false, fmt.Errorf("git diff --cached --quiet failed: %w: %s", err, errStr)
	}
	return false, fmt.Errorf("git diff --cached --quiet failed: %w", err)
}

func (c *defaultGitClient) commitWithMessageFile(
	ctx context.Context,
	message string,
	baseArgs []string,
	noVerify bool,
	stdout, stderr io.Writer,
) error {
	if strings.TrimSpace(message) == "" {
		return fmt.Errorf("commit message cannot be empty")
	}
	tmpFile, err := os.CreateTemp("", "ghish-commit-msg-*")
	if err != nil {
		return fmt.Errorf("failed to create temporary commit message file: %w", err)
	}
	defer os.Remove(tmpFile.Name())

	_, writeErr := io.WriteString(tmpFile, message)
	if err := errors.Join(writeErr, tmpFile.Close()); err != nil {
		return fmt.Errorf("failed to write temporary commit message file: %w", err)
	}

	args := append([]string{}, baseArgs...)
	if noVerify {
		args = append(args, "--no-verify")
	}
	args = append(args, "-F", tmpFile.Name())
	return c.Run(ctx, stdout, stderr, args...)
}

// CommitNewAll commits staged index changes (or passes -a when nothing is
// staged yet) using a temporary commit message file, returning autoStaged=true
// when -a was used.
func (c *defaultGitClient) CommitNewAll(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) (bool, error) {
	hasStaged, err := c.HasStagedChanges(ctx)
	if err != nil {
		return false, err
	}
	autoStaged := !hasStaged
	baseArgs := []string{"commit", "-a"}
	if hasStaged {
		baseArgs = []string{"commit"}
	}
	return autoStaged, c.commitWithMessageFile(ctx, message, baseArgs, noVerify, stdout, stderr)
}

// CommitAmendMessage rewrites HEAD's commit message via a temporary file while
// preserving staged index changes and intentional empty commits.
func (c *defaultGitClient) CommitAmendMessage(ctx context.Context, message string, noVerify bool, stdout, stderr io.Writer) error {
	if err := c.CheckAmendAllowed(ctx); err != nil {
		return err
	}
	return c.commitWithMessageFile(ctx, message, []string{"commit", "--amend", "--only", "--allow-empty"}, noVerify, stdout, stderr)
}

// CommitAmendNoEdit runs commit-msg hooks on HEAD while preserving staged index
// changes and intentional empty commits.
func (c *defaultGitClient) CommitAmendNoEdit(ctx context.Context, stdout, stderr io.Writer) error {
	if err := c.CheckAmendAllowed(ctx); err != nil {
		return err
	}
	return c.Run(ctx, stdout, stderr, "commit", "--amend", "--only", "--allow-empty", "--no-edit")
}

// UpdateHeadRef atomically moves HEAD from expectedCurrentSHA to targetSHA via
// compare-and-swap without modifying the index or working tree.
func (c *defaultGitClient) UpdateHeadRef(ctx context.Context, targetSHA, expectedCurrentSHA, reason string) error {
	targetSHA = strings.TrimSpace(targetSHA)
	if targetSHA == "" {
		return fmt.Errorf("update-ref target SHA cannot be empty")
	}
	expectedCurrentSHA = strings.TrimSpace(expectedCurrentSHA)
	if expectedCurrentSHA == "" {
		return fmt.Errorf("update-ref expected current SHA cannot be empty")
	}
	args := []string{"update-ref"}
	reason = strings.TrimSpace(reason)
	if reason != "" {
		args = append(args, "-m", reason)
	}
	args = append(args, "HEAD", targetSHA, expectedCurrentSHA)
	_, err := c.runOutput(ctx, args...)
	return err
}

func filterGitPushProgress(output string) string {
	if output == "" {
		return ""
	}
	lines := strings.Split(output, "\n")
	var kept []string
	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if isGitPushProgressLine(trimmed) {
			continue
		}
		kept = append(kept, line)
	}
	return strings.Join(kept, "\n")
}

func isGitPushProgressLine(trimmed string) bool {
	for _, prefix := range []string{
		"Counting objects:",
		"Enumerating objects:",
		"Compressing objects:",
		"Delta compression",
		"Writing objects:",
		"Total ",
		"remote: Resolving deltas:",
		"remote: Processing changes:",
	} {
		if strings.HasPrefix(trimmed, prefix) {
			return true
		}
	}
	return false
}
