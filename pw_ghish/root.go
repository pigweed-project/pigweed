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

// Package pw_ghish implements the gerrit-cli commands.
package pw_ghish

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"strings"
	"sync"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	// HostFlag is the global flag for the Gerrit host.
	HostFlag string
	// VerboseFlag enables verbose logging.
	VerboseFlag bool
	// ProfileFlag explicitly specifies the project profile.
	ProfileFlag string
)

// GitRunner defines the interface for running git commands.
type GitRunner interface {
	Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error
}

// RealGitRunner implements GitRunner using os/exec.
type RealGitRunner struct {
	Dir string
}

func (r *RealGitRunner) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	cmd := exec.CommandContext(ctx, "git", args...)
	if r != nil && r.Dir != "" {
		cmd.Dir = r.Dir
	}
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	return cmd.Run()
}

// Config holds the configuration for the CLI.
type Config struct {
	Host    string
	Git     GitRunner
	CWD     string
	Profile ProjectProfile
}

// GetProfile returns the project profile for the configuration.
func (c *Config) GetProfile(ctx context.Context) ProjectProfile {
	if c.Profile != nil {
		return c.Profile
	}
	if ProfileFlag != "" {
		if p, ok := GetProfile(ProfileFlag); ok {
			c.Profile = p
			return c.Profile
		}
	}
	if c.Host != "" {
		if p, err := DetectProfile("", c.Host, ""); err == nil && p.Name() != "generic" {
			c.Profile = p
			return c.Profile
		}
	}
	var remoteURL string
	if c.Git != nil {
		if val, err := c.GitClient().ConfigGet(ctx, "remote.origin.url"); err == nil {
			remoteURL = val
		}
	}
	p, err := DetectProfile(remoteURL, c.Host, ProfileFlag)
	if err != nil {
		return profiles["generic"]
	}
	c.Profile = p
	return c.Profile
}

type contextKey string

const configKey contextKey = "config"

// SetConfig sets the config in the command context.
func SetConfig(cmd *cobra.Command, cfg *Config) {
	ctx := cmd.Context()
	if ctx == nil {
		ctx = context.Background()
	}
	cmd.SetContext(context.WithValue(ctx, configKey, cfg))
}

// GetConfig retrieves the config from the command context or its parent commands.
func GetConfig(cmd *cobra.Command) *Config {
	for c := cmd; c != nil; c = c.Parent() {
		if c.Context() != nil {
			if cfg, ok := c.Context().Value(configKey).(*Config); ok && cfg != nil {
				return cfg
			}
		}
	}
	return nil
}

// DefaultGitRunner is the default implementation of GitRunner.
var DefaultGitRunner GitRunner = &RealGitRunner{}

