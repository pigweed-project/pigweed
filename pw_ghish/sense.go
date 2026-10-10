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
	"os"
	"os/user"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

const (
	staleBehindThreshold   = 20
	staleAgeSeconds        = 3 * 24 * 3600 // 3 days
	maxInlineFailingBuilds = 2
	maxInlineLogLines      = 18
)

var (
	primaryOncallRegex   = regexp.MustCompile(`primary:\s*"([^"]+)"`)
	secondaryOncallRegex = regexp.MustCompile(`secondary:\s*"([^"]+)"`)
	chatRoomURLRegex     = regexp.MustCompile(`(?i)^https?://chat\.google\.com/(?:u/\d+/)?(?:room|dm)/([A-Za-z0-9_-]+)(?:/([A-Za-z0-9_.-]+))?(?:/([A-Za-z0-9_.-]+))?(?:[?#].*)?$`)
	chatMailURLRegex     = regexp.MustCompile(`(?i)^https?://mail\.google\.com/chat/(?:u/\d+/)?#chat/(?:space|dm)/([A-Za-z0-9_-]+)(?:/([A-Za-z0-9_.-]+))?(?:/([A-Za-z0-9_.-]+))?(?:[?#].*)?$`)
	chatResourceRegex    = regexp.MustCompile(`^spaces/([A-Za-z0-9_-]+)(?:/threads/([A-Za-z0-9_.-]+))?(?:/messages/([A-Za-z0-9_.-]+))?$`)
	mergeTreeConflictRe  = regexp.MustCompile(`CONFLICT \([^)]+\): Merge conflict in (.+)`)
)

// SenseGitState holds local Git repository state for `./gh sense`.
type SenseGitState struct {
	Branch           string   `json:"branch"`
	BaseRef          string   `json:"base_ref,omitempty"`
	HeadSHA          string   `json:"head_sha,omitempty"`
	CommitSubject    string   `json:"commit_subject,omitempty"`
	CommitAgeSeconds int64    `json:"commit_age_seconds,omitempty"`
	ChangeID         string   `json:"change_id,omitempty"`
	BugID            string   `json:"bug_id,omitempty"`
	CommitsAhead     int      `json:"commits_ahead"`
	CommitsBehind    int      `json:"commits_behind"`
	IsDirty          bool     `json:"is_dirty"`
	DirtyFilesCount  int      `json:"dirty_files_count,omitempty"`
	DirtyFiles       []string `json:"dirty_files,omitempty"`
	ConflictedFiles  []string `json:"conflicted_files,omitempty"`
	RebaseInProgress bool     `json:"rebase_in_progress,omitempty"`
	InProgressOp     string   `json:"in_progress_op,omitempty"`
	IsStale          bool     `json:"is_stale,omitempty"`
	MergeConflicts   []string `json:"merge_conflicts,omitempty"`
	RebaseVerdict    string   `json:"rebase_verdict,omitempty"`
	RebaseReason     string   `json:"rebase_reason,omitempty"`
}

func (g SenseGitState) effectiveBaseRef() string {
	if strings.TrimSpace(g.BaseRef) != "" {
		return strings.TrimSpace(g.BaseRef)
	}
	return "origin/main"
}

// SenseGitWorktreeEntry describes a single entry from `git worktree list --porcelain`.
type SenseGitWorktreeEntry struct {
	Path   string `json:"path"`
	Head   string `json:"head,omitempty"`
	Branch string `json:"branch,omitempty"`
}

// SenseWorktreeState holds local `gh wt` and `git worktree` state for `./gh sense`.
type SenseWorktreeState struct {
	CurrentProject           string                  `json:"current_project,omitempty"`
	CurrentSlot              string                  `json:"current_slot,omitempty"`
	CurrentStatusBadge       string                  `json:"current_status_badge,omitempty"`
	CurrentDetails           string                  `json:"current_details,omitempty"`
	CurrentRecommendedAction string                  `json:"current_recommended_action,omitempty"`
	IsUnmanagedGitWorktree   bool                    `json:"is_unmanaged_git_worktree,omitempty"`
	GitWorktrees             []SenseGitWorktreeEntry `json:"git_worktrees,omitempty"`
	TotalSlots               int                     `json:"total_slots,omitempty"`
	AvailableSlots           int                     `json:"available_slots,omitempty"`
	ReadyToLand              []WorkspaceProjectMatch `json:"ready_to_land,omitempty"`
	NeedsAttention           []WorkspaceProjectMatch `json:"needs_attention,omitempty"`
	MergedProjects           []WorkspaceProjectMatch `json:"merged_projects,omitempty"`
	AllProjects              []WorkspaceProjectMatch `json:"all_projects,omitempty"`
}

func (w SenseWorktreeState) isPoolManaged() bool {
	return w.CurrentProject != "" || w.CurrentSlot != "" || w.TotalSlots > 0 || len(w.AllProjects) > 0
}

// SenseFailingCheckDetail holds inline failure step and log excerpt details for a failing CI build.
type SenseFailingCheckDetail struct {
	Builder    string `json:"builder"`
	BuildID    string `json:"build_id,omitempty"`
	FailedStep string `json:"failed_step,omitempty"`
	LogExcerpt string `json:"log_excerpt,omitempty"`
}

// SenseCLState holds Gerrit CL metadata, review thread analysis, and CI build status.
type SenseCLState struct {
	Number                    int                       `json:"number"`
	Subject                   string                    `json:"subject"`
	Status                    string                    `json:"status"`
	Owner                     string                    `json:"owner,omitempty"`
	ChangeID                  string                    `json:"change_id"`
	URL                       string                    `json:"url"`
	ShortRef                  string                    `json:"short_ref,omitempty"`
	FetchRef                  string                    `json:"fetch_ref,omitempty"`
	HeadUploadedToGerrit      bool                      `json:"head_uploaded_to_gerrit,omitempty"`
	IsWIP                     bool                      `json:"is_wip,omitempty"`
	Submittable               bool                      `json:"submittable,omitempty"`
	Mergeable                 bool                      `json:"mergeable"`
	CodeReviewScore           int                       `json:"code_review_score"`
	VerifiedScore             int                       `json:"verified_score"`
	CommitQueueScore          int                       `json:"commit_queue_score"`
	UnresolvedThreadsCount    int                       `json:"unresolved_threads_count"`
	DraftsCount               int                       `json:"drafts_count"`
	ExternalUnresolvedThreads []UnresolvedComment       `json:"external_unresolved_threads,omitempty"`
	AuthorSelfDrafts          []UnresolvedComment       `json:"author_self_drafts,omitempty"`
	ChecksSummary             string                    `json:"checks_summary,omitempty"`
	FailingChecks             []CheckItem               `json:"failing_checks,omitempty"`
	FailingCheckDetails       []SenseFailingCheckDetail `json:"failing_check_details,omitempty"`
	PendingChecks             []CheckItem               `json:"pending_checks,omitempty"`
	PendingChecksCount        int                       `json:"pending_checks_count"`
	PassingChecksCount        int                       `json:"passing_checks_count"`
	Blockers                  []string                  `json:"blockers,omitempty"`
}

// SenseIssueComment holds a single human comment on an issue.
type SenseIssueComment struct {
	Number    int    `json:"number"`
	Author    string `json:"author,omitempty"`
	CreatedAt string `json:"created_at,omitempty"`
	Message   string `json:"message"`
}

// SenseRelatedCL holds a compact summary of a Gerrit CL linked to a target issue.
type SenseRelatedCL struct {
	Number   int    `json:"number"`
	ShortRef string `json:"short_ref,omitempty"`
	Status   string `json:"status"`
	Subject  string `json:"subject"`
	IsWIP    bool   `json:"is_wip,omitempty"`
}

// SenseIssueSummary holds a 1-line summary of an open candidate issue.
type SenseIssueSummary struct {
	Number   int64  `json:"number"`
	Priority string `json:"priority,omitempty"`
	Title    string `json:"title"`
}

// SenseIssueInfo holds normalized issue metadata, comments, and related CLs for a target issue.
type SenseIssueInfo struct {
	Number      int64               `json:"number"`
	Title       string              `json:"title"`
	State       string              `json:"state"`
	Priority    string              `json:"priority,omitempty"`
	Assignee    string              `json:"assignee,omitempty"`
	Description string              `json:"description,omitempty"`
	URL         string              `json:"url,omitempty"`
	Comments    []SenseIssueComment `json:"comments,omitempty"`
	RelatedCLs  []SenseRelatedCL    `json:"related_cls,omitempty"`
}

// SenseTargetState holds parsed and resolved metadata for an explicit `<target>` argument.
type SenseTargetState struct {
	RawArg              string                 `json:"raw_arg"`
	TargetType          string                 `json:"target_type"` // "bug" | "cl" | "chat_thread" | "unknown"
	NormalizedID        string                 `json:"normalized_id"`
	NotFound            bool                   `json:"not_found,omitempty"`
	FetchError          string                 `json:"fetch_error,omitempty"`
	ChatSpaceID         string                 `json:"chat_space_id,omitempty"`
	ChatThreadID        string                 `json:"chat_thread_id,omitempty"`
	ChatMessageID       string                 `json:"chat_message_id,omitempty"`
	ExistingWorktree    *WorkspaceProjectMatch `json:"existing_worktree,omitempty"`
	ExistingProjectUUID string                 `json:"existing_project_uuid,omitempty"`
	Issue               *SenseIssueInfo        `json:"issue,omitempty"`
	CL                  *SenseCLState          `json:"cl,omitempty"`
}

// SenseOncallState holds optional oncall rotation detection results.
type SenseOncallState struct {
	UserLogin       string `json:"user_login,omitempty"`
	PrimaryOncall   string `json:"primary_oncall,omitempty"`
	SecondaryOncall string `json:"secondary_oncall,omitempty"`
	IsUserOncall    bool   `json:"is_user_oncall,omitempty"`
	Role            string `json:"role,omitempty"`
}

// SensePrepareState records the outcome of `--prepare` worktree preparation and its safety gates.
type SensePrepareState struct {
	Requested      bool   `json:"requested"`
	Status         string `json:"status"` // "PREPARED" | "ALREADY_ON_TARGET" | "BLOCKED" | "SKIPPED_NO_TARGET"
	ActionTaken    string `json:"action_taken,omitempty"`
	BlockedReason  string `json:"blocked_reason,omitempty"`
	PreviousBranch string `json:"previous_branch,omitempty"`
	PreviousHEAD   string `json:"previous_head,omitempty"`
	NewBranch      string `json:"new_branch,omitempty"`
	NewHEAD        string `json:"new_head,omitempty"`
}

// SenseReport is the unified context snapshot and modality classification returned by `./gh sense`.
type SenseReport struct {
	Modality         string              `json:"modality"`
	Confidence       string              `json:"confidence"`
	Summary          string              `json:"summary"`
	RecommendedSteps []string            `json:"recommended_steps,omitempty"`
	SafetyGates      []string            `json:"safety_gates,omitempty"`
	Warnings         []string            `json:"warnings,omitempty"`
	Prepare          *SensePrepareState  `json:"prepare,omitempty"`
	Git              SenseGitState       `json:"git"`
	Worktree         SenseWorktreeState  `json:"worktree"`
	ActiveCL         *SenseCLState       `json:"active_cl,omitempty"`
	Target           *SenseTargetState   `json:"target,omitempty"`
	Oncall           SenseOncallState    `json:"oncall,omitempty"`
	CandidateIssues  []SenseIssueSummary `json:"candidate_issues,omitempty"`
}

// senseWarnCollector safely collects non-fatal subsystem warnings across concurrent sensing goroutines.
type senseWarnCollector struct {
	mu   sync.Mutex
	list []string
}