const cheatSheetTemplate = `A command line tool for managing Gerrit code reviews, inspired by the GitHub CLI (gh).
This tool enables developers and GenAI agents to interact with Gerrit using standard GitHub CLI concepts.

===========================
GENERAL CONCEPT & BEHAVIOR
===========================
The CLI structure is centered around '%[1]s pr <command>'.
Most commands mirror standard GitHub 'gh pr' flags, but not all of them: a handful
are accepted with a different meaning. Check 'WHERE THIS DIFFERS FROM THE REAL gh'
at the end of this text before relying on a 'gh' flag you have not used here.

- Host Auto-Detection: When run from a local Git repository,
  %[1]s automatically detects the Gerrit host (e.g., pigweed-review.googlesource.com).
  Use the global '--host <domain>' flag to override or explicitly declare the host.
- Automatic Inline Threading: Posting an inline comment on a line with active threads
  automatically detects and appends a reply to the most recent active thread on that line.

===========================
CORE COMMAND CHEAT-SHEET
===========================

1. LISTING & STATUS
   - List open changes:
     $ %[1]s pr list [--limit 30] [--state open|closed|merged|all] [--base <branch>] [--label <label>]
   - Check your overall review status:
     $ %[1]s pr status

2. VIEWING DETAILS & COMMENTS
   - View change metadata and file modifications:
     $ %[1]s pr view <change_id>
   - View change and all review comments (with nested replies and line numbers):
     $ %[1]s pr view <change_id> --comments
   - Output structured metadata:
     $ %[1]s pr view <change_id> --json number,title,state,author,files
   - Query CI/CD check/build status (via LUCI Buildbucket):
     $ %[1]s pr checks <change_id>
   - Inspect failure logs, step details, or rerun checks:
     $ %[1]s run view <change_id> --log-failed
     $ %[1]s run view <change_id> -j <builder>
     $ %[1]s run rerun <change_id> --failed

3. WORKFLOW PREPARATION & CHECKOUTS
   - Checkout a specific change locally:
     $ %[1]s pr checkout <change_id>
   - Cherry-pick a change onto the active branch:
     $ %[1]s pr cherry-pick <change_id>

4. CREATING & EDITING
   - Create a new change from current HEAD:
     $ %[1]s pr create [-t "Title"] [-b "Body"] [--reviewer "user@google.com"] [--draft]
   - Edit change metadata or commit message:
     $ %[1]s pr edit <change_id> [-m "New message"] [--add-reviewer "email"] [--add-label "Code-Review=2"]

5. REVIEWS & RESPONDING
   - Submit review (Approve / Request changes):
     $ %[1]s pr review <change_id> --approve -m "Looks good!"
     $ %[1]s pr review <change_id> --request-changes -m "Please address feedback."
   - Post inline comments (and reply to threads):
     $ %[1]s pr comment <change_id> --path <file_path> --line <line_number> -m "Comment message" [--resolved] [--draft]
   - Post change-level comment from a file:
     $ %[1]s pr comment <change_id> -F <body_file_path> [--draft]

6. BUGANIZER ISSUES
   - View issue details (auto-detects Bug:/Fixed: trailer from HEAD if ID omitted):
     $ %[1]s issue view [<number> | <url>] [--comments] [--json <fields>]
   - List open issues in project component:
     $ %[1]s issue list [--assignee <email|me>] [--state open|closed|all] [-l P1] [-S "query"]
   - Create an issue (and optionally link to current commit via --amend):
     $ %[1]s issue create -t "Title" -b "Body" [-P P1] [--amend]
   - Close, reopen, comment, or edit issues:
     $ %[1]s issue close [<number> | <url>] [-r completed|"not planned"] [--duplicate-of <id>] [-c "Comment"]
     $ %[1]s issue comment [<number> | <url>] -b "Comment text"

7. WORKTREES & MULTI-AGENT SLOTS
   - Inspect/configure warm worktree pool, hooks, and shared Bazel caches:
     $ %[1]s wt init [--check] [--slots 10]
   - Allocate or resume a project in a warm slot (zero-click Antigravity/Jetski sidebar sync):
     $ %[1]s wt use <project> [--branch <branch>] [--cl <change_id>] [--json]
   - View live dashboard of mounted and parked projects with Gerrit statuses:
     $ %[1]s wt list
   - Park an idle project to free its warm slot (branch & CL remain tracked):
     $ %[1]s wt park <project>
   - Rebase onto origin/main in-place to start the next CL in a persistent project:
     $ %[1]s wt next [<project>]
   - Close a completed workstream permanently:
     $ %[1]s wt close <project>

===========================
RECOMMENDED AGENT WORKFLOW
===========================
When a GenAI Agent is tasked with addressing review feedback:
1. Verify Git state is clean (using 'git status --porcelain'). Stash uncommitted changes if needed.
2. Prep state by checking out a clean baseline (e.g. 'git checkout origin/main').
3. Cherry-pick the CL: '%[1]s pr cherry-pick <change_id>'.
4. Fetch & review comments: '%[1]s pr view <change_id> --comments'.
5. Apply fixes, compile, and validate via tests.
6. Respond to comments: Reply to inline threads using '%[1]s pr comment <change_id> --path ... --line ... -m ... --resolved'.
7. Upload new patchset: Push your changes to Gerrit using '%[1]s pr push'.
`

// ghDivergenceHelp is appended verbatim to the root help text. It is a plain
// constant rather than a format slot so that stray '%' characters can never
// break the Sprintf that builds the cheat sheet.
//
// Inclusion rule: a line earns its place here only if a 'gh' habit SUCCEEDS
// with a different meaning. Shorthands that 'gh' binds to something else are
// left unbound (see ghShorthandCollisions in root_test.go), so those mistakes
// fail loudly and do not belong in this list.
const ghDivergenceHelp = `
=============================================
WHERE THIS DIFFERS FROM THE REAL 'gh'
=============================================
No 'gh' shorthand is re-used here for a different flag: where a spelling would
collide it is simply not bound, so the mistake fails with an unknown-flag
error. Trust that error. What follows is the remaining set of places where a
flag is spelled as it is in 'gh' but the Gerrit concept underneath differs.

  pr list -a/--assignee   gh filters by assignee -> here Gerrit 'reviewer:' (Gerrit dropped assignees in 3.8)
  pr list -l/--label      gh an issue label      -> here a Gerrit vote predicate, e.g. Code-Review+2
  run list/view --json    gh a field list        -> here a boolean ('pr view --json' does take fields)
  --json state            gh OPEN/CLOSED/MERGED  -> here Gerrit's NEW/MERGED/ABANDONED
  pr review --request-changes  gh blocks the PR  -> here Code-Review-1, advisory; -2 is the veto
  pr comment --draft      (gh: a draft PR)       -> here an unpublished draft comment
  issue -l/--label        gh free-form text      -> here Buganizer priority/type/hotlist (P0-P4, bug, feature, task, hotlist:<id>)

Long form only, because 'gh' gives the shorthand another meaning: --auto
(gh -a is --assignee), --publish (-p is --project), --force (-f is --fill),
--cq (-q is --jq), and --message on 'pr edit' and 'pr merge' (-m is --milestone
and --merge).

Not implemented, and loud about it: --jq/-q as an output filter, 'gh api',
'gh auth status', -R/--repo, and 'pr merge --squash/--rebase/--delete-branch'
(Gerrit submits a whole change; the strategy is a project setting).
`

// RootCmd is the root command for gh-ish.
var RootCmd = &cobra.Command{
	Use:          "gh-ish",
	Short:        "Gerrit CLI - manage Gerrit code reviews",
	Long:         "",
	SilenceUsage: true,
	PersistentPreRunE: func(cmd *cobra.Command, args []string) error {
		if ProfileFlag != "" {
			if _, ok := GetProfile(ProfileFlag); !ok {
				return fmt.Errorf("unknown profile: %q", ProfileFlag)
			}
		}
		cwd, err := os.Getwd()
		if err != nil {
			cwd = os.Getenv("PWD")
		}

		existingCfg := GetConfig(cmd)
		if existingCfg != nil && existingCfg.CWD != "" {
			cwd = existingCfg.CWD
		}
		gitRunner := DefaultGitRunner
		if existingCfg != nil && existingCfg.Git != nil {
			gitRunner = existingCfg.Git
		}
		host := HostFlag
		if existingCfg != nil && existingCfg.Host != "" && host == "" {
			host = existingCfg.Host
		}
		SetConfig(cmd, &Config{
			Host: host,
			Git:  gitRunner,
			CWD:  cwd,
		})
		if VerboseFlag {
			opts := &slog.HandlerOptions{Level: slog.LevelDebug}
			handler := slog.NewTextHandler(os.Stderr, opts)
			slog.SetDefault(slog.New(handler))
		}
		return nil
	},
}

// PrCmd is the command for managing pull requests.
var PrCmd = &cobra.Command{
	Use:   "pr",
	Short: "Manage pull requests (changes)",
}

// IssueCmd is the command for managing Buganizer issues.
var IssueCmd = &cobra.Command{
	Use:   "issue",
	Short: "Manage Buganizer issues",
}

func setupRootCmdHelp() {
	invokedAs := os.Getenv("GH_ISH_INVOKED_AS")
	if invokedAs == "" {
		invokedAs = "gh-ish"
	}
	RootCmd.Use = invokedAs
	if RootCmd.Annotations == nil {
		RootCmd.Annotations = make(map[string]string)
	}
	RootCmd.Annotations[cobra.CommandDisplayNameAnnotation] = invokedAs
	RootCmd.Long = fmt.Sprintf(cheatSheetTemplate, invokedAs) + ghDivergenceHelp
}