func (w *senseWarnCollector) addf(format string, args ...any) {
	if w == nil {
		return
	}
	msg := strings.TrimSpace(fmt.Sprintf(format, args...))
	if msg == "" {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	for _, existing := range w.list {
		if existing == msg {
			return
		}
	}
	w.list = append(w.list, msg)
}

func (w *senseWarnCollector) items() []string {
	if w == nil {
		return nil
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if len(w.list) == 0 {
		return nil
	}
	out := make([]string, len(w.list))
	copy(out, w.list)
	return out
}

// senseProjectMeta encapsulates project-specific conventions (shortlinks, base branch, issue prefix,
// and URL templates) derived from ProjectConfig and ProjectProfile so `sense` has zero hardcoded
// project assumptions.
type senseProjectMeta struct {
	profileName       string
	remote            string
	defaultBranch     string
	baseRef           string
	issuePrefix       string
	clShortlinkPrefix string
	projectLabel      string
	localCheckHint    string
}

func resolveSenseProjectMeta(ctx context.Context, cfg *Config, projCfg *ProjectConfig) senseProjectMeta {
	remote := "origin"
	defBranch := "main"
	if cfg != nil {
		if r := strings.TrimSpace(cfg.ResolveRemote(ctx)); r != "" {
			remote = r
		}
		if b := strings.TrimSpace(cfg.ResolveDefaultBranch(ctx)); b != "" {
			defBranch = b
		}
	}

	issuePrefix := "b/"
	if projCfg != nil && strings.TrimSpace(projCfg.Issue.Prefix) != "" {
		p := strings.TrimSpace(projCfg.Issue.Prefix)
		if !strings.HasSuffix(p, "/") && !strings.HasSuffix(p, ":") && !strings.HasSuffix(p, "-") {
			p += "/"
		}
		issuePrefix = p
	}

	profileName := "generic"
	var profile ProjectProfile
	if cfg != nil {
		profile = cfg.GetProfile(ctx)
		if profile != nil && profile.Name() != "" {
			profileName = profile.Name()
		}
	}

	gHost := ""
	if cfg != nil {
		gHost = CanonicalGerritHost(cfg.GerritHost(ctx))
	}
	if gHost == "" && profile != nil {
		gHost = CanonicalGerritHost(profile.DefaultGerritHost())
	}

	clShortlink := ""
	if projCfg != nil && len(projCfg.Gerrit.Shortlinks) > 0 {
		for pfx, host := range projCfg.Gerrit.Shortlinks {
			if gHost == "" || CanonicalGerritHost(host) == gHost {
				clShortlink = strings.TrimRight(strings.TrimSpace(pfx), "/") + "/"
				break
			}
		}
	}
	if clShortlink == "" {
		switch profileName {
		case "pigweed":
			clShortlink = "pwrev/"
		case "fuchsia":
			clShortlink = "fxrev/"
		}
	}

	projectLabel := ""
	if projCfg != nil && strings.TrimSpace(projCfg.Gerrit.Project) != "" {
		projectLabel = strings.TrimSpace(projCfg.Gerrit.Project)
	} else if profileName != "" && profileName != "generic" {
		projectLabel = strings.ToUpper(profileName[:1]) + profileName[1:]
	}

	localHint := ""
	if projCfg != nil && strings.TrimSpace(projCfg.CI.LocalPresubmitHint) != "" {
		localHint = strings.TrimSpace(projCfg.CI.LocalPresubmitHint)
	}

	return senseProjectMeta{
		profileName:       profileName,
		remote:            remote,
		defaultBranch:     defBranch,
		baseRef:           remote + "/" + defBranch,
		issuePrefix:       issuePrefix,
		clShortlinkPrefix: clShortlink,
		projectLabel:      projectLabel,
		localCheckHint:    localHint,
	}
}

func (m senseProjectMeta) issueRef(id any) string {
	s := strings.TrimSpace(fmt.Sprintf("%v", id))
	s = strings.TrimPrefix(s, m.issuePrefix)
	s = strings.TrimPrefix(s, "b/")
	return m.issuePrefix + s
}

func (m senseProjectMeta) clRef(num int) string {
	if m.clShortlinkPrefix != "" {
		return fmt.Sprintf("%s%d", m.clShortlinkPrefix, num)
	}
	return fmt.Sprintf("#%d", num)
}

func (m senseProjectMeta) issueWebURL(ctx context.Context, cfg *Config, projCfg *ProjectConfig, issueID int64) string {
	if projCfg != nil && strings.TrimSpace(projCfg.Issue.Host) != "" {
		h := strings.TrimRight(strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(projCfg.Issue.Host), "https://"), "http://"), "/")
		return fmt.Sprintf("https://%s/issues/%d", h, issueID)
	}
	if cfg != nil {
		if p := cfg.GetProfile(ctx); p != nil {
			if u := p.IssueWebURL(issueID); u != "" {
				return u
			}
		}
	}
	return fmt.Sprintf("https://issuetracker.google.com/issues/%d", issueID)
}

func (m senseProjectMeta) clWebURL(ctx context.Context, cfg *Config, gHost, project string, number int) string {
	host := CanonicalGerritHost(gHost)
	if host == "" && cfg != nil {
		host = CanonicalGerritHost(cfg.GerritHost(ctx))
		if host == "" {
			if p := cfg.GetProfile(ctx); p != nil {
				host = CanonicalGerritHost(p.DefaultGerritHost())
			}
		}
	}
	if host != "" && project != "" {
		return fmt.Sprintf("https://%s/c/%s/+/%d", host, project, number)
	}
	if host != "" {
		return fmt.Sprintf("https://%s/+/%d", host, number)
	}
	return m.clRef(number)
}

// NewSenseCommand creates a Cobra command for `./gh sense` (also aliased under `pr` and `wt`).
func NewSenseCommand() *cobra.Command {
	var (
		jsonFlag     bool
		statusFlag   bool
		dryRunFlag   bool
		fleetFlag    bool
		allSlotsFlag bool
		prepareFlag  bool
	)

	cmd := &cobra.Command{
		Use:   "sense [<target>]",
		Short: "Sense Git, worktree, Gerrit CL, CI, issue, and oncall context and classify workflow modality",
		Long: `Perform fast context sensing across local Git state, worktrees,
active Gerrit CL threads/drafts, CI checks, issue tracker metadata, and optional oncall schedules.

Outputs a compact, token-efficient text directive briefing by default (or with --status / --dry-run),
or outputs structured JSON when --json is passed. Pass --prepare to safely fetch and switch the local
branch for a bug or CL target when the working tree is clean.`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			targetArg := ""
			if len(args) > 0 {
				targetArg = strings.TrimSpace(args[0])
			}
			includeFleet := fleetFlag || allSlotsFlag
			report, err := BuildSenseReportWithOptions(cmd.Context(), cmd, targetArg, includeFleet, prepareFlag)
			if err != nil {
				return err
			}

			if jsonFlag {
				data, err := json.MarshalIndent(report, "", "  ")
				if err != nil {
					return fmt.Errorf("failed to marshal sense report JSON: %w", err)
				}
				fmt.Fprintln(cmd.OutOrStdout(), string(data))
				return nil
			}

			_ = statusFlag
			_ = dryRunFlag
			fmt.Fprint(cmd.OutOrStdout(), FormatSenseStatusCard(report))
			return nil
		},
	}

	cmd.Flags().BoolVar(&jsonFlag, "json", false, "Output structured JSON report instead of compact text briefing")
	cmd.Flags().BoolVar(&statusFlag, "status", false, "Render compact text status card and action plan (default)")
	cmd.Flags().BoolVar(&dryRunFlag, "dry-run", false, "Alias for --status (preview classification without taking action)")
	cmd.Flags().BoolVar(&fleetFlag, "fleet", false, "Query Gerrit status across all managed gh wt worktrees")
	cmd.Flags().BoolVar(&allSlotsFlag, "all-slots", false, "Alias for --fleet")
	cmd.Flags().BoolVar(&prepareFlag, "prepare", false, "Safely fetch and switch to target bug/CL branch if working tree is clean")
	return cmd
}

func init() {
	RootCmd.AddCommand(NewSenseCommand())
	PrCmd.AddCommand(NewSenseCommand())
}

// ParseChatThreadTarget extracts (spaceID, threadID, messageID, ok) from a Google Chat URL or resource name.
func ParseChatThreadTarget(rawArg string) (string, string, string, bool) {
	arg := strings.TrimSpace(rawArg)
	for _, re := range []*regexp.Regexp{chatRoomURLRegex, chatMailURLRegex, chatResourceRegex} {
		if m := re.FindStringSubmatch(arg); len(m) > 1 && m[1] != "" {
			spaceID := m[1]
			threadID := ""
			msgID := ""
			if len(m) > 2 {
				threadID = m[2]
			}
			if len(m) > 3 {
				msgID = m[3]
			}
			return spaceID, threadID, msgID, true
		}
	}
	return "", "", "", false
}

// ParseSenseTargetArg classifies a raw argument into ("bug"|"cl"|"chat_thread"|"unknown", normalizedID).
func ParseSenseTargetArg(rawArg string) (string, string) {
	arg := strings.TrimSpace(rawArg)
	if arg == "" {
		return "unknown", ""
	}

	// 1. Google Chat thread / space URLs or resource names
	if spaceID, threadID, _, ok := ParseChatThreadTarget(arg); ok {
		if threadID != "" {
			return "chat_thread", spaceID + "/" + threadID
		}
		return "chat_thread", spaceID
	}

	// 2. Explicit bug prefixes, Issue Tracker URLs, or 8+ digit issue IDs
	if shorthandBugRegex.MatchString(arg) || issueTrackerURLRegex.MatchString(arg) || publicChromiumIssueRegex.MatchString(arg) {
		if id, err := ParseIssueID(arg); err == nil && id > 0 {
			return "bug", strconv.FormatInt(id, 10)
		}
	}
	if m := pureNumericBugRegex.FindStringSubmatch(arg); len(m) > 1 && len(m[1]) >= 8 {
		return "bug", m[1]
	}

	// 3. Explicit Gerrit CL shortlinks, URLs, Change-Ids, or 3-7 digit CL numbers
	if m := pureNumericBugRegex.FindStringSubmatch(arg); len(m) > 1 && len(m[1]) >= 3 && len(m[1]) <= 7 {
		return "cl", m[1]
	}
	if strings.HasPrefix(arg, "#") {
		if m := pureNumericBugRegex.FindStringSubmatch(strings.TrimPrefix(arg, "#")); len(m) > 1 {
			return "cl", m[1]
		}
	}
	if isChangeIdentifier(arg) {
		parsed := ParseChangeTarget(arg)
		if parsed.ChangeID != "" {
			return "cl", parsed.ChangeID
		}
	}

	return "unknown", arg
}

// BuildSenseReport gathers local and remote signals in parallel and classifies the workflow modality.
func BuildSenseReport(ctx context.Context, cmd *cobra.Command, targetArg string, includeFleet bool) (*SenseReport, error) {
	return BuildSenseReportWithOptions(ctx, cmd, targetArg, includeFleet, false)
}