func init() {
	setupRootCmdHelp()

	RootCmd.AddCommand(PrCmd)
	RootCmd.AddCommand(IssueCmd)
	RootCmd.PersistentFlags().StringVar(&HostFlag, "host", "", "Gerrit host to connect to")
	RootCmd.PersistentFlags().BoolVarP(&VerboseFlag, "verbose", "v", false, "Enable verbose (debug) logging")
	RootCmd.PersistentFlags().StringVar(&ProfileFlag, "profile", "", "Project profile (pigweed, fuchsia, generic)")

	flag.StringVar(&HostFlag, "host", "", "Gerrit host to connect to")
	flag.BoolVar(&VerboseFlag, "verbose", false, "Enable verbose (debug) logging")
	flag.StringVar(&ProfileFlag, "profile", "", "Project profile (pigweed, fuchsia, generic)")
}

// getRPCClient returns an HTTP client configured with authenticated transport.
var getRPCClient = func(ctx context.Context, gerritHost string) (*http.Client, error) {
	tr, err := NewAuthTransportContext(ctx, gerritHost)
	if err != nil {
		return nil, err
	}
	return &http.Client{Transport: tr}, nil
}

// NewGerritClient creates a new Gerrit client. It can be overridden in tests.
var NewGerritClient = func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
	config := GetConfig(cmd)
	if config == nil {
		config = &Config{
			Host: HostFlag,
			Git:  DefaultGitRunner,
		}
	}
	gerritURL, err := config.GerritURL(ctx)
	if err != nil {
		return nil, err
	}

	httpClient, err := getRPCClient(ctx, gerritURL)
	if err != nil {
		return nil, fmt.Errorf("failed to get HTTP client: %w", err)
	}

	return gerrit.NewClient(ctx, gerritURL, httpClient)
}

var rootAliasesOnce sync.Once

func newRootAlias(target *cobra.Command, name, short string) *cobra.Command {
	alias := &cobra.Command{
		Use:                target.Use,
		Aliases:            target.Aliases,
		Short:              short,
		Long:               target.Long,
		Example:            target.Example,
		ValidArgs:          target.ValidArgs,
		ValidArgsFunction:  target.ValidArgsFunction,
		Args:               target.Args,
		SilenceUsage:       target.SilenceUsage,
		SilenceErrors:      target.SilenceErrors,
		Run:                target.Run,
		RunE:               target.RunE,
		PreRun:             target.PreRun,
		PreRunE:            target.PreRunE,
		PostRun:            target.PostRun,
		PostRunE:           target.PostRunE,
		DisableFlagParsing: target.DisableFlagParsing,
		Annotations:        target.Annotations,
	}
	parts := strings.SplitN(target.Use, " ", 2)
	if len(parts) > 1 {
		alias.Use = name + " " + parts[1]
	} else {
		alias.Use = name
	}
	alias.Flags().AddFlagSet(target.Flags())
	for _, sub := range target.Commands() {
		alias.AddCommand(newRootAlias(sub, sub.Name(), sub.Short))
	}
	return alias
}

func setupRootAliases() {
	rootAliasesOnce.Do(func() {
		RootCmd.AddCommand(newRootAlias(checksCmd, "checks", "Show CI/CD status (from Buildbucket)"))
		RootCmd.AddCommand(newRootAlias(viewCmd, "view", "Display change details"))
		RootCmd.AddCommand(newRootAlias(diffCmd, "diff", "Show changes in a PR or patchset"))
		RootCmd.AddCommand(newRootAlias(statusCmd, "status", "Show status of relevant pull requests"))
	})
}

// Execute runs the root command.
func Execute() error {
	setupRootCmdHelp()
	setupRootAliases()
	RootCmd.SetArgs(NormalizeCQArgs(os.Args[1:]))
	return RootCmd.Execute()
}

// GerritURL returns the Gerrit host URL, detecting it from git config.
func (c *Config) GerritURL(ctx context.Context) (string, error) {
	var host string
	if c.Host != "" {
		host = c.Host
		if !strings.HasPrefix(host, "http://") && !strings.HasPrefix(host, "https://") {
			host = "https://" + host
		}
	} else {
		urlStr, err := c.GitClient().ConfigGet(ctx, "remote.origin.url")
		if err != nil || urlStr == "" {
			if defHost := c.GetProfile(ctx).DefaultGerritHost(); defHost != "" {
				return defHost, nil
			}
			return "", fmt.Errorf("could not detect Gerrit host: 'remote.origin.url' is not configured in git config, and no --host flag was provided.\n\n" +
				"Remedies:\n" +
				"  1. Run inside a cloned Git repository with an origin remote:\n" +
				"     git remote add origin https://<project>.googlesource.com/<project>\n" +
				"  2. Or specify the Gerrit host explicitly via flag:\n" +
				"     gh pr <subcommand> --host <project>-review.googlesource.com")
		}
		if before, ok := strings.CutSuffix(urlStr, ".git"); ok {
			urlStr = before
		}

		parsed, err := url.Parse(urlStr)
		if err == nil {
			if parsed.Scheme == "sso" {
				h := parsed.Host
				if strings.HasSuffix(h, "-review.googlesource.com") {
					// Already formatted
				} else if idx := strings.IndexByte(h, '.'); idx != -1 {
					h = h[:idx] + "-review.googlesource.com"
				} else {
					h = h + "-review.googlesource.com"
				}
				host = "https://" + h
			} else if strings.HasSuffix(parsed.Host, ".googlesource.com") {
				if !strings.Contains(parsed.Host, "-review") {
					parsed.Host = strings.Replace(parsed.Host, ".googlesource.com", "-review.googlesource.com", 1)
				}
				parsed.Path = "" // Strip project path for API base
				host = parsed.String()
			} else {
				host = urlStr
			}
		} else {
			host = urlStr
		}
	}

	// Apply /a suffix for googlesource.com hosts if not present
	u, err := url.Parse(host)
	if err == nil && strings.HasSuffix(u.Host, ".googlesource.com") && !strings.HasSuffix(host, "/a") {
		host = host + "/a"
	}

	return host, nil
}

// CleanGerritHost extracts and normalizes the hostname from a Gerrit host string or URL,
// stripping schemes (http://, https://), paths, trailing slashes, and Gerrit's /a suffix.
func CleanGerritHost(raw string) string {
	if raw == "" {
		return ""
	}
	if strings.Contains(raw, "://") {
		if parsedU, err := url.Parse(raw); err == nil && parsedU.Host != "" {
			return parsedU.Host
		}
	}
	raw = strings.TrimPrefix(strings.TrimPrefix(raw, "https://"), "http://")
	raw = strings.TrimSuffix(raw, "/")
	raw = strings.TrimSuffix(raw, "/a")
	return strings.TrimSuffix(raw, "/")
}

// GerritHost returns the cleaned Gerrit hostname (without scheme or path),
// resolving from Config.GerritURL(ctx) or falling back to HostFlag.
func (c *Config) GerritHost(ctx context.Context) string {
	var raw string
	if c != nil {
		if gURL, err := c.GerritURL(ctx); err == nil {
			raw = gURL
		}
	}
	if raw == "" {
		raw = HostFlag
	}
	return CleanGerritHost(raw)
}

// FindLatestChangeBySubject searches for the latest change with the given subject and owner:self.
func FindLatestChangeBySubject(ctx context.Context, cmd *cobra.Command, subject string) (int, error) {
	client, err := NewGerritClient(ctx, cmd)
	if err != nil {
		return 0, fmt.Errorf("failed to get Gerrit client: %w", err)
	}

	query := "owner:self"
	if subject != "" {
		query += fmt.Sprintf(` message:"%s"`, subject)
	}

	opt := &gerrit.QueryChangeOptions{
		QueryOptions: gerrit.QueryOptions{
			Query: []string{query},
			Limit: 1,
		},
	}
	changes, _, err := client.Changes.QueryChanges(ctx, opt)
	if err != nil {
		return 0, fmt.Errorf("failed to query changes: %w", err)
	}

	if changes == nil || len(*changes) == 0 {
		return 0, fmt.Errorf("no changes found matching query: %s", query)
	}

	return (*changes)[0].Number, nil
}