// BuildSenseReportWithOptions gathers local and remote signals in parallel, optionally executes
// safe worktree preparation (--prepare) when all 7 non-clobbering gates pass, and classifies the modality.
func BuildSenseReportWithOptions(ctx context.Context, cmd *cobra.Command, targetArg string, includeFleet bool, prepare bool) (*SenseReport, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	// Fail fast if an explicit --profile flag is unknown
	if strings.TrimSpace(ProfileFlag) != "" {
		if _, err := DetectProfile("", "", ProfileFlag); err != nil {
			return nil, err
		}
	}

	cfg := GetConfig(cmd)
	if cfg == nil {
		cfg = &Config{Git: DefaultGitRunner}
	}
	if cfg.Git == nil {
		return nil, fmt.Errorf("internal error: git runner is not initialized")
	}

	// Fail fast on malformed .ghish.toml or invalid git config ghish.*
	projCfg, err := cfg.LoadProjectConfig(ctx)
	if err != nil {
		return nil, err
	}
	if projCfg == nil {
		projCfg = DefaultProjectConfig()
	}

	cwd := cfg.CWD
	if cwd == "" {
		if wd, wdErr := os.Getwd(); wdErr == nil {
			cwd = wd
		}
	}

	meta := resolveSenseProjectMeta(ctx, cfg, projCfg)
	warnings := &senseWarnCollector{}

	gitState, gitWTs := senseGitAndWorktrees(ctx, cfg, cwd, meta, warnings)

	var (
		targetType   string
		normTargetID string
		targetBugID  int64
		targetCLNum  int
		targetCID    string
		chatSpaceID  string
		chatThreadID string
		chatMsgID    string
	)
	if targetArg != "" {
		targetType, normTargetID = ParseSenseTargetArg(targetArg)
		switch targetType {
		case "bug":
			if parsedID, parseErr := strconv.ParseInt(normTargetID, 10, 64); parseErr == nil {
				targetBugID = parsedID
			} else {
				warnings.addf("failed to parse issue ID %q: %v", normTargetID, parseErr)
			}
		case "cl":
			if n, convErr := strconv.Atoi(normTargetID); convErr == nil {
				targetCLNum = n
			} else if strings.HasPrefix(normTargetID, "I") {
				targetCID = normTargetID
			}
		case "chat_thread":
			chatSpaceID, chatThreadID, chatMsgID, _ = ParseChatThreadTarget(targetArg)
		}
	}

	var (
		wg              sync.WaitGroup
		wtSenseRes      *WorkspaceSenseResult
		activeCL        *SenseCLState
		targetSt        *SenseTargetState
		oncallSt        SenseOncallState
		candidateIssues []SenseIssueSummary
	)

	// 1. Worktree sensing (instant local state load unless includeFleet is true)
	wg.Add(1)
	go func() {
		defer wg.Done()
		if RegisteredWorkspaceIntegration != nil && RegisteredWorkspaceIntegration.IsEnabled() {
			res, wErr := RegisteredWorkspaceIntegration.SenseWorktrees(ctx, cwd, targetBugID, targetCLNum, targetCID, includeFleet)
			if wErr != nil {
				warnings.addf("worktree sensing failed: %v", wErr)
			} else if res != nil {
				wtSenseRes = res
				return
			}
		}
		wtSenseRes = &WorkspaceSenseResult{
			Enabled:        false,
			ReadyToLand:    []WorkspaceProjectMatch{},
			NeedsAttention: []WorkspaceProjectMatch{},
			MergedProjects: []WorkspaceProjectMatch{},
		}
	}()

	// 2. Active CL sensing (only when HEAD has a Change-Id and CommitsAhead > 0)
	if gitState.ChangeID != "" && gitState.CommitsAhead > 0 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			cl, clErr := senseGerritCL(ctx, cmd, cfg, projCfg, meta, gitState.ChangeID, gitState.HeadSHA, cwd, true, warnings)
			if clErr != nil && !isNotFoundError(clErr) {
				warnings.addf("active change %s: %v", gitState.ChangeID, firstLine(clErr.Error()))
			}
			activeCL = cl
		}()
	}

	// 3. Target sensing (when an explicit <target> argument was provided)
	if targetArg != "" {
		wg.Add(1)
		go func() {
			defer wg.Done()
			st := &SenseTargetState{
				RawArg:        targetArg,
				TargetType:    targetType,
				NormalizedID:  normTargetID,
				ChatSpaceID:   chatSpaceID,
				ChatThreadID:  chatThreadID,
				ChatMessageID: chatMsgID,
			}
			switch targetType {
			case "bug":
				if targetBugID > 0 {
					iss, issErr := senseBugTarget(ctx, cmd, cfg, meta, projCfg, targetBugID, warnings)
					st.Issue = iss
					if iss == nil {
						if issErr != nil && !isNotFoundError(issErr) {
							st.FetchError = firstLine(issErr.Error())
							warnings.addf("target issue %s: %s", meta.issueRef(targetBugID), st.FetchError)
						} else if isNotFoundError(issErr) || cfg.Git == DefaultGitRunner {
							st.NotFound = true
						}
					}
				}
			case "cl":
				// Avoid duplicate Gerrit RPC if target Arg is identical to active Change-Id
				if targetCID != "" && targetCID == gitState.ChangeID && gitState.CommitsAhead > 0 {
					// Linked after wg.Wait() from activeCL
				} else {
					cl, clErr := senseGerritCL(ctx, cmd, cfg, projCfg, meta, targetArg, gitState.HeadSHA, cwd, false, warnings)
					st.CL = cl
					if cl == nil {
						if clErr != nil && !isNotFoundError(clErr) {
							st.FetchError = firstLine(clErr.Error())
							warnings.addf("target CL %s: %s", normTargetID, st.FetchError)
						} else if isNotFoundError(clErr) || cfg.Git == DefaultGitRunner {
							st.NotFound = true
						}
					}
				}
			}
			targetSt = st
		}()
	}

	// 4. Optional oncall schedule sensing
	wg.Add(1)
	go func() {
		defer wg.Done()
		oncallSt = senseOncall(ctx, cfg, projCfg, warnings)
	}()

	// 5. Candidate issues pre-fetch when worktree is clean/idle and no target arg was passed
	if targetArg == "" && !gitState.IsDirty && !gitState.RebaseInProgress && gitState.CommitsAhead == 0 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			candidateIssues = senseCandidateIssues(ctx, cmd, cfg, warnings)
		}()
	}

	wg.Wait()

	// If worktree had 1 commit ahead on a MERGED CL and no target arg was passed, pre-fetch candidate issues now
	if targetArg == "" && !gitState.IsDirty && !gitState.RebaseInProgress && len(candidateIssues) == 0 && activeCL != nil && activeCL.Status == "MERGED" {
		candidateIssues = senseCandidateIssues(ctx, cmd, cfg, warnings)
	}

	// Share activeCL and targetSt.CL if they refer to the same change
	if targetSt != nil && targetSt.TargetType == "cl" && isSameGerritCL(targetSt, activeCL, gitState.ChangeID) {
		if activeCL != nil {
			targetSt.CL = activeCL
			targetSt.NotFound = false
			targetSt.FetchError = ""
		}
	}

	// Cross-link target CL Change-Id with local worktrees if not already matched
	if targetSt != nil {
		if wtSenseRes != nil && wtSenseRes.TargetMatch == nil && targetSt.CL != nil && targetSt.CL.ChangeID != "" && RegisteredWorkspaceIntegration != nil && RegisteredWorkspaceIntegration.IsEnabled() {
			if retryRes, rErr := RegisteredWorkspaceIntegration.SenseWorktrees(ctx, cwd, 0, targetSt.CL.Number, targetSt.CL.ChangeID, false); rErr != nil {
				warnings.addf("worktree target cross-link failed: %v", rErr)
			} else if retryRes != nil && retryRes.TargetMatch != nil {
				wtSenseRes.TargetMatch = retryRes.TargetMatch
			}
		}
		if wtSenseRes != nil && wtSenseRes.TargetMatch == nil && wtSenseRes.CurrentProject != "" && isSameGerritCL(targetSt, activeCL, gitState.ChangeID) {
			cid := gitState.ChangeID
			if activeCL != nil && activeCL.ChangeID != "" {
				cid = activeCL.ChangeID
			} else if targetSt.CL != nil && targetSt.CL.ChangeID != "" {
				cid = targetSt.CL.ChangeID
			}
			wtSenseRes.TargetMatch = &WorkspaceProjectMatch{
				Project:     wtSenseRes.CurrentProject,
				Residency:   "MOUNTED",
				Slot:        wtSenseRes.CurrentSlot,
				SymlinkPath: cwd,
				Branch:      gitState.Branch,
				ChangeID:    cid,
			}
		}
		if wtSenseRes != nil && wtSenseRes.TargetMatch != nil {
			targetSt.ExistingWorktree = wtSenseRes.TargetMatch
			targetSt.ExistingProjectUUID = wtSenseRes.TargetMatch.ProjectUUID
		}
	}

	// Execute optional ultra-safe worktree preparation (--prepare) if requested
	var prepState *SensePrepareState
	if prepare {
		prepState = executeSafePrepare(ctx, cmd, cfg, projCfg, meta, cwd, &gitState, gitWTs, &activeCL, targetSt, warnings)
	}

	// Compute deterministic rebase verdict now that gitState and activeCL are both available
	gitState.RebaseVerdict, gitState.RebaseReason = computeRebaseVerdict(gitState, activeCL)

	// Build SenseWorktreeState
	wtState := SenseWorktreeState{
		ReadyToLand:    []WorkspaceProjectMatch{},
		NeedsAttention: []WorkspaceProjectMatch{},
		MergedProjects: []WorkspaceProjectMatch{},
	}
	if wtSenseRes != nil {
		wtState.CurrentProject = wtSenseRes.CurrentProject
		wtState.CurrentSlot = wtSenseRes.CurrentSlot
		wtState.CurrentStatusBadge = wtSenseRes.CurrentStatusBadge
		wtState.CurrentDetails = wtSenseRes.CurrentDetails
		wtState.CurrentRecommendedAction = wtSenseRes.CurrentRecommendedAction
		wtState.TotalSlots = wtSenseRes.TotalSlots
		wtState.AvailableSlots = wtSenseRes.AvailableSlots
		if wtSenseRes.ReadyToLand != nil {
			wtState.ReadyToLand = wtSenseRes.ReadyToLand
		}
		if wtSenseRes.NeedsAttention != nil {
			wtState.NeedsAttention = wtSenseRes.NeedsAttention
		}
		if wtSenseRes.MergedProjects != nil {
			wtState.MergedProjects = wtSenseRes.MergedProjects
		}
		if wtSenseRes.AllProjects != nil {
			wtState.AllProjects = wtSenseRes.AllProjects
		}
		if gitState.BugID == "" && wtSenseRes.CurrentIssueID > 0 {
			gitState.BugID = meta.issueRef(wtSenseRes.CurrentIssueID)
		}
	}
	if len(gitWTs) > 1 && wtState.CurrentProject == "" && wtState.CurrentSlot == "" {
		wtState.IsUnmanagedGitWorktree = true
		wtState.GitWorktrees = gitWTs
	}

	if wtState.CurrentStatusBadge == "" {
		wtState.CurrentStatusBadge = inferSingleWorktreeBadge(gitState, activeCL)
	}

	modality, confidence, summary, steps, gates := classifySenseModalityWithMeta(gitState, wtState, activeCL, targetSt, oncallSt, len(candidateIssues), meta)

	// Deduplicate targetSt.CL before returning if it is identical to activeCL so JSON doesn't emit it twice
	if targetSt != nil && activeCL != nil && targetSt.CL != nil && targetSt.CL.Number == activeCL.Number {
		targetCopy := *targetSt
		targetCopy.CL = nil
		targetSt = &targetCopy
	}

	return &SenseReport{
		Modality:         modality,
		Confidence:       confidence,
		Summary:          summary,
		RecommendedSteps: steps,
		SafetyGates:      gates,
		Warnings:         warnings.items(),
		Prepare:          prepState,
		Git:              gitState,
		Worktree:         wtState,
		ActiveCL:         activeCL,
		Target:           targetSt,
		Oncall:           oncallSt,
		CandidateIssues:  candidateIssues,
	}, nil
}

// isSameGerritCL reports whether target (of type "cl") refers to the same Gerrit change as activeCL or localChangeID.
func isSameGerritCL(target *SenseTargetState, activeCL *SenseCLState, localChangeID string) bool {
	if target == nil || target.TargetType != "cl" {
		return false
	}
	if activeCL != nil {
		if strconv.Itoa(activeCL.Number) == target.NormalizedID ||
			(activeCL.ChangeID != "" && activeCL.ChangeID == target.NormalizedID) ||
			(target.CL != nil && activeCL.Number > 0 && activeCL.Number == target.CL.Number) {
			return true
		}
	}
	if target.CL != nil && localChangeID != "" && localChangeID == target.CL.ChangeID {
		return true
	}
	return false
}

// branchTracksBug reports whether gitState's BugID or branch name already tracks the normalized numeric bug ID.
func branchTracksBug(git SenseGitState, normBugID string) bool {
	if normBugID == "" {
		return false
	}
	if strings.TrimPrefix(git.BugID, "b/") == normBugID || git.BugID == normBugID {
		return true
	}
	wantBranch := "b-" + normBugID
	return git.Branch == wantBranch || strings.HasPrefix(git.Branch, wantBranch+"-")
}

func isNotFoundError(err error) bool {
	if err == nil {
		return false
	}
	msg := err.Error()
	return reStatus404.MatchString(msg) || strings.Contains(strings.ToLower(msg), "not found") || strings.Contains(strings.ToLower(msg), "does not exist")
}

// CanSafelyPrepareWorktree evaluates the 7 Ironclad Non-Clobbering Safety Gates before `--prepare`
// is allowed to fetch or switch branches in the current worktree.
func CanSafelyPrepareWorktree(
	git SenseGitState,
	activeCL *SenseCLState,
	target *SenseTargetState,
	cwd string,
	gitWTs []SenseGitWorktreeEntry,
) (status string, reason string) {
	baseRef := git.effectiveBaseRef()

	// Gate 7 / Target validation
	if target == nil || (target.TargetType != "bug" && target.TargetType != "cl") {
		return "SKIPPED_NO_TARGET", "--prepare requires an issue or CL target"
	}
	if target.NotFound {
		return "BLOCKED", fmt.Sprintf("GATE_7_TARGET_NOT_FOUND: %s %s was not found on remote server", target.TargetType, target.NormalizedID)
	}
	if target.FetchError != "" {
		return "BLOCKED", fmt.Sprintf("GATE_7_TARGET_FETCH_FAILED: %s", target.FetchError)
	}
	if target.TargetType == "bug" && target.Issue != nil && target.Issue.State == "CLOSED" {
		return "BLOCKED", fmt.Sprintf("GATE_7_BUG_ALREADY_CLOSED: issue b/%s is already CLOSED (%s)", target.NormalizedID, target.Issue.Title)
	}

	// Check if already on the requested target
	if isSameGerritCL(target, activeCL, git.ChangeID) && activeCL != nil {
		return "ALREADY_ON_TARGET", fmt.Sprintf("CL #%d is already checked out on branch %q", activeCL.Number, git.Branch)
	}
	if target.TargetType == "bug" && branchTracksBug(git, target.NormalizedID) {
		return "ALREADY_ON_TARGET", fmt.Sprintf("branch %q already tracks b/%s", git.Branch, target.NormalizedID)
	}

	// Gate 1: Zero uncommitted or untracked files in `git status --porcelain`
	if git.IsDirty || git.DirtyFilesCount > 0 {
		return "BLOCKED", fmt.Sprintf("GATE_1_DIRTY_TREE: working tree has %d uncommitted or untracked file(s)", git.DirtyFilesCount)
	}

	// Gate 2: No in-progress Git state machine operation (rebase, merge, cherry-pick, bisect, revert)
	if git.RebaseInProgress || git.InProgressOp != "" {
		op := git.InProgressOp
		if op == "" {
			op = "rebase"
		}
		return "BLOCKED", fmt.Sprintf("GATE_2_GIT_OP_IN_PROGRESS: git %s is currently in progress", op)
	}

	// Gate 3: Cryptographic verification of local commits (`<baseRef>..HEAD`)
	if git.CommitsAhead > 0 {
		if activeCL == nil {
			return "BLOCKED", fmt.Sprintf("GATE_3_UNPUSHED_COMMITS: branch %q has %d local commit(s) not uploaded to Gerrit", git.Branch, git.CommitsAhead)
		}
		if activeCL.Status == "NEW" {
			return "BLOCKED", fmt.Sprintf("GATE_3_ACTIVE_OPEN_CL: branch %q has open unmerged CL #%d (%s)", git.Branch, activeCL.Number, activeCL.Subject)
		}
		if activeCL.Status != "MERGED" {
			return "BLOCKED", fmt.Sprintf("GATE_3_UNMERGED_CL: branch %q CL #%d has status %s (not MERGED)", git.Branch, activeCL.Number, activeCL.Status)
		}
		if git.CommitsAhead > 1 {
			return "BLOCKED", fmt.Sprintf("GATE_3_MULTIPLE_LOCAL_COMMITS: branch %q has %d commits ahead of %s", git.Branch, git.CommitsAhead, baseRef)
		}
		if !activeCL.HeadUploadedToGerrit {
			return "BLOCKED", fmt.Sprintf("GATE_3_LOCAL_SHA_NOT_UPLOADED: local HEAD (%s) does not match any uploaded patchset SHA on merged CL #%d", shortSHA(git.HeadSHA), activeCL.Number)
		}
	}

	// Gate 5: Cross-worktree exclusivity (do not check out duplicate branch if already in another slot/worktree)
	if target.ExistingWorktree != nil && target.ExistingWorktree.SymlinkPath != "" {
		if !isSameWorktreePath(target.ExistingWorktree.SymlinkPath, cwd) {
			return "BLOCKED", fmt.Sprintf("GATE_5_OTHER_WORKTREE: target is already tracked in worktree project %q (%s at %s)",
				target.ExistingWorktree.Project, target.ExistingWorktree.Residency, target.ExistingWorktree.SymlinkPath)
		}
	}
	targetBranch := desiredPrepareBranchName(target)
	for _, wt := range gitWTs {
		if wt.Path != "" && !isSameWorktreePath(wt.Path, cwd) && wt.Branch != "" {
			if wt.Branch == targetBranch || (target.TargetType == "bug" && strings.HasPrefix(wt.Branch, "b-"+target.NormalizedID+"-")) {
				return "BLOCKED", fmt.Sprintf("GATE_5_OTHER_GIT_WORKTREE: branch %q is already checked out in worktree %s", wt.Branch, wt.Path)
			}
		}
	}

	return "READY", ""
}

// isSameWorktreePath reports whether pathA and pathB refer to the same worktree
// directory (or a subdirectory within it), resolving symlinks on both sides so
// symlinked worktree paths and physical worktree paths compare equal.
func isSameWorktreePath(wtRootPath, cwd string) bool {
	if wtRootPath == "" || cwd == "" {
		return false
	}
	cleanRoot := filepath.Clean(wtRootPath)
	cleanCWD := filepath.Clean(cwd)
	if cleanRoot == cleanCWD || strings.HasPrefix(cleanCWD, cleanRoot+string(filepath.Separator)) {
		return true
	}
	realRoot := cleanRoot
	if resolved, err := filepath.EvalSymlinks(cleanRoot); err == nil && resolved != "" {
		realRoot = filepath.Clean(resolved)
	}
	realCWD := cleanCWD
	if resolved, err := filepath.EvalSymlinks(cleanCWD); err == nil && resolved != "" {
		realCWD = filepath.Clean(resolved)
	}
	return realRoot == realCWD || strings.HasPrefix(realCWD, realRoot+string(filepath.Separator))
}

func desiredPrepareBranchName(target *SenseTargetState) string {
	if target == nil {
		return ""
	}
	switch target.TargetType {
	case "bug":
		return "b-" + target.NormalizedID
	case "cl":
		return "cl-" + target.NormalizedID
	default:
		return ""
	}
}

func executeSafePrepare(
	ctx context.Context,
	cmd *cobra.Command,
	cfg *Config,
	projCfg *ProjectConfig,
	meta senseProjectMeta,
	cwd string,
	gitState *SenseGitState,
	gitWTs []SenseGitWorktreeEntry,
	activeCL **SenseCLState,
	target *SenseTargetState,
	warnings *senseWarnCollector,
) *SensePrepareState {
	prep := &SensePrepareState{
		Requested:      true,
		PreviousBranch: gitState.Branch,
		PreviousHEAD:   gitState.HeadSHA,
	}

	gateStatus, reason := CanSafelyPrepareWorktree(*gitState, *activeCL, target, cwd, gitWTs)
	if gateStatus != "READY" {
		prep.Status = gateStatus
		prep.BlockedReason = reason
		return prep
	}

	git := cfg.GitClient()
	branchName := desiredPrepareBranchName(target)

	// Gate 4: Check if local branch already exists so we NEVER overwrite it with `-B`
	branchExists, verifyErr := git.VerifyRef(ctx, "refs/heads/"+branchName)
	if verifyErr != nil {
		warnings.addf("verifying local branch %s: %v", branchName, verifyErr)
	}

	if target.TargetType == "bug" {
		if branchExists {
			if err := git.Checkout(ctx, branchName, io.Discard, io.Discard); err != nil {
				prep.Status = "BLOCKED"
				prep.BlockedReason = fmt.Sprintf("git checkout %s failed: %v", branchName, err)
				return prep
			}
			prep.ActionTaken = fmt.Sprintf("switched to existing local branch %s (preserving existing commits)", branchName)
		} else {
			fCtx, cancel := context.WithTimeout(ctx, 4*time.Second)
			if fErr := git.Fetch(fCtx, meta.remote, meta.defaultBranch, io.Discard, io.Discard); fErr != nil {
				warnings.addf("git fetch %s %s failed (creating branch from local %s): %v", meta.remote, meta.defaultBranch, meta.baseRef, fErr)
			}
			cancel()
			if err := git.Run(ctx, io.Discard, io.Discard, "checkout", "-b", branchName, meta.baseRef); err != nil {
				prep.Status = "BLOCKED"
				prep.BlockedReason = fmt.Sprintf("git checkout -b %s %s failed: %v", branchName, meta.baseRef, err)
				return prep
			}
			prep.ActionTaken = fmt.Sprintf("fetched %s and created new branch %s", meta.baseRef, branchName)
		}
	} else if target.TargetType == "cl" {
		if branchExists {
			if err := git.Checkout(ctx, branchName, io.Discard, io.Discard); err != nil {
				prep.Status = "BLOCKED"
				prep.BlockedReason = fmt.Sprintf("git checkout %s failed: %v", branchName, err)
				return prep
			}
			prep.ActionTaken = fmt.Sprintf("switched to existing local branch %s", branchName)
		} else {
			fetchRef := ""
			if target.CL != nil && target.CL.FetchRef != "" {
				fetchRef = target.CL.FetchRef
			}
			if fetchRef == "" {
				prep.Status = "BLOCKED"
				prep.BlockedReason = fmt.Sprintf("could not resolve Gerrit fetch ref for CL %s", target.NormalizedID)
				return prep
			}
			if err := git.Fetch(ctx, meta.remote, fetchRef, io.Discard, io.Discard); err != nil {
				prep.Status = "BLOCKED"
				prep.BlockedReason = fmt.Sprintf("git fetch %s %s failed: %v", meta.remote, fetchRef, err)
				return prep
			}
			if err := git.Run(ctx, io.Discard, io.Discard, "checkout", "-b", branchName, "FETCH_HEAD"); err != nil {
				prep.Status = "BLOCKED"
				prep.BlockedReason = fmt.Sprintf("git checkout -b %s FETCH_HEAD failed: %v", branchName, err)
				return prep
			}
			prep.ActionTaken = fmt.Sprintf("fetched %s and created branch %s", fetchRef, branchName)
		}
	}

	// Refresh gitState after branch preparation
	newGit, _ := senseGitAndWorktrees(ctx, cfg, cwd, meta, warnings)
	if newGit.Branch == "" || newGit.Branch == prep.PreviousBranch {
		newGit.Branch = branchName
	}
	if target.TargetType == "bug" {
		newGit.BugID = meta.issueRef(target.NormalizedID)
		if !branchExists {
			newGit.CommitsAhead = 0
			newGit.CommitsBehind = 0
			newGit.ChangeID = ""
			*activeCL = nil
		} else if newGit.ChangeID != "" && newGit.CommitsAhead > 0 {
			cl, clErr := senseGerritCL(ctx, cmd, cfg, projCfg, meta, newGit.ChangeID, newGit.HeadSHA, cwd, true, warnings)
			if clErr != nil && !isNotFoundError(clErr) {
				warnings.addf("active change %s on prepared branch %s: %v", newGit.ChangeID, branchName, firstLine(clErr.Error()))
			}
			*activeCL = cl
		} else {
			*activeCL = nil
		}
	} else if target.TargetType == "cl" && target.CL != nil {
		*activeCL = target.CL
		newGit.ChangeID = target.CL.ChangeID
	}
	*gitState = newGit
	prep.Status = "PREPARED"
	prep.NewBranch = gitState.Branch
	prep.NewHEAD = gitState.HeadSHA
	return prep
}

func computeRebaseVerdict(git SenseGitState, cl *SenseCLState) (verdict string, reason string) {
	baseRef := git.effectiveBaseRef()
	if git.RebaseInProgress || git.InProgressOp != "" {
		op := git.InProgressOp
		if op == "" {
			op = "rebase"
		}
		if len(git.ConflictedFiles) > 0 {
			return "REBASE_IN_PROGRESS", fmt.Sprintf("git %s in progress; conflicted files: %s", op, strings.Join(git.ConflictedFiles, ", "))
		}
		return "REBASE_IN_PROGRESS", fmt.Sprintf("git %s in progress", op)
	}
	if len(git.MergeConflicts) > 0 || (cl != nil && cl.Status == "NEW" && !cl.Mergeable) {
		if len(git.MergeConflicts) > 0 {
			return "REBASE_CONFLICT", fmt.Sprintf("conflicts with %s in %s", baseRef, strings.Join(git.MergeConflicts, ", "))
		}
		return "REBASE_CONFLICT", "Gerrit reports merge conflict with target branch"
	}
	if git.CommitsBehind == 0 {
		return "UP_TO_DATE", fmt.Sprintf("0 commits behind %s", baseRef)
	}
	if cl != nil && cl.Status == "NEW" {
		hasGreenOrRunningCQ := (cl.VerifiedScore > 0 || cl.PassingChecksCount > 0 || cl.PendingChecksCount > 0) && len(cl.FailingChecks) == 0
		if hasGreenOrRunningCQ {
			return "REBASE_SKIP_KEEP_CQ", fmt.Sprintf("%d commit(s) behind %s but cleanly mergeable; do NOT rebase so active/green CQ checks are preserved", git.CommitsBehind, baseRef)
		}
	}
	if git.IsStale {
		return "REBASE_RECOMMENDED", fmt.Sprintf("%d commit(s) behind %s and cleanly mergeable via git merge-tree", git.CommitsBehind, baseRef)
	}
	return "REBASE_OPTIONAL", fmt.Sprintf("%d commit(s) behind %s (cleanly mergeable)", git.CommitsBehind, baseRef)
}

func inferSingleWorktreeBadge(git SenseGitState, cl *SenseCLState) string {
	if git.RebaseInProgress || git.InProgressOp != "" {
		return "LOCAL_WIP"
	}
	if cl != nil && cl.Status == "NEW" {
		if cl.UnresolvedThreadsCount > 0 || len(cl.FailingChecks) > 0 || cl.CodeReviewScore < 0 || !cl.Mergeable {
			return "NEEDS_ATTENTION"
		}
		if cl.Submittable && cl.PendingChecksCount == 0 {
			return "READY_TO_LAND"
		}
		return "IN_REVIEW"
	}
	if git.IsDirty || git.CommitsAhead > 0 {
		return "LOCAL_WIP"
	}
	if cl != nil && cl.Status == "ABANDONED" {
		return "ABANDONED"
	}
	return "READY_FOR_NEXT"
}

func senseGitAndWorktrees(ctx context.Context, cfg *Config, cwd string, meta senseProjectMeta, warnings *senseWarnCollector) (SenseGitState, []SenseGitWorktreeEntry) {
	st := SenseGitState{
		BaseRef: meta.baseRef,
	}
	wtList := []SenseGitWorktreeEntry{}
	if cfg == nil || cfg.Git == nil {
		return st, wtList
	}

	git := cfg.GitClient()
	runGit := func(args ...string) (string, error) {
		var out, errBuf bytes.Buffer
		err := git.Run(ctx, &out, &errBuf, args...)
		if err != nil && strings.TrimSpace(errBuf.String()) != "" {
			err = fmt.Errorf("%w: %s", err, strings.TrimSpace(errBuf.String()))
		}
		return strings.TrimSpace(out.String()), err
	}

	if br, err := git.CurrentBranch(ctx); err == nil && br != "" {
		st.Branch = br
	} else if br, err := git.RevParse(ctx, "--abbrev-ref", "HEAD"); err == nil {
		st.Branch = br
	}

	if sha, err := git.RevParse(ctx, "HEAD"); err == nil {
		st.HeadSHA = sha
	}

	baseRef := st.effectiveBaseRef()
	if lr, err := runGit("rev-list", "--left-right", "--count", baseRef+"...HEAD"); err == nil && lr != "" {
		parts := strings.Fields(lr)
		if len(parts) == 2 {
			behind, bErr := strconv.Atoi(parts[0])
			ahead, aErr := strconv.Atoi(parts[1])
			if bErr == nil && aErr == nil {
				st.CommitsBehind = behind
				st.CommitsAhead = ahead
			} else {
				warnings.addf("unexpected git rev-list --left-right output %q", lr)
			}
		}
	} else if aheadStr, err := runGit("rev-list", "--count", baseRef+"..HEAD"); err == nil && aheadStr != "" {
		if ahead, aErr := strconv.Atoi(aheadStr); aErr == nil {
			st.CommitsAhead = ahead
		}
	}

	if msg, err := git.CommitMessage(ctx, "HEAD"); err == nil && msg != "" {
		lines := strings.Split(msg, "\n")
		if len(lines) > 0 {
			st.CommitSubject = strings.TrimSpace(lines[0])
		}
		// Only attribute HEAD's Change-Id and Bug trailer to the local branch when CommitsAhead > 0;
		// when CommitsAhead == 0, HEAD is an ancestor of baseRef (an already-merged upstream commit).
		if st.CommitsAhead > 0 {
			if cid := ExtractChangeID(msg); cid != "" {
				st.ChangeID = cid
			}
			if bugs := ExtractBugLinks(msg); len(bugs) > 0 && bugs[0].ID != "" {
				st.BugID = meta.issueRef(bugs[0].ID)
			}
		}
	}
	if st.BugID == "" && st.Branch != "" {
		if id, ok := ExtractIssueIDFromBranchName(st.Branch); ok && id > 0 {
			st.BugID = meta.issueRef(id)
		}
	}

	if ctStr, err := runGit("log", "-1", "--format=%ct", "HEAD"); err == nil && ctStr != "" {
		if ct, parseErr := strconv.ParseInt(ctStr, 10, 64); parseErr == nil && ct > 0 {
			if age := time.Now().Unix() - ct; age > 0 {
				st.CommitAgeSeconds = age
			}
		}
	}

	// In-memory conflict detection via `git merge-tree --write-tree HEAD <baseRef>` when both ahead & behind
	if st.CommitsAhead > 0 && st.CommitsBehind > 0 {
		mtOut, mtErr := runGit("merge-tree", "--write-tree", "HEAD", baseRef)
		if mtErr != nil || strings.Contains(mtOut, "CONFLICT (") {
			for _, m := range mergeTreeConflictRe.FindAllStringSubmatch(mtOut, -1) {
				if len(m) > 1 && strings.TrimSpace(m[1]) != "" {
					st.MergeConflicts = append(st.MergeConflicts, strings.TrimSpace(m[1]))
				}
			}
			if len(st.MergeConflicts) == 0 && mtErr != nil {
				st.MergeConflicts = []string{"(conflict detected by git merge-tree)"}
			}
		}
	}

	var statusOut bytes.Buffer
	if err := git.Run(ctx, &statusOut, io.Discard, "status", "--porcelain"); err == nil {
		raw := strings.TrimRight(statusOut.String(), "\r\n")
		if strings.TrimSpace(raw) != "" {
			lines := strings.Split(raw, "\n")
			st.IsDirty = true
			st.DirtyFilesCount = len(lines)
			for _, line := range lines {
				if len(line) < 3 {
					continue
				}
				xy := line[:2]
				path := strings.TrimSpace(line[2:])
				if path == "" {
					continue
				}
				if len(st.DirtyFiles) < 8 {
					st.DirtyFiles = append(st.DirtyFiles, path)
				}
				switch xy {
				case "UU", "AA", "DD", "AU", "UA", "DU", "UD":
					st.ConflictedFiles = append(st.ConflictedFiles, path)
				}
			}
		}
	}

	if gitDir, err := git.GitDir(ctx); err == nil && gitDir != "" {
		if !filepath.IsAbs(gitDir) && cwd != "" {
			gitDir = filepath.Join(cwd, gitDir)
		}
		for _, item := range []struct {
			marker string
			op     string
		}{
			{"rebase-merge", "rebase"},
			{"rebase-apply", "rebase"},
			{"MERGE_HEAD", "merge"},
			{"CHERRY_PICK_HEAD", "cherry-pick"},
			{"BISECT_LOG", "bisect"},
			{"REVERT_HEAD", "revert"},
		} {
			if _, statErr := os.Stat(filepath.Join(gitDir, item.marker)); statErr == nil {
				st.InProgressOp = item.op
				if item.op == "rebase" {
					st.RebaseInProgress = true
				}
				break
			}
		}
	}

	if wtRaw, err := runGit("worktree", "list", "--porcelain"); err == nil && wtRaw != "" {
		wtList = parseGitWorktreeListPorcelain(wtRaw)
	}
	if wtList == nil {
		wtList = []SenseGitWorktreeEntry{}
	}

	st.IsStale = st.CommitsBehind >= staleBehindThreshold || (st.CommitsBehind > 0 && st.CommitAgeSeconds >= staleAgeSeconds)
	return st, wtList
}

func parseGitWorktreeListPorcelain(raw string) []SenseGitWorktreeEntry {
	var list []SenseGitWorktreeEntry
	var current *SenseGitWorktreeEntry
	for _, line := range strings.Split(raw, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			if current != nil && current.Path != "" {
				list = append(list, *current)
			}
			current = nil
			continue
		}
		if strings.HasPrefix(line, "worktree ") {
			if current != nil && current.Path != "" {
				list = append(list, *current)
			}
			current = &SenseGitWorktreeEntry{
				Path: strings.TrimSpace(strings.TrimPrefix(line, "worktree ")),
			}
		} else if current != nil {
			if strings.HasPrefix(line, "HEAD ") {
				current.Head = strings.TrimSpace(strings.TrimPrefix(line, "HEAD "))
			} else if strings.HasPrefix(line, "branch ") {
				br := strings.TrimSpace(strings.TrimPrefix(line, "branch "))
				current.Branch = strings.TrimPrefix(br, "refs/heads/")
			}
		}
	}
	if current != nil && current.Path != "" {
		list = append(list, *current)
	}
	return list
}

func isRealGitRunner(cfg *Config) bool {
	if cfg == nil || cfg.Git == nil {
		return false
	}
	_, isReal := cfg.Git.(*RealGitRunner)
	return isReal
}

func isMockWithoutLocalIssueServer(cfg *Config, client *IssueTrackerClient) bool {
	if !isRealGitRunner(cfg) {
		if client == nil || (!strings.HasPrefix(client.Endpoint, "http://127.0.0.1") && !strings.HasPrefix(client.Endpoint, "http://localhost")) {
			return true
		}
	}
	return false
}

func senseBugTarget(
	ctx context.Context,
	cmd *cobra.Command,
	cfg *Config,
	meta senseProjectMeta,
	projCfg *ProjectConfig,
	targetBugID int64,
	warnings *senseWarnCollector,
) (*SenseIssueInfo, error) {
	client, cErr := NewIssueTrackerClientForCommand(ctx, cmd)
	if cErr != nil {
		return nil, cErr
	}
	if client == nil || isMockWithoutLocalIssueServer(cfg, client) {
		return nil, nil
	}

	var (
		wg         sync.WaitGroup
		info       *SenseIssueInfo
		issueErr   error
		comments   []SenseIssueComment
		relatedCLs []SenseRelatedCL
	)

	wg.Add(2)
	go func() {
		defer wg.Done()
		iss, iErr := client.GetIssue(ctx, targetBugID)
		if iErr != nil {
			issueErr = iErr
			return
		}
		if iss == nil {
			return
		}
		desc := ""
		if eff := iss.EffectiveDescription(); eff != nil {
			desc = SanitizeUntrustedText(eff.Comment)
		}
		assignee := ""
		if iss.State.Assignee != nil {
			assignee = iss.State.Assignee.EmailAddress
		}
		stateStr := "OPEN"
		switch strings.ToUpper(strings.TrimSpace(iss.State.Status)) {
		case "FIXED", "VERIFIED", "WONT_FIX", "INFEASIBLE", "OBSOLETE", "INTENDED_BEHAVIOR", "DUPLICATE", "NOT_REPRODUCIBLE", "CLOSED":
			stateStr = "CLOSED"
		}
		info = &SenseIssueInfo{
			Number:      int64(iss.IssueID),
			Title:       SanitizeUntrustedText(iss.State.Title),
			State:       stateStr,
			Priority:    iss.State.Priority,
			Assignee:    assignee,
			Description: truncateLines(desc, 25, 1200),
			URL:         meta.issueWebURL(ctx, cfg, projCfg, int64(iss.IssueID)),
		}
	}()

	go func() {
		defer wg.Done()
		cResp, err := client.ListComments(ctx, targetBugID, 20, "")
		if err != nil {
			if !isNotFoundError(err) {
				warnings.addf("issue %s: failed to load comments: %v", meta.issueRef(targetBugID), firstLine(err.Error()))
			}
			return
		}
		if cResp == nil {
			return
		}
		for i, c := range cResp.IssueComments {
			if c.CommentNumber == 1 || (c.CommentNumber == 0 && i == 0) {
				continue
			}
			body := strings.TrimSpace(SanitizeUntrustedText(c.Comment))
			if body == "" {
				continue
			}
			author := c.EffectiveAuthorEmail()
			if author == "unknown" {
				author = ""
			}
			// Filter out low-signal bot comments (auto-assigner & gitwatcher HTML dumps,
			// since RelatedCLs already surfaces linked Gerrit changes cleanly).
			lowerAuthor := strings.ToLower(author)
			if strings.HasPrefix(lowerAuthor, "blunderbuss-") ||
				strings.HasPrefix(lowerAuthor, "dx-workflow-gitwatcher@") ||
				strings.HasPrefix(body, "Automated g4 rollback") {
				continue
			}
			created := ""
			if !c.CreatedTime.IsZero() {
				created = c.CreatedTime.Format("2006-01-02")
			}
			comments = append(comments, SenseIssueComment{
				Number:    c.CommentNumber,
				Author:    author,
				CreatedAt: created,
				Message:   truncateLines(body, 12, 600),
			})
		}
		sort.Slice(comments, func(i, j int) bool {
			return comments[i].Number < comments[j].Number
		})
	}()

	// Also query Gerrit for any existing CLs referencing this issue (`message:<id>`)
	if cfg != nil && cfg.Git == DefaultGitRunner {
		wg.Add(1)
		go func() {
			defer wg.Done()
			qCtx, cancel := context.WithTimeout(ctx, 2500*time.Millisecond)
			defer cancel()
			gClient, gErr := NewGerritClient(qCtx, cmd)
			if gErr != nil || gClient == nil {
				if gErr != nil {
					warnings.addf("issue %s: skipping related Gerrit CL lookup: %v", meta.issueRef(targetBugID), firstLine(gErr.Error()))
				}
				return
			}
			changes, _, qErr := gClient.Changes.QueryChanges(qCtx, &gerrit.QueryChangeOptions{
				QueryOptions: gerrit.QueryOptions{
					Query: []string{fmt.Sprintf("message:%d", targetBugID)},
					Limit: 5,
				},
			})
			if qErr != nil {
				warnings.addf("issue %s: related Gerrit CL query failed: %v", meta.issueRef(targetBugID), firstLine(qErr.Error()))
				return
			}
			if changes != nil {
				for _, ch := range *changes {
					relatedCLs = append(relatedCLs, SenseRelatedCL{
						Number:   ch.Number,
						ShortRef: meta.clRef(ch.Number),
						Status:   ch.Status,
						Subject:  ch.Subject,
						IsWIP:    ch.WorkInProgress,
					})
				}
			}
		}()
	}

	wg.Wait()
	if issueErr != nil {
		return nil, issueErr
	}
	if info != nil {
		info.Comments = comments
		info.RelatedCLs = relatedCLs
	}
	return info, nil
}

// resolveCurrentUserIdentity resolves the user's email and short username from Git config,
// environment variables, or the OS user entry.
func resolveCurrentUserIdentity(ctx context.Context, cfg *Config) (email string, login string) {
	if cfg != nil && cfg.Git != nil {
		if em, err := cfg.GitClient().UserEmail(ctx); err == nil && strings.TrimSpace(em) != "" {
			email = strings.TrimSpace(em)
		}
	}
	if email != "" {
		if before, _, ok := strings.Cut(email, "@"); ok && before != "" {
			login = before
		}
	}
	if login == "" {
		login = strings.TrimSpace(os.Getenv("USER"))
	}
	if login == "" {
		if u, err := user.Current(); err == nil && u != nil {
			login = strings.TrimSpace(u.Username)
		}
	}
	return email, login
}

func senseCandidateIssues(ctx context.Context, cmd *cobra.Command, cfg *Config, warnings *senseWarnCollector) []SenseIssueSummary {
	client, err := NewIssueTrackerClientForCommand(ctx, cmd)
	if err != nil || client == nil || isMockWithoutLocalIssueServer(cfg, client) {
		if err != nil && cfg != nil && cfg.Git == DefaultGitRunner {
			warnings.addf("candidate issues client init failed: %v", firstLine(err.Error()))
		}
		return nil
	}

	qCtx, cancel := context.WithTimeout(ctx, 5000*time.Millisecond)
	defer cancel()

	email, _ := resolveCurrentUserIdentity(qCtx, cfg)
	if email == "" {
		return nil
	}

	resp, err := client.ListIssues(qCtx, fmt.Sprintf("status:open assignee:%s", email), 5, "")
	if (err != nil || resp == nil || len(resp.Issues) == 0) && qCtx.Err() == nil {
		if repResp, repErr := client.ListIssues(qCtx, fmt.Sprintf("status:open reporter:%s", email), 5, ""); repErr == nil && repResp != nil {
			resp = repResp
			err = nil
		}
	}
	if err != nil {
		warnings.addf("candidate open issues query failed: %v", firstLine(err.Error()))
		return nil
	}
	if resp == nil {
		return nil
	}
	var out []SenseIssueSummary
	for _, iss := range resp.Issues {
		if iss == nil {
			continue
		}
		out = append(out, SenseIssueSummary{
			Number:   int64(iss.IssueID),
			Priority: iss.State.Priority,
			Title:    SanitizeUntrustedText(iss.State.Title),
		})
	}
	return out
}

func extractChangeOwner(change *gerrit.ChangeInfo) string {
	if change == nil {
		return ""
	}
	if change.Owner.Email != "" {
		return change.Owner.Email
	}
	if change.Owner.Username != "" {
		return change.Owner.Username
	}
	return change.Owner.Name
}

func extractChangeRevisionInfo(change *gerrit.ChangeInfo, localHeadSHA string) (patchsetNum int, fetchRef string, headUploaded bool) {
	if change == nil {
		return 0, "", false
	}
	if change.Revisions != nil {
		if localHeadSHA != "" {
			if _, exists := change.Revisions[localHeadSHA]; exists {
				headUploaded = true
			}
		}
		if change.CurrentRevision != "" {
			if rev, ok := change.Revisions[change.CurrentRevision]; ok {
				patchsetNum = rev.Number
				fetchRef = rev.Ref
			}
		}
	}
	if fetchRef == "" && change.Number > 0 && patchsetNum > 0 {
		mod := change.Number % 100
		fetchRef = fmt.Sprintf("refs/changes/%02d/%d/%d", mod, change.Number, patchsetNum)
	}
	return patchsetNum, fetchRef, headUploaded
}

func fetchCommentsMapWithFallback(
	primaryID string,
	fallbackID string,
	fetchFn func(id string) (*map[string][]gerrit.CommentInfo, *gerrit.Response, error),
) (map[string][]gerrit.CommentInfo, error) {
	cMap, _, err := fetchFn(primaryID)
	if err == nil && cMap != nil {
		return *cMap, nil
	}
	if fallbackID != "" && fallbackID != primaryID {
		if cMap2, _, err2 := fetchFn(fallbackID); err2 == nil && cMap2 != nil {
			return *cMap2, nil
		}
	}
	return nil, err
}

func senseGerritCL(
	ctx context.Context,
	cmd *cobra.Command,
	cfg *Config,
	projCfg *ProjectConfig,
	meta senseProjectMeta,
	rawTarget string,
	localHeadSHA string,
	cwd string,
	isCheckedOut bool,
	warnings *senseWarnCollector,
) (*SenseCLState, error) {
	parsed := ParseChangeTarget(rawTarget)
	if parsed.ChangeID == "" {
		return nil, fmt.Errorf("empty change identifier from %q", rawTarget)
	}
	client, err := NewGerritClient(ctx, cmd)
	if err != nil || client == nil {
		return nil, err
	}

	gHost := parsed.Host
	if gHost == "" && cfg != nil {
		gHost = cfg.GerritHost(ctx)
	}
	gHost = CanonicalGerritHost(gHost)

	opt := &gerrit.ChangeOptions{
		AdditionalFields: []string{"DETAILED_LABELS", "ALL_REVISIONS", "DETAILED_ACCOUNTS", "SUBMITTABLE", "SUBMIT_REQUIREMENTS"},
	}
	change, _, err := client.Changes.GetChange(ctx, parsed.ChangeID, opt)
	if err != nil {
		return nil, FormatGerritError(err, "getting", parsed.ChangeID, gHost)
	}
	if change == nil || change.Number == 0 {
		return nil, nil
	}

	ownerEmail := extractChangeOwner(change)
	patchsetNum, fetchRef, headUploaded := extractChangeRevisionInfo(change, localHeadSHA)

	var (
		subWg               sync.WaitGroup
		publishedComments   map[string][]gerrit.CommentInfo
		draftComments       map[string][]gerrit.CommentInfo
		failingChecks       = []CheckItem{}
		failingCheckDetails []SenseFailingCheckDetail
		pendingCount        int
		passingCount        int
		checksSummary       string
	)

	// Skip comments, drafts, and CI build queries when a CL is already MERGED or ABANDONED
	if change.Status != "MERGED" && change.Status != "ABANDONED" {
		numStr := strconv.Itoa(change.Number)
		changeIDStr := numStr
		if change.ChangeID != "" {
			changeIDStr = change.ChangeID
		}

		subWg.Add(2)
		go func() {
			defer subWg.Done()
			var cErr error
			publishedComments, cErr = fetchCommentsMapWithFallback(changeIDStr, numStr, func(id string) (*map[string][]gerrit.CommentInfo, *gerrit.Response, error) {
				return client.Changes.ListChangeComments(ctx, id)
			})
			if cErr != nil {
				warnings.addf("CL #%d: failed to load comments: %v", change.Number, firstLine(cErr.Error()))
			}
		}()

		go func() {
			defer subWg.Done()
			var dErr error
			draftComments, dErr = fetchCommentsMapWithFallback(changeIDStr, numStr, func(id string) (*map[string][]gerrit.CommentInfo, *gerrit.Response, error) {
				return client.Changes.ListChangeDrafts(ctx, id)
			})
			if dErr != nil {
				warnings.addf("CL #%d: failed to load drafts: %v", change.Number, firstLine(dErr.Error()))
			}
		}()

		if patchsetNum > 0 && change.Project != "" && gHost != "" {
			patchsets := EquivalentPatchsets(change, patchsetNum)
			subWg.Add(1)
			go func() {
				defer subWg.Done()
				provider := ResolveCIProvider(cfg, projCfg, client, gHost, change)
				builds, bErr := SearchProviderBuildsForPatchsets(ctx, provider, gHost, change.Number, patchsets, change)
				if bErr != nil {
					if isRealGitRunner(cfg) || !isNotFoundError(bErr) {
						warnings.addf("CL #%d: failed to query CI checks: %v", change.Number, firstLine(bErr.Error()))
					}
					return
				}
				if builds == nil {
					return
				}
				deduped := deduplicateLatestBuilds(builds)
				visibleBuilds, _ := FilterBuildsByTags(deduped, projCfg.CI.HideTagFilters)
				checksSummary = formatCheckSummary(visibleBuilds)
				var failedBuilds []bbBuild
				for _, b := range visibleBuilds {
					if b.IsExperimental() {
						continue
					}
					item := NewCheckItem(b)
					switch classifyCheckStatus(b.Status) {
					case checkPassed:
						passingCount++
					case checkPending:
						pendingCount++
					case checkFailed:
						failingChecks = append(failingChecks, item)
						failedBuilds = append(failedBuilds, b)
					}
				}
				if len(failedBuilds) > 0 && provider != nil {
					limit := len(failedBuilds)
					if limit > maxInlineFailingBuilds {
						limit = maxInlineFailingBuilds
					}
					logCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
					defer cancel()
					for i := 0; i < limit; i++ {
						fb := failedBuilds[i]
						rep, fErr := provider.FetchFailureReportWithOptions(logCtx, fb, FailureReportOptions{
							MaxLogLines:            maxInlineLogLines,
							PreferredLogs:          projCfg.PreferredLogs(),
							IncludeSummaryMarkdown: true,
						})
						if fErr != nil {
							warnings.addf("CL #%d builder %s: failed to fetch failure log: %v", change.Number, fb.Builder.Builder, firstLine(fErr.Error()))
							continue
						}
						if rep != nil {
							det := SenseFailingCheckDetail{
								Builder:    fb.Builder.Builder,
								BuildID:    fb.ID,
								FailedStep: rep.FailedStep,
							}
							if rep.LogSnippet != "" {
								det.LogExcerpt = truncateLines(strings.TrimSpace(rep.LogSnippet), maxInlineLogLines, 1200)
							} else if rep.StepSummary != "" {
								det.LogExcerpt = truncateLines(strings.TrimSpace(rep.StepSummary), 10, 600)
							} else if rep.BuildSummary != "" {
								det.LogExcerpt = truncateLines(strings.TrimSpace(rep.BuildSummary), 10, 600)
							}
							if det.FailedStep != "" || det.LogExcerpt != "" {
								failingCheckDetails = append(failingCheckDetails, det)
							}
						}
					}
				}
			}()
		}

		subWg.Wait()
	}

	commentsSummary := AnalyzeComments(publishedComments, draftComments)
	externalUnresolved := commentsSummary.Unresolved
	authorDrafts := commentsSummary.Drafts
	if isCheckedOut {
		externalUnresolved = enrichCommentsWithLocalCode(cwd, externalUnresolved)
		authorDrafts = enrichCommentsWithLocalCode(cwd, authorDrafts)
	}

	crScore := extractLabelScore(change.Labels, "Code-Review")
	vScore := extractLabelScore(change.Labels, "Verified")
	if _, hasVerified := change.Labels["Verified"]; !hasVerified {
		vScore = extractLabelScore(change.Labels, "Presubmit-Verified")
	}
	cqScore := extractLabelScore(change.Labels, "Commit-Queue")

	mergeable := true
	if !change.Mergeable {
		for _, sr := range change.SubmitRequirements {
			if strings.Contains(strings.ToLower(sr.Name), "merge") && strings.EqualFold(sr.Status, "UNSATISFIED") {
				mergeable = false
				break
			}
		}
	}

	return &SenseCLState{
		Number:                    change.Number,
		Subject:                   change.Subject,
		Status:                    change.Status,
		Owner:                     ownerEmail,
		ChangeID:                  change.ChangeID,
		URL:                       meta.clWebURL(ctx, cfg, gHost, change.Project, change.Number),
		ShortRef:                  meta.clRef(change.Number),
		FetchRef:                  fetchRef,
		HeadUploadedToGerrit:      headUploaded,
		IsWIP:                     change.WorkInProgress,
		Submittable:               change.Submittable,
		Mergeable:                 mergeable,
		CodeReviewScore:           crScore,
		VerifiedScore:             vScore,
		CommitQueueScore:          cqScore,
		UnresolvedThreadsCount:    commentsSummary.UnresolvedThreads,
		DraftsCount:               commentsSummary.DraftsCount,
		ExternalUnresolvedThreads: externalUnresolved,
		AuthorSelfDrafts:          authorDrafts,
		ChecksSummary:             checksSummary,
		FailingChecks:             failingChecks,
		FailingCheckDetails:       failingCheckDetails,
		PendingChecksCount:        pendingCount,
		PassingChecksCount:        passingCount,
		Blockers:                  extractBlockers(change),
	}, nil
}

func enrichCommentsWithLocalCode(cwd string, items []UnresolvedComment) []UnresolvedComment {
	if len(items) == 0 || cwd == "" {
		return items
	}
	out := make([]UnresolvedComment, len(items))
	copy(out, items)
	for i := range out {
		if out[i].Line <= 0 || out[i].File == "" || strings.HasPrefix(out[i].File, "/") {
			continue
		}
		fullPath := filepath.Join(cwd, filepath.FromSlash(out[i].File))
		data, err := os.ReadFile(fullPath)
		if err != nil {
			continue
		}
		lines := strings.Split(string(data), "\n")
		lineIdx := out[i].Line - 1
		if lineIdx < 0 || lineIdx >= len(lines) {
			continue
		}
		start := lineIdx - 2
		if start < 0 {
			start = 0
		}
		end := lineIdx + 2
		if end >= len(lines) {
			end = len(lines) - 1
		}
		var snip []string
		for idx := start; idx <= end; idx++ {
			prefix := "  "
			if idx == lineIdx {
				prefix = "> "
			}
			snip = append(snip, fmt.Sprintf("%sL%d: %s", prefix, idx+1, lines[idx]))
		}
		out[i].CodeSnippet = strings.Join(snip, "\n")
	}
	return out
}

func senseOncall(ctx context.Context, cfg *Config, projCfg *ProjectConfig, warnings *senseWarnCollector) SenseOncallState {
	_, userLogin := resolveCurrentUserIdentity(ctx, cfg)
	res := SenseOncallState{UserLogin: userLogin}

	scheduleFile := strings.TrimSpace(os.Getenv("GH_ISH_ONCALL_FILE"))
	if scheduleFile == "" && projCfg != nil {
		scheduleFile = strings.TrimSpace(projCfg.Oncall.ScheduleFile)
	}
	if scheduleFile == "" {
		return res
	}

	data, err := os.ReadFile(scheduleFile)
	if err != nil {
		warnings.addf("failed to read configured oncall schedule %s: %v", scheduleFile, err)
		return res
	}
	content := string(data)
	if m := primaryOncallRegex.FindStringSubmatch(content); len(m) > 1 {
		res.PrimaryOncall = m[1]
	}
	if m := secondaryOncallRegex.FindStringSubmatch(content); len(m) > 1 {
		res.SecondaryOncall = m[1]
	}
	if userLogin != "" {
		if strings.EqualFold(userLogin, res.PrimaryOncall) {
			res.IsUserOncall = true
			res.Role = "primary"
		} else if strings.EqualFold(userLogin, res.SecondaryOncall) {
			res.IsUserOncall = true
			res.Role = "secondary"
		}
	}
	return res
}

func classifySenseModality(
	git SenseGitState,
	wt SenseWorktreeState,
	activeCL *SenseCLState,
	target *SenseTargetState,
	oncall SenseOncallState,
	candidateCount int,
) (modality string, confidence string, summary string, steps []string, gates []string) {
	defaultMeta := senseProjectMeta{
		profileName:       "pigweed",
		remote:            "origin",
		defaultBranch:     "main",
		baseRef:           git.effectiveBaseRef(),
		issuePrefix:       "b/",
		clShortlinkPrefix: "pwrev/",
		projectLabel:      "Pigweed",
	}
	return classifySenseModalityWithMeta(git, wt, activeCL, target, oncall, candidateCount, defaultMeta)
}

func classifySenseModalityWithMeta(
	git SenseGitState,
	wt SenseWorktreeState,
	activeCL *SenseCLState,
	target *SenseTargetState,
	oncall SenseOncallState,
	candidateCount int,
	meta senseProjectMeta,
) (modality string, confidence string, summary string, steps []string, gates []string) {
	baseRef := git.effectiveBaseRef()

	// Tier 0: Rebase or git operation in progress
	if git.RebaseInProgress || git.InProgressOp != "" {
		op := git.InProgressOp
		if op == "" {
			op = "rebase"
		}
		fmtHint := "format changed files"
		if meta.localCheckHint != "" {
			fmtHint = "`" + meta.localCheckHint + "`"
		}
		return "RESUME_CONFLICT_RESOLUTION",
			"HIGH",
			fmt.Sprintf("Git %s in progress on `%s` with unresolved conflicts.", op, git.Branch),
			[]string{
				fmt.Sprintf("Resolve conflicted files (`/freshen`), run affected tests + %s, and complete `git %s --continue`.", fmtHint, op),
			},
			nil
	}

	// Tier 1: Explicit target argument
	if target != nil {
		if target.FetchError != "" {
			if target.TargetType == "bug" {
				issRef := meta.issueRef(target.NormalizedID)
				return "CRANK_BUG",
					"LOW",
					fmt.Sprintf("Failed to fetch issue %s: %s", issRef, target.FetchError),
					[]string{
						fmt.Sprintf("Check authentication (`./gh auth status`) or inspect `%s` via `./gh issue view %s`.", issRef, target.NormalizedID),
					},
					[]string{"GATE_7_TARGET_FETCH_FAILED"}
			}
			if target.TargetType == "cl" {
				return "ADOPT_CL",
					"LOW",
					fmt.Sprintf("Failed to fetch Gerrit CL %s: %s", target.NormalizedID, target.FetchError),
					[]string{
						fmt.Sprintf("Check authentication (`./gh auth status`) or inspect CL %s via `./gh pr view %s`.", target.NormalizedID, target.NormalizedID),
					},
					[]string{"GATE_7_TARGET_FETCH_FAILED"}
			}
		}

		if target.NotFound {
			if target.TargetType == "bug" {
				issRef := meta.issueRef(target.NormalizedID)
				return "CRANK_BUG",
					"LOW",
					fmt.Sprintf("Target issue %s could not be found (or requires authentication).", issRef),
					[]string{
						fmt.Sprintf("Verify issue ID %s or run `./gh issue view %s`.", issRef, target.NormalizedID),
					},
					[]string{"GATE_7_TARGET_NOT_FOUND"}
			}
			if target.TargetType == "cl" {
				return "ADOPT_CL",
					"LOW",
					fmt.Sprintf("Target Gerrit CL %s could not be found on Gerrit.", target.NormalizedID),
					[]string{
						fmt.Sprintf("Verify CL number %s or run `./gh pr view %s`.", target.NormalizedID, target.NormalizedID),
					},
					[]string{"GATE_7_TARGET_NOT_FOUND"}
			}
		}

		if target.TargetType == "bug" {
			bugID := target.NormalizedID
			issRef := meta.issueRef(bugID)
			title := fmt.Sprintf("Issue %s", issRef)
			state := "UNKNOWN"
			var openRelatedCL *SenseRelatedCL
			activeCLMatchesBug := activeCL != nil && activeCL.Status == "NEW" && branchTracksBug(git, bugID)

			if target.Issue != nil {
				if target.Issue.Title != "" {
					title = target.Issue.Title
				}
				if target.Issue.State != "" {
					state = target.Issue.State
				}
				for i := range target.Issue.RelatedCLs {
					rcl := &target.Issue.RelatedCLs[i]
					if activeCL != nil && activeCL.Number == rcl.Number && activeCL.Status == "NEW" {
						activeCLMatchesBug = true
					}
					if rcl.Status == "NEW" && openRelatedCL == nil {
						openRelatedCL = rcl
					}
				}
			}

			// If the current branch is already on the open Gerrit CL for this issue, fall through to Tier 2 (DRIVE_ACTIVE_CL).
			if !(activeCLMatchesBug && activeCL != nil) {
				if state == "CLOSED" {
					return "CRANK_BUG",
						"HIGH",
						fmt.Sprintf("Target issue %s (%s) is already CLOSED.", issRef, title),
						[]string{
							fmt.Sprintf("No code changes needed — %s is already CLOSED. Report status and linked CL(s) to the user.", issRef),
						},
						[]string{"GATE_7_BUG_ALREADY_CLOSED"}
				}

				var s []string
				if target.ExistingWorktree != nil {
					ex := target.ExistingWorktree
					if ex.Residency == "PARKED" {
						s = append(s, fmt.Sprintf("Mount parked worktree: `./gh wt use %s --json` (path: `%s`).", ex.Project, ex.SymlinkPath))
					} else {
						s = append(s, fmt.Sprintf("Use existing mounted worktree `%s` at `%s`.", ex.Project, ex.SymlinkPath))
					}
				} else if openRelatedCL != nil {
					clRef := meta.clRef(openRelatedCL.Number)
					if git.IsDirty || (activeCL != nil && activeCL.Status == "NEW" && activeCL.Number != openRelatedCL.Number) {
						if wt.isPoolManaged() {
							s = append(s, fmt.Sprintf("Existing open CL %s (%s) is linked to %s; mount it in a slot via `./gh wt use cl-%d --cl %d --json`.", clRef, openRelatedCL.Subject, issRef, openRelatedCL.Number, openRelatedCL.Number))
						} else {
							s = append(s, fmt.Sprintf("Existing open CL %s (%s) is linked to %s; commit or stash current changes first, then check it out via `./gh pr checkout %d`.", clRef, openRelatedCL.Subject, issRef, openRelatedCL.Number))
						}
					} else {
						s = append(s, fmt.Sprintf("Existing open CL %s (%s) is linked to %s; check it out via `./gh pr checkout %d` instead of creating a duplicate CL.", clRef, openRelatedCL.Subject, issRef, openRelatedCL.Number))
					}
				} else if git.IsDirty || (activeCL != nil && activeCL.Status == "NEW") {
					if wt.isPoolManaged() {
						s = append(s, fmt.Sprintf("Current worktree has active work; allocate slot via `./gh wt use --issue %s --json` (or switch branch if clean).", bugID))
					} else {
						s = append(s, fmt.Sprintf("Current worktree has active work; commit or stash current changes (or use another worktree) before creating a branch for %s.", issRef))
					}
				} else if !branchTracksBug(git, bugID) {
					s = append(s, fmt.Sprintf("Prepare branch (`./gh sense --prepare %s` or `git checkout -b b-%s %s`).", issRef, bugID, baseRef))
				}

				if openRelatedCL != nil {
					clRef := meta.clRef(openRelatedCL.Number)
					s = append(s, fmt.Sprintf("Drive existing CL %s for %s: address comments/CI, push (`./gh pr push --cq`), watch CQ (`./gh pr checks %d --watch --fail-fast`), then `./gh pr ready %d --owner`.", clRef, issRef, openRelatedCL.Number, openRelatedCL.Number))
				} else {
					if target.Issue != nil && len(target.Issue.RelatedCLs) > 0 {
						s = append(s, fmt.Sprintf("Inspect %d historical Gerrit CL(s) linked to %s for context.", len(target.Issue.RelatedCLs), issRef))
					}
					s = append(s,
						fmt.Sprintf("Fix %s + add unit tests, run `/review` self-gate, commit (`Fixed: %s`), upload (`./gh pr create --cq --draft`), watch CQ (`./gh pr checks --watch --fail-fast`), then `./gh pr ready --owner`.", issRef, issRef),
					)
				}
				return "CRANK_BUG",
					"HIGH",
					fmt.Sprintf("Target is issue %s: %s (state: %s).", issRef, title, state),
					s,
					nil
			}
		}

		if target.TargetType == "cl" {
			clID := target.NormalizedID
			isCurrentWorktreeCL := isSameGerritCL(target, activeCL, git.ChangeID)
			if isCurrentWorktreeCL && activeCL == nil && target.CL != nil {
				activeCL = target.CL
			}
			if target.CL != nil && (target.CL.Status == "MERGED" || target.CL.Status == "ABANDONED") && !isCurrentWorktreeCL {
				return "ADOPT_CL",
					"HIGH",
					fmt.Sprintf("Target Gerrit CL %s (%s) is already %s.", clID, target.CL.Subject, target.CL.Status),
					[]string{
						fmt.Sprintf("No action required — CL %s is already %s.", clID, target.CL.Status),
					},
					nil
			}
			if !isCurrentWorktreeCL || activeCL == nil || activeCL.Status != "NEW" {
				clTitle := fmt.Sprintf("CL %s", clID)
				if target.CL != nil && target.CL.Subject != "" {
					clTitle = target.CL.Subject
				} else if activeCL != nil && activeCL.Subject != "" {
					clTitle = activeCL.Subject
				}
				var s []string
				if target.ExistingWorktree != nil {
					ex := target.ExistingWorktree
					if ex.Residency == "PARKED" {
						s = append(s, fmt.Sprintf("Mount parked worktree: `./gh wt use %s --json`.", ex.Project))
					} else {
						s = append(s, fmt.Sprintf("Operate in mounted worktree `%s` (`%s`).", ex.Project, ex.SymlinkPath))
					}
				} else if git.IsDirty || (activeCL != nil && activeCL.Status == "NEW" && strconv.Itoa(activeCL.Number) != clID) {
					if wt.isPoolManaged() {
						s = append(s, fmt.Sprintf("Mount CL %s in a warm slot: `./gh wt use cl-%s --cl %s --json`.", clID, clID, clID))
					} else {
						s = append(s, fmt.Sprintf("Current worktree has active work; commit or stash current changes (or use another worktree) before running `./gh pr checkout %s`.", clID))
					}
				} else {
					s = append(s, fmt.Sprintf("Check out CL %s (`./gh sense --prepare %s` or `./gh pr checkout %s`).", clID, clID, clID))
				}
				s = append(s,
					"Address comments/drafts (`/respond`), rebase if needed (`/freshen`), push (`./gh pr push --cq`), watch CQ (`./gh pr checks --watch --fail-fast`), then publish drafts (`./gh pr review --publish`).",
				)
				return "ADOPT_CL",
					"HIGH",
					fmt.Sprintf("Adopt Gerrit CL %s (%s) and drive it to CQ pass and review readiness.", clID, clTitle),
					s,
					nil
			}
		}

		if target.TargetType == "chat_thread" {
			spaceID := target.ChatSpaceID
			threadID := target.ChatThreadID
			readCmd := fmt.Sprintf("readonly read-thread --space %s --thread %s", spaceID, threadID)
			if threadID == "" {
				readCmd = fmt.Sprintf("readonly list-messages --space %s --max 20", spaceID)
			}
			s := []string{
				fmt.Sprintf("Read chat thread via `gchat` (`%s`), extract linked issue or CL identifier, and route to `CRANK_BUG` or `ADOPT_CL` (read-only; never post to Chat).", readCmd),
			}
			return "CRANK_CHAT_THREAD",
				"HIGH",
				fmt.Sprintf("Target is Google Chat thread `%s` (space: `%s`, thread: `%s`).", target.NormalizedID, spaceID, threadID),
				s,
				nil
		}
	}

	// Tier 2: Active open Gerrit CL on current branch
	if activeCL != nil && activeCL.Status == "NEW" {
		clNum := activeCL.Number
		var s []string
		var summaryParts []string

		if git.RebaseVerdict == "REBASE_CONFLICT" || git.RebaseVerdict == "REBASE_RECOMMENDED" {
			summaryParts = append(summaryParts, fmt.Sprintf("%s (%s)", git.RebaseVerdict, git.RebaseReason))
			s = append(s, fmt.Sprintf("Run `/freshen` to rebase CL %d onto `%s`.", clNum, baseRef))
		}

		if len(activeCL.AuthorSelfDrafts) > 0 {
			summaryParts = append(summaryParts, fmt.Sprintf("%d author self-draft(s)", len(activeCL.AuthorSelfDrafts)))
			s = append(s, fmt.Sprintf("Implement %d author self-draft note(s) via `/respond` and delete via `--delete-draft`.", len(activeCL.AuthorSelfDrafts)))
		}

		if len(activeCL.ExternalUnresolvedThreads) > 0 {
			summaryParts = append(summaryParts, fmt.Sprintf("%d unresolved thread(s)", len(activeCL.ExternalUnresolvedThreads)))
			s = append(s, fmt.Sprintf("Address or push back on %d unresolved reviewer thread(s) via `/respond` (stage with `--draft`).", len(activeCL.ExternalUnresolvedThreads)))
		}

		if len(activeCL.FailingChecks) > 0 {
			var failNames []string
			for i, fc := range activeCL.FailingChecks {
				if i >= 3 {
					break
				}
				failNames = append(failNames, fc.Name)
			}
			summaryParts = append(summaryParts, fmt.Sprintf("%d failing check(s) (%s)", len(activeCL.FailingChecks), strings.Join(failNames, ", ")))
			s = append(s, fmt.Sprintf("Fix failing CI checks on CL %d (or `./gh run rerun %d --failed` for infra flakes).", clNum, clNum))
		}

		if git.IsDirty {
			summaryParts = append(summaryParts, fmt.Sprintf("%d dirty file(s)", git.DirtyFilesCount))
			s = append(s, "Format, run `/review` self-gate, and amend into HEAD preserving `Change-Id`.")
		}

		if len(s) > 0 {
			s = append(s,
				fmt.Sprintf("Push (`./gh pr push --cq`) and watch CQ (`./gh pr checks %d --watch --fail-fast`).", clNum),
			)
			if len(activeCL.ExternalUnresolvedThreads) > 0 || activeCL.DraftsCount > 0 {
				s = append(s, fmt.Sprintf("Once CQ+1 passes, publish draft replies (`./gh pr review %d --publish`).", clNum))
			}
			return "DRIVE_ACTIVE_CL",
				"HIGH",
				fmt.Sprintf("Active CL #%d (%s) needs action: %s.", clNum, activeCL.Subject, strings.Join(summaryParts, "; ")),
				s,
				nil
		}

		if activeCL.PendingChecksCount > 0 {
			return "DRIVE_ACTIVE_CL",
				"HIGH",
				fmt.Sprintf("Active CL #%d (%s) has %d CI check(s) in progress (%d passing).", clNum, activeCL.Subject, activeCL.PendingChecksCount, activeCL.PassingChecksCount),
				[]string{
					fmt.Sprintf("./gh pr checks %d --watch --fail-fast", clNum),
					fmt.Sprintf("Once green: `./gh pr review %d --publish` (if drafts) and `./gh pr ready %d --owner` (if WIP).", clNum, clNum),
				},
				nil
		}

		if activeCL.PassingChecksCount == 0 && activeCL.CommitQueueScore == 0 {
			return "DRIVE_ACTIVE_CL",
				"HIGH",
				fmt.Sprintf("Active CL #%d (%s) is clean but has not run CQ tryjobs.", clNum, activeCL.Subject),
				[]string{
					fmt.Sprintf("./gh pr edit %d --cq && ./gh pr checks %d --watch --fail-fast", clNum, clNum),
				},
				nil
		}

		if activeCL.DraftsCount > 0 {
			s = append(s, fmt.Sprintf("./gh pr review %d --publish", clNum))
		}
		if activeCL.IsWIP {
			s = append(s, fmt.Sprintf("./gh pr ready %d --owner", clNum))
		}
		if activeCL.Submittable {
			s = append(s, fmt.Sprintf("./gh pr merge %d --auto", clNum))
		} else if len(activeCL.Blockers) > 0 && !activeCL.IsWIP {
			s = append(s, fmt.Sprintf("Awaiting review (%s); assign owners via `./gh pr ready %d --owner` if needed.", strings.Join(activeCL.Blockers, ", "), clNum))
		}

		return "DRIVE_ACTIVE_CL",
			"HIGH",
			fmt.Sprintf("Active CL #%d (%s) is green on CI (%s).", clNum, activeCL.Subject, activeCL.ChecksSummary),
			s,
			nil
	}

	// Tier 3: Unuploaded local work (dirty files or unpushed commits with no open Gerrit CL)
	if git.IsDirty || (git.CommitsAhead > 0 && activeCL == nil) {
		var s []string
		rebaseNote := ""
		if git.RebaseVerdict == "REBASE_CONFLICT" || git.RebaseVerdict == "REBASE_RECOMMENDED" {
			rebaseNote = fmt.Sprintf("; %s (%s)", git.RebaseVerdict, git.RebaseReason)
			s = append(s, fmt.Sprintf("Run `/freshen` to rebase local work onto `%s` before uploading.", baseRef))
		}
		s = append(s, "Run affected unit tests, format, run `/review` self-gate, commit, and upload (`./gh pr create --cq --draft`).")
		return "UPLOAD_LOCAL_WIP",
			"HIGH",
			fmt.Sprintf("Worktree has local unuploaded changes (%d dirty file(s), %d commit(s) ahead of `%s`%s).", git.DirtyFilesCount, git.CommitsAhead, baseRef, rebaseNote),
			s,
			nil
	}

	// Tier 4: Current branch's CL was MERGED (or ABANDONED) or clean on base branch
	isMergedCheckout := (activeCL != nil && (activeCL.Status == "MERGED" || activeCL.Status == "ABANDONED")) ||
		strings.Contains(wt.CurrentStatusBadge, "READY_FOR_NEXT")

	if isMergedCheckout {
		clStatusNote := fmt.Sprintf("Clean worktree at `%s`", baseRef)
		if activeCL != nil {
			clStatusNote = fmt.Sprintf("Previous CL #%d (%s) is %s", activeCL.Number, activeCL.Subject, activeCL.Status)
		}

		if oncall.IsUserOncall {
			oncallTitle := fmt.Sprintf("%s oncall", oncall.Role)
			if meta.projectLabel != "" {
				oncallTitle = fmt.Sprintf("%s %s oncall", meta.projectLabel, oncall.Role)
			}
			return "ONCALL_TRIAGE",
				"HIGH",
				fmt.Sprintf("%s, and `%s` is currently %s.", clStatusNote, oncall.UserLogin, oncallTitle),
				[]string{
					"Load `/oncall` skill or triage top open oncall issues (`./gh issue status`).",
				},
				nil
		}

		if len(wt.NeedsAttention) > 0 || len(wt.ReadyToLand) > 0 {
			var steps []string
			for _, p := range wt.ReadyToLand {
				steps = append(steps, fmt.Sprintf("Land `%s` (%s): `./gh pr merge --auto`.", p.Project, p.Details))
			}
			for _, p := range wt.NeedsAttention {
				steps = append(steps, fmt.Sprintf("Crank `%s` (%s): `./gh wt use %s`.", p.Project, p.Details, p.Project))
			}
			return "FLEET_TRIAGE_OR_PICK_BUG",
				"MEDIUM",
				fmt.Sprintf("%s; %d fleet workstream(s) need attention and %d are ready to land.", clStatusNote, len(wt.NeedsAttention), len(wt.ReadyToLand)),
				steps,
				nil
		}

		pickStep := fmt.Sprintf("Select top candidate issue below and run `/crank %s<id>`.", meta.issuePrefix)
		if candidateCount == 0 {
			pickStep = "No open issues assigned to user; run `./gh issue list` to browse open P0–P2 issues or ask the user for a target."
		}
		return "PICK_BUG_FROM_BUGANIZER",
			"MEDIUM",
			fmt.Sprintf("%s (%d commits behind `%s`). Ready to freshen and pick the next open issue.", clStatusNote, git.CommitsBehind, baseRef),
			[]string{
				pickStep,
			},
			nil
	}

	return "PICK_BUG_FROM_BUGANIZER",
		"LOW",
		"Worktree is idle; ready to freshen and discover next task.",
		[]string{"Run `/freshen` and query `./gh issue status`."},
		nil
}

func shortSHA(sha string) string {
	if len(sha) > 9 {
		return sha[:9]
	}
	return sha
}

func truncateLines(s string, maxLines int, maxChars int) string {
	s = strings.TrimSpace(s)
	if s == "" {
		return ""
	}
	if maxChars > 0 && len(s) > maxChars {
		s = s[:maxChars] + "..."
	}
	lines := strings.Split(s, "\n")
	if maxLines > 0 && len(lines) > maxLines {
		lines = append(lines[:maxLines], fmt.Sprintf("... (%d more lines)", len(lines)-maxLines))
	}
	return strings.Join(lines, "\n")
}

// FormatSenseStatusCard renders a compact, token-efficient directive briefing for both LLMs and humans.
func FormatSenseStatusCard(r *SenseReport) string {
	if r == nil {
		return ""
	}
	var sb strings.Builder
	fmt.Fprintf(&sb, "Modality: %s (Confidence: %s)\n", r.Modality, r.Confidence)
	fmt.Fprintf(&sb, "Summary:  %s\n", r.Summary)

	if r.Prepare != nil && r.Prepare.Requested {
		if r.Prepare.Status == "PREPARED" {
			fmt.Fprintf(&sb, "Prepare:  PREPARED (%s; prev=%s@%s -> now=%s@%s)\n",
				r.Prepare.ActionTaken, r.Prepare.PreviousBranch, shortSHA(r.Prepare.PreviousHEAD), r.Prepare.NewBranch, shortSHA(r.Prepare.NewHEAD))
		} else {
			fmt.Fprintf(&sb, "Prepare:  %s (%s)\n", r.Prepare.Status, r.Prepare.BlockedReason)
		}
	}

	badge := r.Worktree.CurrentStatusBadge
	if badge == "" {
		badge = "-"
	}
	if r.Worktree.isPoolManaged() {
		proj := r.Worktree.CurrentProject
		if proj == "" {
			proj = "(none)"
		}
		slot := r.Worktree.CurrentSlot
		if slot == "" {
			slot = "-"
		}
		fmt.Fprintf(&sb, "Worktree: project=%s  slot=%s  badge=%s\n", proj, slot, badge)
	} else if len(r.Worktree.GitWorktrees) > 1 {
		fmt.Fprintf(&sb, "Worktree: badge=%s  git_worktrees=%d\n", badge, len(r.Worktree.GitWorktrees))
	} else if badge != "-" {
		fmt.Fprintf(&sb, "Worktree: badge=%s\n", badge)
	}

	dirtyDetail := fmt.Sprintf("%d files", r.Git.DirtyFilesCount)
	if len(r.Git.ConflictedFiles) > 0 {
		dirtyDetail = fmt.Sprintf("%d files; conflicts: %s", r.Git.DirtyFilesCount, strings.Join(r.Git.ConflictedFiles, ", "))
	} else if len(r.Git.DirtyFiles) > 0 {
		shown := r.Git.DirtyFiles
		suffix := ""
		if len(shown) > 4 {
			suffix = fmt.Sprintf(", +%d more", len(shown)-4)
			shown = shown[:4]
		}
		dirtyDetail = fmt.Sprintf("%d files: %s%s", r.Git.DirtyFilesCount, strings.Join(shown, ", "), suffix)
	}
	fmt.Fprintf(&sb, "Git:      branch=%s  ahead=%d  behind=%d  dirty=%v (%s)  stale=%v\n",
		r.Git.Branch, r.Git.CommitsAhead, r.Git.CommitsBehind, r.Git.IsDirty, dirtyDetail, r.Git.IsStale)
	if r.Git.RebaseVerdict != "" {
		fmt.Fprintf(&sb, "Rebase:   %s — %s\n", r.Git.RebaseVerdict, r.Git.RebaseReason)
	}

	if len(r.Worktree.AllProjects) > 0 {
		fmt.Fprintf(&sb, "Fleet Worktrees (%d):\n", len(r.Worktree.AllProjects))
		for _, p := range r.Worktree.AllProjects {
			slotLabel := p.Slot
			if slotLabel == "" {
				slotLabel = "-"
			}
			det := ""
			if p.Details != "" {
				det = " — " + p.Details
			}
			fmt.Fprintf(&sb, "  - %-22s [%-7s slot=%-5s] %s%s\n", p.Project, p.Residency, slotLabel, p.StatusBadge, det)
		}
	}

	// Render Target metadata and choose which CL card to render in full detail
	clToRender := r.ActiveCL
	clLabel := "Active CL"
	if r.Target != nil && r.Target.CL != nil {
		if r.ActiveCL != nil && r.ActiveCL.Number != r.Target.CL.Number {
			fmt.Fprintf(&sb, "Active CL: #%d (%s) — %s\n", r.ActiveCL.Number, r.ActiveCL.Status, r.ActiveCL.Subject)
		}
		if r.ActiveCL == nil || r.ActiveCL.Number != r.Target.CL.Number {
			clToRender = r.Target.CL
			clLabel = "Target CL"
		}
	}

	if r.Target != nil {
		extraTargetNotes := ""
		if r.Target.NotFound {
			extraTargetNotes += "  not_found=true"
		}
		if r.Target.FetchError != "" {
			extraTargetNotes += fmt.Sprintf("  error=%q", r.Target.FetchError)
		}
		fmt.Fprintf(&sb, "Target:   type=%s  id=%s  existing_wt=%v%s\n",
			r.Target.TargetType, r.Target.NormalizedID, r.Target.ExistingWorktree != nil, extraTargetNotes)
		if r.Target.ExistingWorktree != nil {
			ex := r.Target.ExistingWorktree
			fmt.Fprintf(&sb, "          existing_project=%s (%s)  slot=%s  path=%s\n",
				ex.Project, ex.Residency, ex.Slot, ex.SymlinkPath)
		}
	}

	if clToRender != nil {
		c := clToRender
		wipTag := ""
		if c.IsWIP {
			wipTag = " [WIP]"
		}
		ownerTag := ""
		if c.Owner != "" {
			ownerTag = fmt.Sprintf(" (owner=%s)", c.Owner)
		}
		fmt.Fprintf(&sb, "%s: #%d (%s%s)%s — %s\n", clLabel, c.Number, c.Status, wipTag, ownerTag, c.Subject)
		if c.Status != "MERGED" && c.Status != "ABANDONED" {
			fmt.Fprintf(&sb, "          CR=%+d  V=%+d  CQ=%+d  submittable=%v  mergeable=%v  checks=%d pass / %d pending / %d fail\n",
				c.CodeReviewScore, c.VerifiedScore, c.CommitQueueScore, c.Submittable, c.Mergeable,
				c.PassingChecksCount, c.PendingChecksCount, len(c.FailingChecks))
		}

		if len(c.FailingChecks) > 0 {
			sb.WriteString("Failing Checks:\n")
			detailByBuilder := make(map[string]SenseFailingCheckDetail)
			for _, d := range c.FailingCheckDetails {
				detailByBuilder[d.Builder] = d
			}
			for i, fc := range c.FailingChecks {
				if i >= 5 {
					fmt.Fprintf(&sb, "  ... and %d more failing check(s)\n", len(c.FailingChecks)-5)
					break
				}
				fmt.Fprintf(&sb, "  - %s (%s)\n", fc.Name, fc.URL)
				if det, ok := detailByBuilder[fc.Name]; ok {
					if det.FailedStep != "" {
						fmt.Fprintf(&sb, "    Failed step: %s\n", det.FailedStep)
					}
					if det.LogExcerpt != "" {
						for _, line := range strings.Split(det.LogExcerpt, "\n") {
							fmt.Fprintf(&sb, "      %s\n", line)
						}
					}
				}
			}
		}

		if len(c.AuthorSelfDrafts) > 0 {
			fmt.Fprintf(&sb, "Author Self-Drafts (%d):\n", len(c.AuthorSelfDrafts))
			for _, d := range c.AuthorSelfDrafts {
				idNote := ""
				if d.CommentID != "" {
					idNote = fmt.Sprintf(" [draft_id=%s]", d.CommentID)
				}
				fmt.Fprintf(&sb, "  - %s:%d%s: %q\n", d.File, d.Line, idNote, d.Message)
				if d.CodeSnippet != "" {
					for _, line := range strings.Split(d.CodeSnippet, "\n") {
						fmt.Fprintf(&sb, "    %s\n", line)
					}
				}
			}
		}

		if len(c.ExternalUnresolvedThreads) > 0 {
			fmt.Fprintf(&sb, "Unresolved Threads (%d):\n", len(c.ExternalUnresolvedThreads))
			for _, th := range c.ExternalUnresolvedThreads {
				idNote := ""
				if th.CommentID != "" {
					idNote = fmt.Sprintf(" [comment_id=%s]", th.CommentID)
				}
				draftNote := ""
				if th.HasDraftReply {
					draftNote = " (has staged draft reply)"
				}
				fmt.Fprintf(&sb, "  - %s:%d [PS%d]%s by %s%s: %q\n",
					th.File, th.Line, th.PatchSet, idNote, th.Author, draftNote, th.Message)
				if len(th.ThreadHistory) > 1 {
					for _, h := range th.ThreadHistory {
						fmt.Fprintf(&sb, "      thread: %s\n", h)
					}
				}
				if th.CodeSnippet != "" {
					for _, line := range strings.Split(th.CodeSnippet, "\n") {
						fmt.Fprintf(&sb, "    %s\n", line)
					}
				}
			}
		}
	}

	if r.Target != nil && r.Target.Issue != nil {
		iss := r.Target.Issue
		fmt.Fprintf(&sb, "Issue:    b/%d [%s %s] — %s\n", iss.Number, iss.Priority, iss.State, iss.Title)
		if iss.Description != "" {
			sb.WriteString("  Description:\n")
			for _, line := range strings.Split(iss.Description, "\n") {
				fmt.Fprintf(&sb, "    %s\n", line)
			}
		}
		if len(iss.Comments) > 0 {
			fmt.Fprintf(&sb, "  Comments (%d):\n", len(iss.Comments))
			for _, c := range iss.Comments {
				fmt.Fprintf(&sb, "    #%d (%s %s): %s\n", c.Number, c.Author, c.CreatedAt, strings.ReplaceAll(c.Message, "\n", " "))
			}
		}
		if len(iss.RelatedCLs) > 0 {
			sb.WriteString("  Related Gerrit CLs:\n")
			for _, rcl := range iss.RelatedCLs {
				ref := rcl.ShortRef
				if ref == "" {
					ref = fmt.Sprintf("#%d", rcl.Number)
				}
				fmt.Fprintf(&sb, "    - %s [%s] %s\n", ref, rcl.Status, rcl.Subject)
			}
		}
	}

	if r.Oncall.PrimaryOncall != "" || r.Oncall.SecondaryOncall != "" {
		fmt.Fprintf(&sb, "Oncall:   user=%s  primary=%s  secondary=%s  is_oncall=%v (%s)\n",
			r.Oncall.UserLogin, r.Oncall.PrimaryOncall, r.Oncall.SecondaryOncall, r.Oncall.IsUserOncall, r.Oncall.Role)
	}

	if len(r.CandidateIssues) > 0 {
		sb.WriteString("Candidate Open Issues:\n")
		for _, ci := range r.CandidateIssues {
			fmt.Fprintf(&sb, "  - b/%d [%s] %s\n", ci.Number, ci.Priority, ci.Title)
		}
	}

	if len(r.Warnings) > 0 {
		sb.WriteString("Warnings:\n")
		for _, w := range r.Warnings {
			fmt.Fprintf(&sb, "  - %s\n", w)
		}
	}

	if len(r.RecommendedSteps) > 0 {
		sb.WriteString("Next:\n")
		for i, step := range r.RecommendedSteps {
			fmt.Fprintf(&sb, "  %d. %s\n", i+1, step)
		}
	}
	return sb.String()
}
