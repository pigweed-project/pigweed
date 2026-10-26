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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/spf13/cobra"
)

const maxDefaultIssueStatusItems = 10

func shortIssueUser(email string) string {
	return strings.TrimSuffix(strings.TrimSpace(email), "@google.com")
}

func resolveTargetIssueID(ctx context.Context, cmd *cobra.Command, args []string) (int64, error) {
	if len(args) > 0 && strings.TrimSpace(args[0]) != "" {
		return ParseIssueID(args[0])
	}

	cfg := GetConfig(cmd)
	if cfg == nil {
		cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
	}
	msg, err := cfg.GitClient().CommitMessage(ctx, "HEAD")
	if err != nil {
		return 0, fmt.Errorf("no issue <number> provided and failed to inspect HEAD commit: %w\n\n"+
			"Remediation:\n"+
			"  Provide an explicit issue number or URL:\n"+
			"    gh issue %s <issue_id>", err, cmd.Name())
	}

	links := ExtractBugLinks(msg)
	var ids []int64
	for _, link := range links {
		if id, err := ParseIssueID(link.ID); err == nil && id > 0 {
			ids = append(ids, id)
		}
	}
	ids = dedup(ids)

	if len(ids) == 0 {
		// Tier 2: Branch name inference (e.g. b-315378787-fix-rpc or 315378787-fix-rpc)
		if branch, brErr := cfg.GitClient().CurrentBranch(ctx); brErr == nil && branch != "" {
			if id, ok := ExtractIssueIDFromBranchName(branch); ok && id > 0 {
				fmt.Fprintf(cmd.ErrOrStderr(), "Using issue b/%d from active branch name %q.\n", id, branch)
				return id, nil
			}
		}

		// Tier 3: Optional Worktree state hook inference
		if RegisteredWorkspaceIntegration != nil && RegisteredWorkspaceIntegration.IsEnabled() && cfg.CWD != "" {
			if id, ok := RegisteredWorkspaceIntegration.ResolveIssueIDForPath(cfg.CWD); ok && id > 0 {
				fmt.Fprintf(cmd.ErrOrStderr(), "Using issue b/%d from active worktree project.\n", id)
				return id, nil
			}
		}

		return 0, fmt.Errorf("no issue <number> provided and HEAD commit contains no Bug: or Fixed: trailer.\n\n"+
			"Remediation:\n"+
			"  1. Specify the Buganizer issue ID explicitly:\n"+
			"     gh issue %s <issue_id>\n"+
			"  2. Or link an issue to your current commit:\n"+
			"     git commit --amend -m \"...\\n\\nBug: b/<issue_id>\"", cmd.Name())
	}

	if len(ids) > 1 {
		var formatted []string
		for _, id := range ids {
			formatted = append(formatted, fmt.Sprintf("b/%d", id))
		}
		return 0, fmt.Errorf("HEAD commit references multiple bug IDs (%s); cannot automatically choose one.\n\n"+
			"Remediation:\n"+
			"  Specify which issue you want to target explicitly:\n"+
			"    gh issue %s %d", strings.Join(formatted, ", "), cmd.Name(), ids[0])
	}

	fmt.Fprintf(cmd.ErrOrStderr(), "Using issue b/%d from HEAD commit trailer.\n", ids[0])
	return ids[0], nil
}

var ownersComponentRegex = regexp.MustCompile(`(?im)^\s*#\s*(?:buganizer\s+component|component)\s*:\s*(?:b/)?(\d+)\s*$`)

// ParseOwnersComponent extracts a numeric Buganizer component ID from an OWNERS
// file comment such as `# COMPONENT: 1194524` or `# Buganizer component: 1194524`.
func ParseOwnersComponent(content string) (int64, bool) {
	m := ownersComponentRegex.FindStringSubmatch(content)
	if len(m) < 2 {
		return 0, false
	}
	id, err := strconv.ParseInt(m[1], 10, 64)
	if err != nil || id <= 0 {
		return 0, false
	}
	return id, true
}

func matchPathComponentPattern(pattern, relPath string) bool {
	pattern = strings.TrimSpace(strings.ReplaceAll(filepath.ToSlash(pattern), `\`, `/`))
	relPath = strings.TrimPrefix(strings.TrimSpace(strings.ReplaceAll(filepath.ToSlash(relPath), `\`, `/`)), "./")
	relPath = strings.TrimSuffix(relPath, "/")
	if pattern == "" || relPath == "" {
		return false
	}
	if pattern == "**" || pattern == "/**" {
		return true
	}
	if strings.HasSuffix(pattern, "/**") {
		prefix := strings.TrimPrefix(strings.TrimSuffix(pattern, "/**"), "/")
		if prefix == "" {
			return true
		}
		return relPath == prefix || strings.HasPrefix(relPath, prefix+"/")
	}
	if strings.HasSuffix(pattern, "/*") {
		prefix := strings.TrimPrefix(strings.TrimSuffix(pattern, "/*"), "/")
		if prefix == "" {
			return !strings.Contains(relPath, "/")
		}
		return relPath == prefix || strings.HasPrefix(relPath, prefix+"/")
	}
	if strings.HasSuffix(pattern, "/") {
		prefix := strings.TrimPrefix(strings.TrimSuffix(pattern, "/"), "/")
		if prefix == "" {
			return true
		}
		return relPath == prefix || strings.HasPrefix(relPath, prefix+"/")
	}
	if matched, err := path.Match(pattern, relPath); err == nil && matched {
		return true
	}
	return relPath == pattern || strings.HasPrefix(relPath, pattern+"/")
}

// ResolveIssueComponent resolves the Buganizer component ID using the multi-tier cascade:
//  1. Explicit --component / -C flag (explicitFlag > 0)
//  2. [issue.path_components] in .ghish.toml / git config matched against modifiedPaths (or CWD)
//  3. Nearest OWNERS file (# COMPONENT: <id> or # Buganizer component: <id>) when UseOwnersComponents is true
//  4. [issue] default_component in .ghish.toml / git config
//  5. git config ghish.componentid
//  6. ProjectProfile.DefaultComponentID()
func ResolveIssueComponent(ctx context.Context, cfg *Config, explicitFlag int64, modifiedPaths []string) (int64, error) {
	if explicitFlag > 0 {
		return explicitFlag, nil
	}

	var pcfg *ProjectConfig
	if cfg != nil {
		if cfg.ProjectConfig != nil {
			pcfg = cfg.ProjectConfig
		} else if hasProjectConfigFileOnDisk(cfg.CWD) {
			var err error
			pcfg, err = cfg.LoadProjectConfig(ctx)
			if err != nil {
				return 0, err
			}
		} else if dg, ok := cfg.GitClient().(*defaultGitClient); ok {
			cand := DefaultProjectConfig()
			applied, err := dg.applyGitConfigToProjectConfig(ctx, cand)
			if err != nil {
				return 0, err
			}
			if applied {
				pcfg = cand
			}
		}
	}

	candidatePaths := append([]string(nil), modifiedPaths...)
	var repoRoot string
	if cfg != nil && cfg.CWD != "" {
		repoRoot = findGitTopLevelOnDisk(cfg.CWD)
	}
	if len(candidatePaths) == 0 && cfg != nil && cfg.Git != nil {
		if dg, ok := cfg.GitClient().(*defaultGitClient); ok {
			if out, err := dg.runOutput(ctx, "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"); err == nil {
				for _, line := range strings.Split(out, "\n") {
					if p := strings.TrimSpace(line); p != "" {
						candidatePaths = append(candidatePaths, p)
					}
				}
			}
		}
	}
	if len(candidatePaths) == 0 && repoRoot != "" && cfg != nil && cfg.CWD != "" {
		if absCWD, err := filepath.Abs(cfg.CWD); err == nil {
			if rel, err := filepath.Rel(repoRoot, absCWD); err == nil && rel != "." && !strings.HasPrefix(rel, "..") {
				candidatePaths = append(candidatePaths, filepath.ToSlash(rel))
			}
		}
	}

	// Tier 2: Path-based component mapping ([issue.path_components])
	if pcfg != nil && len(pcfg.Issue.PathComponents) > 0 && len(candidatePaths) > 0 {
		for _, relPath := range candidatePaths {
			bestLen := -1
			var bestComp int64
			for pattern, compID := range pcfg.Issue.PathComponents {
				if compID > 0 && matchPathComponentPattern(pattern, relPath) {
					if len(pattern) > bestLen {
						bestLen = len(pattern)
						bestComp = compID
					}
				}
			}
			if bestComp > 0 {
				return bestComp, nil
			}
		}
	}

	// Tier 3: Nearest OWNERS file (# COMPONENT: <id> or # Buganizer component: <id>)
	if (pcfg == nil || pcfg.UseOwnersComponents()) && repoRoot != "" {
		var searchDirs []string
		if len(candidatePaths) > 0 {
			for _, relPath := range candidatePaths {
				fullPath := filepath.Join(repoRoot, filepath.FromSlash(relPath))
				if info, err := os.Stat(fullPath); err == nil && info.IsDir() {
					searchDirs = append(searchDirs, fullPath)
				} else {
					searchDirs = append(searchDirs, filepath.Dir(fullPath))
				}
			}
		} else if cfg != nil && cfg.CWD != "" {
			if absCWD, err := filepath.Abs(cfg.CWD); err == nil {
				searchDirs = append(searchDirs, absCWD)
			}
		}
		for _, startDir := range searchDirs {
			curr := startDir
			for {
				ownersPath := filepath.Join(curr, "OWNERS")
				data, err := os.ReadFile(ownersPath)
				if err != nil {
					if !errors.Is(err, os.ErrNotExist) {
						return 0, fmt.Errorf("failed to read %s: %w", ownersPath, err)
					}
				} else if compID, ok := ParseOwnersComponent(string(data)); ok {
					return compID, nil
				}
				if filepath.Clean(curr) == filepath.Clean(repoRoot) {
					break
				}
				parent := filepath.Dir(curr)
				if parent == curr {
					break
				}
				curr = parent
			}
		}
	}

	return projectComponentID(ctx, cfg, pcfg)
}

// projectComponentID resolves the project-wide component from project config,
// git config, or profile defaults, ignoring path- and OWNERS-based mappings.
func projectComponentID(ctx context.Context, cfg *Config, pcfg *ProjectConfig) (int64, error) {
	// Tier 4: ProjectConfig default_component
	if pcfg != nil && pcfg.Issue.DefaultComponent > 0 {
		return pcfg.Issue.DefaultComponent, nil
	}

	// Tier 5: Legacy git config ghish.componentid
	if cfg != nil {
		if val, err := cfg.GitClient().ConfigGet(ctx, "ghish.componentid"); err == nil && strings.TrimSpace(val) != "" {
			trimmed := strings.TrimSpace(val)
			parsed, parseErr := strconv.ParseInt(trimmed, 10, 64)
			if parseErr != nil || parsed <= 0 {
				return 0, fmt.Errorf("invalid git config 'ghish.componentid' value %q: must be a positive integer component ID.\n\n"+
					"Remediation:\n"+
					"  Set a valid numeric Buganizer component ID:\n"+
					"    git config ghish.componentid 1194524", trimmed)
			}
			return parsed, nil
		}
		// Tier 6: Profile default component ID
		if def := cfg.GetProfile(ctx).DefaultComponentID(); def > 0 {
			return def, nil
		}
	}
	return 0, nil
}

func resolveComponentID(ctx context.Context, cfg *Config, explicitFlag int64) (int64, error) {
	return ResolveIssueComponent(ctx, cfg, explicitFlag, nil)
}

func resolveUserEmailArg(ctx context.Context, cfg *Config, arg string) (string, error) {
	trimmed := strings.TrimSpace(arg)
	if trimmed == "me" || trimmed == "@me" {
		email, err := cfg.GitClient().UserEmail(ctx)
		if err != nil || strings.TrimSpace(email) == "" {
			return "", fmt.Errorf("cannot resolve %q: git config 'user.email' is not configured.\n\n"+
				"Remediation:\n"+
				"  Configure your git user email:\n"+
				"    git config user.email \"you@example.com\"", trimmed)
		}
		return strings.TrimSpace(email), nil
	}
	return trimmed, nil
}

func readBodyInput(bodyFlag, bodyFileFlag string, stdin io.Reader) (string, error) {
	if bodyFlag != "" && bodyFileFlag != "" {
		return "", fmt.Errorf("cannot specify both --body (-b) and --body-file (-F)")
	}
	if bodyFlag == "-" && bodyFileFlag == "" {
		bodyFileFlag = "-"
		bodyFlag = ""
	}
	if bodyFileFlag != "" {
		if bodyFileFlag == "-" {
			in := stdin
			if in == nil {
				in = os.Stdin
			}
			data, err := io.ReadAll(in)
			if err != nil {
				return "", fmt.Errorf("failed to read body from stdin: %w", err)
			}
			if strings.TrimSpace(string(data)) == "" {
				return "", fmt.Errorf("body from stdin is empty; comment body cannot be empty")
			}
			return string(data), nil
		}
		data, err := os.ReadFile(bodyFileFlag)
		if err != nil {
			return "", fmt.Errorf("failed to read body file %q: %w", bodyFileFlag, err)
		}
		return string(data), nil
	}
	return bodyFlag, nil
}

// --- Subcommands ---

// IssueComment aliases BuganizerComment for issue comment summarization helpers.
type IssueComment = BuganizerComment

const maxDefaultIssueComments = 15

var (
	issueCommentURLRegex     = regexp.MustCompile(`https?://[^\s)>\]"]+`)
	issueDetailsBlockRegex   = regexp.MustCompile(`(?is)<details\b[^>]*>.*?</details>`)
	issueSummaryTagRegex     = regexp.MustCompile(`(?is)<summary\b[^>]*>.*?</summary>`)
	issueDetailsTagOpenClose = regexp.MustCompile(`(?i)</?details\b[^>]*>`)
	issueExpandDetailsPhrase = regexp.MustCompile(`(?i)expand for full commit details:?`)
)

func isCommitMetadataLine(line string) bool {
	trimmed := strings.TrimSpace(line)
	if trimmed == "" {
		return true
	}
	lower := strings.ToLower(trimmed)
	if strings.HasPrefix(lower, "http://") || strings.HasPrefix(lower, "https://") {
		return true
	}
	if strings.HasPrefix(lower, "the following revision refers to this bug") ||
		strings.HasPrefix(lower, "expand for full commit details") {
		return true
	}
	if strings.HasPrefix(lower, "commit ") && len(trimmed) > 7 {
		return true
	}
	for _, prefix := range []string{
		"project:",
		"branch:",
		"author:",
		"date:",
		"link:",
		"commit:",
		"bug:",
		"fixed:",
		"fixes:",
		"change-id:",
		"reviewed-on:",
		"reviewed-by:",
		"commit-queue:",
		"auto-submit:",
		"presubmit-verified:",
		"signed-off-by:",
		"cq-include-trybots:",
	} {
		if strings.HasPrefix(lower, prefix) {
			return true
		}
	}
	return false
}

// summarizeBotIssueComment detects automated GitWatcher / commit-mirror / bot
// comments on a Buganizer issue and returns a compact 1-line summary:
//
//	"  • [Commit #N] <timestamp>: <subject> (<link>)"
func summarizeBotIssueComment(c IssueComment) (summaryLine string, isBot bool) {
	author := strings.ToLower(c.EffectiveAuthorEmail())
	body := strings.TrimSpace(SanitizeUntrustedText(c.Comment))
	lowerBody := strings.ToLower(body)

	authorIsBot := strings.Contains(author, "gitwatcher") ||
		strings.Contains(author, "robot") ||
		strings.Contains(author, "buganizer-system") ||
		strings.Contains(author, "blunderbuss")

	hasCommitHeaders := false
	hasProjectLine := false
	hasBranchLine := false
	for _, line := range strings.Split(body, "\n") {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "Project:") {
			hasProjectLine = true
		} else if strings.HasPrefix(trimmed, "Branch:") {
			hasBranchLine = true
		}
		if hasProjectLine && hasBranchLine {
			hasCommitHeaders = true
			break
		}
	}
	bodyIsCommitDump := (strings.Contains(lowerBody, "<details") && strings.Contains(lowerBody, "expand for full commit details")) ||
		hasCommitHeaders ||
		(authorIsBot && strings.Contains(body, "Project:") && (strings.Contains(body, "Reviewed-on:") || strings.Contains(body, "Link:")))

	if !authorIsBot && !bodyIsCommitDump {
		return "", false
	}

	var link string
	for _, line := range strings.Split(body, "\n") {
		trimmed := strings.TrimSpace(line)
		lower := strings.ToLower(trimmed)
		if strings.HasPrefix(lower, "link:") || strings.HasPrefix(lower, "reviewed-on:") {
			if m := issueCommentURLRegex.FindString(trimmed); m != "" {
				link = m
				break
			}
		}
	}
	if link == "" {
		link = issueCommentURLRegex.FindString(body)
	}

	outsideDetails := issueDetailsBlockRegex.ReplaceAllString(body, "")
	var subject string
	for _, line := range strings.Split(outsideDetails, "\n") {
		trimmed := strings.TrimSpace(line)
		if !isCommitMetadataLine(trimmed) {
			subject = trimmed
			break
		}
	}
	if subject == "" {
		stripped := issueSummaryTagRegex.ReplaceAllString(body, "")
		stripped = issueDetailsTagOpenClose.ReplaceAllString(stripped, "")
		stripped = issueExpandDetailsPhrase.ReplaceAllString(stripped, "")
		for _, line := range strings.Split(stripped, "\n") {
			trimmed := strings.TrimSpace(line)
			if !isCommitMetadataLine(trimmed) {
				subject = trimmed
				break
			}
		}
	}
	if link != "" && strings.Contains(subject, link) {
		subject = strings.TrimSpace(strings.ReplaceAll(subject, "("+link+")", ""))
		subject = strings.TrimSpace(strings.ReplaceAll(subject, link, ""))
	}
	if subject == "" {
		subject = "automated commit update"
	}

	ts := c.CreatedTime.Format(time.RFC3339)
	if link != "" {
		return fmt.Sprintf("  • [Commit #%d] %s: %s (%s)", c.CommentNumber, ts, subject, link), true
	}
	return fmt.Sprintf("  • [Commit #%d] %s: %s", c.CommentNumber, ts, subject), true
}

var (
	issueViewComments bool
	issueViewAll      bool
	issueViewWeb      bool
	issueViewJSON     string
)

var issueViewCmd = &cobra.Command{
	Use:         "view [<number> | <url>]",
	Annotations: map[string]string{KindAnnotation: KindView},
	Short:       "View a Buganizer issue",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		jsonFields, err := ValidateIssueJSONFields(issueViewJSON)
		if err != nil {
			return err
		}

		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		profile := cfg.GetProfile(ctx)

		if issueViewWeb {
			urlStr := profile.IssueWebURL(issueID)
			fmt.Fprintf(cmd.OutOrStdout(), "Opening %s in your browser.\n", urlStr)
			return OpenBrowserFn(urlStr)
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		var (
			issue       *BuganizerIssue
			issueErr    error
			comments    []BuganizerComment
			commentsErr error
		)
		needComments := issueViewComments || issueViewAll || slices.Contains(jsonFields, "comments") || slices.Contains(jsonFields, "body")
		if needComments {
			cCtx, cancel := context.WithCancel(ctx)
			defer cancel()
			var wg sync.WaitGroup
			wg.Add(2)
			go func() {
				defer wg.Done()
				issue, issueErr = client.GetIssue(ctx, issueID)
				if issueErr != nil {
					cancel()
				}
			}()
			go func() {
				defer wg.Done()
				comments, commentsErr = client.ListAllComments(cCtx, issueID)
			}()
			wg.Wait()
		} else {
			issue, issueErr = client.GetIssue(ctx, issueID)
		}
		if issueErr != nil {
			return issueErr
		}
		if commentsErr != nil {
			return commentsErr
		}

		if len(jsonFields) > 0 {
			m := IssueToJSONMap(issue, comments, profile, jsonFields)
			out, err := json.MarshalIndent(m, "", "  ")
			if err != nil {
				return fmt.Errorf("failed to serialize issue JSON: %w", err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(out))
			return nil
		}

		out := cmd.OutOrStdout()
		fmt.Fprintf(out, "%s #%d\n", SanitizeUntrustedText(issue.State.Title), issue.IssueID)
		verdictState := "CLOSED"
		if IsIssueOpen(issue.State.Status) {
			verdictState = "OPEN"
		}
		priSev := issue.State.Priority
		if issue.State.Severity != "" {
			if priSev != "" {
				priSev = fmt.Sprintf("%s/%s", priSev, issue.State.Severity)
			} else {
				priSev = issue.State.Severity
			}
		}
		verdictDetail := strings.TrimSpace(fmt.Sprintf("%s %s", issue.State.Status, priSev))
		if verdictDetail != "" {
			fmt.Fprintf(out, "Verdict:   %s (%s)\n", verdictState, verdictDetail)
		} else {
			fmt.Fprintf(out, "Verdict:   %s\n", verdictState)
		}
		fmt.Fprintf(out, "Status:    %s\n", issue.State.Status)
		if issue.State.Priority != "" {
			fmt.Fprintf(out, "Priority:  %s\n", issue.State.Priority)
		}
		if issue.State.Severity != "" {
			fmt.Fprintf(out, "Severity:  %s\n", issue.State.Severity)
		}
		if issue.State.Type != "" {
			fmt.Fprintf(out, "Type:      %s\n", issue.State.Type)
		}
		if issue.State.ComponentID > 0 {
			fmt.Fprintf(out, "Component: %d\n", issue.State.ComponentID)
		}
		if len(issue.State.HotlistIDs) > 0 {
			hids := make([]string, 0, len(issue.State.HotlistIDs))
			for _, hid := range issue.State.HotlistIDs {
				hids = append(hids, strconv.FormatInt(int64(hid), 10))
			}
			fmt.Fprintf(out, "Hotlists:  %s\n", strings.Join(hids, ", "))
		}
		if issue.State.Assignee != nil && issue.State.Assignee.EmailAddress != "" {
			fmt.Fprintf(out, "Assignee:  %s\n", shortIssueUser(issue.State.Assignee.EmailAddress))
		} else {
			fmt.Fprintf(out, "Assignee:  (unassigned)\n")
		}
		if issue.State.Reporter != nil && issue.State.Reporter.EmailAddress != "" {
			fmt.Fprintf(out, "Reporter:  %s\n", shortIssueUser(issue.State.Reporter.EmailAddress))
		}
		fmt.Fprintf(out, "URL:       %s\n\n", profile.IssueWebURL(int64(issue.IssueID)))

		desc := ""
		if effDesc := issue.EffectiveDescription(); effDesc != nil {
			desc = effDesc.Comment
		} else if len(comments) > 0 {
			desc = comments[0].Comment
		}
		header := fmt.Sprintf("--- BEGIN UNTRUSTED ISSUE DESCRIPTION (b/%d) ---", issue.IssueID)
		footer := "--- END UNTRUSTED ISSUE DESCRIPTION ---"
		fmt.Fprintln(out, FormatUntrustedBlock(header, footer, desc))

		if (issueViewComments || issueViewAll) && len(comments) > 1 {
			fmt.Fprintln(out, "\nComments:")
			var thread []BuganizerComment
			for i, c := range comments {
				if c.CommentNumber == 1 || (c.CommentNumber == 0 && i == 0) {
					continue
				}
				thread = append(thread, c)
			}

			if !issueViewAll {
				humanCount := 0
				for _, c := range thread {
					if _, isBot := summarizeBotIssueComment(c); !isBot {
						humanCount++
					}
				}
				if humanCount > maxDefaultIssueComments {
					toSkipHumans := humanCount - maxDefaultIssueComments
					skipped := 0
					cutoff := 0
					for idx, c := range thread {
						if _, isBot := summarizeBotIssueComment(c); !isBot {
							skipped++
							if skipped == toSkipHumans {
								cutoff = idx + 1
								break
							}
						}
					}
					fmt.Fprintf(out, "  (… %d earlier comments omitted; use --all to view all)\n", cutoff)
					thread = thread[cutoff:]
				}
			}

			for _, c := range thread {
				if !issueViewAll {
					if summaryLine, isBot := summarizeBotIssueComment(c); isBot {
						fmt.Fprintln(out, summaryLine)
						continue
					}
				}
				author := c.EffectiveAuthorEmail()
				cHeader := fmt.Sprintf("--- BEGIN UNTRUSTED COMMENT #%d by %s (%s) ---",
					c.CommentNumber, author, c.CreatedTime.Format(time.RFC3339))
				cFooter := "--- END UNTRUSTED COMMENT ---"
				fmt.Fprintln(out, "")
				fmt.Fprintln(out, FormatUntrustedBlock(cHeader, cFooter, c.Comment))
			}
		}

		if IsIssueOpen(issue.State.Status) {
			fmt.Fprintf(out, "\nNext: gh issue develop %d  (or gh issue comment %d -b \"...\")\n", issue.IssueID, issue.IssueID)
		} else {
			fmt.Fprintf(out, "\nNext: gh issue reopen %d\n", issue.IssueID)
		}

		return nil
	},
}

var (
	issueListAssignee      string
	issueListAuthor        string
	issueListLabels        []string
	issueListState         string
	issueListSearch        string
	issueListLimit         int
	issueListComponent     int64
	issueListAllComponents bool
	issueListJSON          string
)

var issueListCmd = &cobra.Command{
	Use:         "list",
	Annotations: map[string]string{KindAnnotation: KindList},
	Short:       "List Buganizer issues",
	Args:        cobra.NoArgs,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		jsonFields, err := ValidateIssueJSONFields(issueListJSON)
		if err != nil {
			return err
		}

		stateFilter := strings.ToLower(strings.TrimSpace(issueListState))
		if stateFilter == "" {
			stateFilter = "open"
		}
		if stateFilter != "open" && stateFilter != "closed" && stateFilter != "all" {
			return fmt.Errorf("invalid --state value %q; valid choices are 'open', 'closed', or 'all'", issueListState)
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		profile := cfg.GetProfile(ctx)
		pcfg, err := cfg.LoadProjectConfig(ctx)
		if err != nil {
			return err
		}

		var labelTokens []string
		hasPositiveComponentLabel := false
		for _, l := range issueListLabels {
			token, err := LabelToQueryToken(pcfg.ExpandIssueLabel(l))
			if err != nil {
				return err
			}
			if strings.HasPrefix(token, "componentid:") {
				hasPositiveComponentLabel = true
			}
			labelTokens = append(labelTokens, token)
		}

		var queryParts []string
		if !issueListAllComponents && (issueListComponent > 0 || !hasPositiveComponentLabel) {
			compID, err := resolveComponentID(ctx, cfg, issueListComponent)
			if err != nil {
				return err
			}
			if compID > 0 {
				queryParts = append(queryParts, fmt.Sprintf("componentid:%d+", compID))
			}
		}
		if stateFilter == "open" {
			queryParts = append(queryParts, "status:open")
		} else if stateFilter == "closed" {
			queryParts = append(queryParts, "status:closed")
		}

		if issueListAssignee != "" {
			assignee, err := resolveUserEmailArg(ctx, cfg, issueListAssignee)
			if err != nil {
				return err
			}
			queryParts = append(queryParts, fmt.Sprintf("assignee:%s", assignee))
		}
		if issueListAuthor != "" {
			author, err := resolveUserEmailArg(ctx, cfg, issueListAuthor)
			if err != nil {
				return err
			}
			queryParts = append(queryParts, fmt.Sprintf("reporter:%s", author))
		}

		queryParts = append(queryParts, labelTokens...)

		if issueListSearch != "" {
			queryParts = append(queryParts, issueListSearch)
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		queryStr := strings.Join(queryParts, " ")
		resp, err := client.ListIssues(ctx, queryStr, issueListLimit, "")
		if err != nil {
			return err
		}

		if len(jsonFields) > 0 {
			outList := make([]map[string]any, 0, len(resp.Issues))
			for _, iss := range resp.Issues {
				outList = append(outList, IssueToJSONMap(iss, nil, profile, jsonFields))
			}
			data, err := json.MarshalIndent(outList, "", "  ")
			if err != nil {
				return fmt.Errorf("failed to serialize issue list JSON: %w", err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(data))
			return nil
		}

		out := cmd.OutOrStdout()
		if len(resp.Issues) == 0 {
			fmt.Fprintln(out, "No issues found matching query.")
			return nil
		}

		fmt.Fprintln(out, "ID           STATUS      PRI     ASSIGNEE                  TITLE")
		for _, iss := range resp.Issues {
			assignee := "unassigned"
			if iss.State.Assignee != nil && iss.State.Assignee.EmailAddress != "" {
				assignee = shortIssueUser(iss.State.Assignee.EmailAddress)
			}
			pri := iss.State.Priority
			if pri == "" {
				pri = "--"
			}
			if iss.State.Severity != "" {
				pri = fmt.Sprintf("%s/%s", pri, iss.State.Severity)
			}
			fmt.Fprintf(out, "b/%-9d  %-10s  %-6s  %-24s  %s\n",
				iss.IssueID, iss.State.Status, pri, assignee, SanitizeUntrustedText(iss.State.Title))
		}
		fmt.Fprintf(out, "\nNext: gh issue view %d  (or gh issue develop %d)\n", resp.Issues[0].IssueID, resp.Issues[0].IssueID)
		return nil
	},
}

var issueStatusJSON string

var issueStatusCmd = &cobra.Command{
	Use:         "status",
	Annotations: map[string]string{KindAnnotation: KindList},
	Short:       "Show status of relevant Buganizer issues",
	Args:        cobra.NoArgs,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		jsonFields, err := ValidateIssueJSONFields(issueStatusJSON)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		email, err := cfg.GitClient().UserEmail(ctx)
		if err != nil || strings.TrimSpace(email) == "" {
			return fmt.Errorf("could not determine user email from git config 'user.email'.\n\n" +
				"Remediation:\n" +
				"  Configure your git email address:\n" +
				"    git config user.email \"you@example.com\"")
		}
		email = strings.TrimSpace(email)
		profile := cfg.GetProfile(ctx)

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		assignedResp, err := client.ListIssues(ctx, fmt.Sprintf("status:open assignee:%s", email), 25, "")
		if err != nil {
			return err
		}
		reportedResp, err := client.ListIssues(ctx, fmt.Sprintf("status:open reporter:%s", email), 25, "")
		if err != nil {
			return err
		}

		if len(jsonFields) > 0 {
			assignedMaps := make([]map[string]any, 0, len(assignedResp.Issues))
			for _, iss := range assignedResp.Issues {
				assignedMaps = append(assignedMaps, IssueToJSONMap(iss, nil, profile, jsonFields))
			}
			reportedMaps := make([]map[string]any, 0, len(reportedResp.Issues))
			for _, iss := range reportedResp.Issues {
				reportedMaps = append(reportedMaps, IssueToJSONMap(iss, nil, profile, jsonFields))
			}
			payload := map[string]any{
				"assigned": assignedMaps,
				"reported": reportedMaps,
			}
			data, err := json.MarshalIndent(payload, "", "  ")
			if err != nil {
				return err
			}
			fmt.Fprintln(cmd.OutOrStdout(), string(data))
			return nil
		}

		formatWS := func(id int64) string {
			if RegisteredWorkspaceIntegration == nil || !RegisteredWorkspaceIntegration.IsEnabled() {
				return ""
			}
			ws, ok := RegisteredWorkspaceIntegration.FindWorkspaceForIssue(id)
			if !ok {
				return ""
			}
			if ws.Residency == "MOUNTED" {
				return fmt.Sprintf("  [%s MOUNTED (%s): %s]", GlyphRunning, ws.Slot, ws.SymlinkPath)
			}
			return fmt.Sprintf("  [%s PARKED: branch %s]", GlyphPending, ws.Branch)
		}

		out := cmd.OutOrStdout()
		shortEmail := shortIssueUser(email)
		fmt.Fprintf(out, "Issues assigned to you (%s):\n", shortEmail)
		if len(assignedResp.Issues) == 0 {
			fmt.Fprintln(out, "  None")
		} else {
			shown := assignedResp.Issues
			if !VerboseFlag && len(shown) > maxDefaultIssueStatusItems {
				shown = shown[:maxDefaultIssueStatusItems]
			}
			for _, iss := range shown {
				fmt.Fprintf(out, "  b/%-9d  %-4s  %s%s\n",
					iss.IssueID, iss.State.Priority, SanitizeUntrustedText(iss.State.Title), formatWS(int64(iss.IssueID)))
			}
			if !VerboseFlag && len(assignedResp.Issues) > maxDefaultIssueStatusItems {
				fmt.Fprintf(out, "  ... and %d more issues (use 'gh issue list --assignee me' or '-v' to view all)\n",
					len(assignedResp.Issues)-maxDefaultIssueStatusItems)
			}
		}

		fmt.Fprintf(out, "\nIssues opened by you (%s):\n", shortEmail)
		if len(reportedResp.Issues) == 0 {
			fmt.Fprintln(out, "  None")
		} else {
			shown := reportedResp.Issues
			if !VerboseFlag && len(shown) > maxDefaultIssueStatusItems {
				shown = shown[:maxDefaultIssueStatusItems]
			}
			for _, iss := range shown {
				fmt.Fprintf(out, "  b/%-9d  %-4s  %s%s\n",
					iss.IssueID, iss.State.Priority, SanitizeUntrustedText(iss.State.Title), formatWS(int64(iss.IssueID)))
			}
			if !VerboseFlag && len(reportedResp.Issues) > maxDefaultIssueStatusItems {
				fmt.Fprintf(out, "  ... and %d more issues (use 'gh issue list --assignee me' or '-v' to view all)\n",
					len(reportedResp.Issues)-maxDefaultIssueStatusItems)
			}
		}

		fmt.Fprintf(out, "\nVerdict: %d assigned, %d reported\n", len(assignedResp.Issues), len(reportedResp.Issues))
		if len(assignedResp.Issues) > 0 {
			fmt.Fprintf(out, "Next: gh issue develop %d\n", assignedResp.Issues[0].IssueID)
		} else if len(reportedResp.Issues) > 0 {
			fmt.Fprintf(out, "Next: gh issue develop %d\n", reportedResp.Issues[0].IssueID)
		} else {
			fmt.Fprintln(out, "Next: gh issue create")
		}
		return nil
	},
}

var (
	issueCreateTitle     string
	issueCreateBody      string
	issueCreateBodyFile  string
	issueCreateAssignee  string
	issueCreateLabels    []string
	issueCreatePriority  string
	issueCreateSeverity  string
	issueCreateType      string
	issueCreateComponent int64
	issueCreateCCs       []string
	issueCreateAmend     bool
)

var issueCreateCmd = &cobra.Command{
	Use:         "create",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Create a new Buganizer issue",
	Args:        cobra.NoArgs,
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		title := strings.TrimSpace(issueCreateTitle)
		if title == "" {
			return fmt.Errorf("--title (-t) is required when creating an issue.\n\n" +
				"Example:\n" +
				"  gh issue create -t \"pw_rpc: Fix channel deadlock\" -b \"Description...\" [-P P1] [--amend]")
		}

		body, err := readBodyInput(issueCreateBody, issueCreateBodyFile, cmd.InOrStdin())
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}

		var trailerFmt string
		if issueCreateAmend {
			if err := cfg.GitClient().CheckAmendAllowed(ctx); err != nil {
				return err
			}
			trailerFmt, err = ResolveBugTrailerFormat(ctx, cfg)
			if err != nil {
				return err
			}
		}

		pcfg, err := cfg.LoadProjectConfig(ctx)
		if err != nil {
			return err
		}

		compID, err := resolveComponentID(ctx, cfg, issueCreateComponent)
		if err != nil {
			return err
		}

		state := BuganizerState{
			ComponentID: FlexInt64(compID),
			Type:        "BUG",
			Priority:    "P2",
			Status:      "NEW",
			Title:       title,
		}

		for _, l := range issueCreateLabels {
			if _, err := ApplyLabelToState(&state, pcfg.ExpandIssueLabel(l)); err != nil {
				return err
			}
		}
		if issueCreatePriority != "" {
			var parsed BuganizerState
			if field, err := ApplyLabelToState(&parsed, issueCreatePriority); err != nil || field != "priority" {
				return fmt.Errorf("invalid --priority %q; expected P0..P4", issueCreatePriority)
			}
			state.Priority = parsed.Priority
		}
		if issueCreateSeverity != "" {
			var parsed BuganizerState
			if field, err := ApplyLabelToState(&parsed, issueCreateSeverity); err != nil || field != "severity" {
				return fmt.Errorf("invalid --severity %q; expected S0..S4", issueCreateSeverity)
			}
			state.Severity = parsed.Severity
		}
		if issueCreateType != "" {
			var parsed BuganizerState
			if field, err := ApplyLabelToState(&parsed, issueCreateType); err != nil || field != "type" {
				return fmt.Errorf("invalid --type %q; expected bug, feature, task, cleanup, process, or vulnerability", issueCreateType)
			}
			state.Type = parsed.Type
		}

		if state.ComponentID <= 0 {
			return fmt.Errorf("no Buganizer component ID configured for this project.\n\n" +
				"Remediation:\n" +
				"  1. Specify a component ID via flag:\n" +
				"     gh issue create -C <component_id> -t ...\n" +
				"  2. Or set a default repository component ID in git config:\n" +
				"     git config ghish.componentid <component_id>")
		}

		if issueCreateAssignee != "" {
			assignee, err := resolveUserEmailArg(ctx, cfg, issueCreateAssignee)
			if err != nil {
				return err
			}
			state.Assignee = &BuganizerUser{EmailAddress: assignee}
			state.Status = "ASSIGNED"
		}

		for _, cc := range issueCreateCCs {
			for _, email := range strings.Split(cc, ",") {
				email = strings.TrimSpace(email)
				if email != "" {
					state.CCs = append(state.CCs, BuganizerUser{EmailAddress: email})
				}
			}
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		hotlistsToAdd := append([]FlexInt64(nil), state.HotlistIDs...)
		state.HotlistIDs = nil

		req := &CreateIssueRequest{
			IssueState: state,
		}
		if body != "" {
			req.IssueComment = NewMarkdownComment(body)
		}

		created, err := client.CreateIssue(ctx, req)
		if err != nil {
			return err
		}
		for _, hid := range hotlistsToAdd {
			if err := client.CreateHotlistEntry(ctx, int64(hid), int64(created.IssueID)); err != nil {
				return err
			}
		}

		profile := cfg.GetProfile(ctx)
		out := cmd.OutOrStdout()
		var details []ReceiptField

		if issueCreateAmend {
			origMsg, err := cfg.GitClient().CommitMessage(ctx, "HEAD")
			if err != nil {
				return fmt.Errorf("created issue b/%d, but failed to read HEAD commit message for --amend: %w", created.IssueID, err)
			}

			existingLinks := ExtractBugLinks(origMsg)
			var bugValues []string
			for _, link := range existingLinks {
				if !link.Closes && !strings.EqualFold(link.ID, "None") {
					if id, err := ParseIssueID(link.ID); err == nil {
						if id == int64(created.IssueID) {
							continue
						}
						bugValues = append(bugValues, FormatBugIDWithFormat(id, trailerFmt))
						continue
					}
					bugValues = append(bugValues, link.ID)
				}
			}
			bugValues = append(bugValues, FormatBugIDWithFormat(int64(created.IssueID), trailerFmt))

			trailerKeyName := "Bug"
			if trailerFmt != "" {
				if kPart, _, hasColon := strings.Cut(trailerFmt, ":"); hasColon && strings.TrimSpace(kPart) != "" {
					trailerKeyName = strings.TrimSpace(kPart)
				}
			}
			trailerLine := fmt.Sprintf("%s: %s", trailerKeyName, strings.Join(bugValues, ", "))

			updatedMsg, err := UpsertTrailer(origMsg, trailerLine)
			if err != nil {
				return fmt.Errorf("created issue b/%d, but failed to update trailer: %w", created.IssueID, err)
			}
			if err := cfg.GitClient().CommitAmendMessage(ctx, updatedMsg, false, out, cmd.ErrOrStderr()); err != nil {
				return fmt.Errorf("created issue b/%d, but git commit --amend failed: %w", created.IssueID, err)
			}
			details = append(details, ReceiptField{
				Key:   "Amended",
				Value: fmt.Sprintf("HEAD commit with '%s'", trailerLine),
			})
		}

		PrintReceipt(out, Receipt{
			Summary: fmt.Sprintf("Created issue b/%d: %s", created.IssueID, SanitizeUntrustedText(created.State.Title)),
			Details: details,
			URL:     profile.IssueWebURL(int64(created.IssueID)),
			Next:    fmt.Sprintf("gh issue develop %d", created.IssueID),
		})
		return nil
	},
}

var (
	issueEditTitle          string
	issueEditAddAssignee    string
	issueEditRemoveAssignee string
	issueEditAddLabels      []string
	issueEditRemoveLabels   []string
	issueEditAddHotlists    []string
	issueEditRemoveHotlists []string
	issueEditPriority       string
	issueEditSeverity       string
	issueEditStatus         string
	issueEditType           string
	issueEditComponent      int64
)

var issueEditCmd = &cobra.Command{
	Use:         "edit [<number> | <url>]",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Edit an existing Buganizer issue",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		pcfg, err := cfg.LoadProjectConfig(ctx)
		if err != nil {
			return err
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		current, err := client.GetIssue(ctx, issueID)
		if err != nil {
			return err
		}

		mod := NewIssueModifier(current)

		if cmd.Flags().Changed("title") {
			if err := mod.SetTitle(issueEditTitle); err != nil {
				return err
			}
		}

		if issueEditPriority != "" {
			if err := mod.SetPriority(issueEditPriority); err != nil {
				return err
			}
		}
		if issueEditSeverity != "" {
			if err := mod.SetSeverity(issueEditSeverity); err != nil {
				return err
			}
		}
		if issueEditType != "" {
			if err := mod.SetType(issueEditType); err != nil {
				return err
			}
		}
		if issueEditComponent > 0 {
			mod.SetComponent(issueEditComponent)
		}

		for _, l := range issueEditAddLabels {
			if err := mod.AddLabel(pcfg.ExpandIssueLabel(l)); err != nil {
				return err
			}
		}
		for _, l := range issueEditRemoveLabels {
			if err := mod.RemoveLabel(pcfg.ExpandIssueLabel(l)); err != nil {
				return err
			}
		}
		for _, h := range issueEditAddHotlists {
			lbl := "hotlist:" + strings.TrimPrefix(strings.TrimSpace(h), "hotlist:")
			if err := mod.AddLabel(pcfg.ExpandIssueLabel(lbl)); err != nil {
				return err
			}
		}
		for _, h := range issueEditRemoveHotlists {
			lbl := "hotlist:" + strings.TrimPrefix(strings.TrimSpace(h), "hotlist:")
			if err := mod.RemoveLabel(pcfg.ExpandIssueLabel(lbl)); err != nil {
				return err
			}
		}

		if issueEditAddAssignee != "" {
			assigneeEmail, err := resolveUserEmailArg(ctx, cfg, issueEditAddAssignee)
			if err != nil {
				return err
			}
			mod.SetAssignee(assigneeEmail)
		}
		if issueEditRemoveAssignee != "" {
			mod.RemoveAssignee()
		}
		if issueEditStatus != "" {
			if err := mod.SetStatus(issueEditStatus); err != nil {
				return err
			}
		}

		if !mod.HasChanges() {
			return fmt.Errorf("no edits specified.\n\n" +
				"Example:\n" +
				"  gh issue edit 345678 -t \"New title\" -P P1 --add-assignee user@google.com")
		}

		updated := current
		if mod.HasMetadataChanges() {
			var err error
			updated, err = client.ModifyIssue(ctx, issueID, mod.BuildRequest())
			if err != nil {
				return err
			}
		}
		for _, hid := range mod.AddedHotlistIDs() {
			if err := client.CreateHotlistEntry(ctx, hid, issueID); err != nil {
				return err
			}
		}
		for _, hid := range mod.RemovedHotlistIDs() {
			if err := client.DeleteHotlistEntry(ctx, hid, issueID); err != nil {
				return err
			}
		}

		profile := cfg.GetProfile(ctx)
		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: fmt.Sprintf("Updated issue b/%d (%s)", updated.IssueID, updated.State.Status),
			URL:     profile.IssueWebURL(int64(updated.IssueID)),
			Next:    fmt.Sprintf("gh issue view %d", updated.IssueID),
		})
		return nil
	},
}

var (
	issueCloseComment     string
	issueCloseMessage     string
	issueCloseReason      string
	issueCloseDuplicateOf int64
)

var issueCloseCmd = &cobra.Command{
	Use:         "close [<number> | <url>]",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Close a Buganizer issue",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		commentText := strings.TrimSpace(issueCloseComment)
		if strings.TrimSpace(issueCloseMessage) != "" {
			if commentText != "" && commentText != strings.TrimSpace(issueCloseMessage) {
				return fmt.Errorf("cannot specify both --comment (-c) and --message (-m)")
			}
			commentText = strings.TrimSpace(issueCloseMessage)
		}

		targetStatus := "FIXED"
		if issueCloseDuplicateOf > 0 {
			targetStatus = "DUPLICATE"
		}
		if issueCloseReason != "" {
			norm, err := NormalizeIssueStatus(issueCloseReason)
			if err != nil || IsIssueOpen(norm) {
				return fmt.Errorf("invalid --reason %q; valid choices are 'completed' ('fixed'), 'not planned' ('obsolete'), 'wontfix' ('infeasible' / 'not_reproducible'), 'intended_behavior' ('wai'), or 'duplicate'", issueCloseReason)
			}
			if norm == "DUPLICATE" && issueCloseDuplicateOf <= 0 {
				return fmt.Errorf("--reason duplicate requires --duplicate-of <issue_id>")
			}
			if issueCloseDuplicateOf > 0 && norm != "DUPLICATE" {
				return fmt.Errorf("--duplicate-of can only be used with --reason duplicate (got %q)", issueCloseReason)
			}
			targetStatus = norm
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		updated, err := client.CloseIssue(ctx, issueID, targetStatus, issueCloseDuplicateOf, commentText)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		profile := cfg.GetProfile(ctx)
		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: fmt.Sprintf("Closed issue b/%d as %s", updated.IssueID, updated.State.Status),
			URL:     profile.IssueWebURL(int64(updated.IssueID)),
			Next:    fmt.Sprintf("gh issue view %d", updated.IssueID),
		})
		return nil
	},
}

var (
	issueReopenComment string
	issueReopenMessage string
)

var issueReopenCmd = &cobra.Command{
	Use:         "reopen [<number> | <url>]",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Reopen a closed Buganizer issue",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		commentText := strings.TrimSpace(issueReopenComment)
		if strings.TrimSpace(issueReopenMessage) != "" {
			if commentText != "" && commentText != strings.TrimSpace(issueReopenMessage) {
				return fmt.Errorf("cannot specify both --comment (-c) and --message (-m)")
			}
			commentText = strings.TrimSpace(issueReopenMessage)
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		updated, err := client.ReopenIssue(ctx, issueID, commentText)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		profile := cfg.GetProfile(ctx)
		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: fmt.Sprintf("Reopened issue b/%d (%s)", updated.IssueID, updated.State.Status),
			URL:     profile.IssueWebURL(int64(updated.IssueID)),
			Next:    fmt.Sprintf("gh issue develop %d", updated.IssueID),
		})
		return nil
	},
}

var (
	issueCommentBody     string
	issueCommentMessage  string
	issueCommentBodyFile string
)

var issueCommentCmd = &cobra.Command{
	Use:         "comment [<number> | <url>]",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Add a comment to a Buganizer issue",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		if issueCommentMessage != "" && issueCommentBody != "" {
			return fmt.Errorf("cannot specify both --message (-m) and --body (-b)")
		}
		if issueCommentMessage != "" && issueCommentBodyFile != "" {
			return fmt.Errorf("cannot specify both --message (-m) and --body-file (-F)")
		}
		rawBody := issueCommentBody
		if rawBody == "" {
			rawBody = issueCommentMessage
		}
		body, err := readBodyInput(rawBody, issueCommentBodyFile, cmd.InOrStdin())
		if err != nil {
			return err
		}
		if strings.TrimSpace(body) == "" {
			return fmt.Errorf("comment body cannot be empty; pass --message (-m), --body (-b), or --body-file (-F)")
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		comment, err := client.CreateComment(ctx, issueID, body)
		if err != nil {
			return err
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}
		profile := cfg.GetProfile(ctx)
		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: fmt.Sprintf("Added comment #%d to issue b/%d", comment.CommentNumber, issueID),
			URL:     profile.IssueWebURL(issueID),
			Next:    fmt.Sprintf("gh issue view %d --comments", issueID),
		})
		return nil
	},
}

var (
	issueDevelopBase     string
	issueDevelopName     string
	issueDevelopWorktree bool
	issueDevelopJSON     bool
)

var issueDevelopCmd = &cobra.Command{
	Use:         "develop [<number> | <url>]",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Create and check out a development branch (or warm worktree slot) for a Buganizer issue",
	Args:        cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		issueID, err := resolveTargetIssueID(ctx, cmd, args)
		if err != nil {
			return err
		}

		client, err := NewIssueTrackerClientForCommand(ctx, cmd)
		if err != nil {
			return err
		}

		issue, err := client.GetIssue(ctx, issueID)
		if err != nil {
			return err
		}

		branchName := strings.TrimSpace(issueDevelopName)
		if branchName == "" {
			branchName = SlugifyBranchName(issueID, issue.State.Title)
		}

		cfg := GetConfig(cmd)
		if cfg == nil {
			cfg = &Config{Host: HostFlag, Git: DefaultGitRunner}
		}

		useWorktree := issueDevelopWorktree
		if !useWorktree {
			if autoWT, cfgErr := cfg.GitClient().ConfigGet(ctx, "ghish.worktree.auto"); cfgErr == nil && strings.EqualFold(strings.TrimSpace(autoWT), "true") {
				useWorktree = true
			}
		}

		profile := cfg.GetProfile(ctx)
		if useWorktree {
			if RegisteredWorkspaceIntegration == nil {
				return fmt.Errorf("worktree integration is not available in this build")
			}
			symlinkPath, slotName, wtErr := RegisteredWorkspaceIntegration.DevelopIssueInWorktree(ctx, issueID, issue.State.Title, branchName)
			if wtErr != nil {
				return wtErr
			}
			if issueDevelopJSON {
				return writeIssueDevelopJSON(cmd.OutOrStdout(), map[string]any{
					"issue_id":     int64(issue.IssueID),
					"branch":       branchName,
					"slot":         slotName,
					"symlink_path": symlinkPath,
				})
			}
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: fmt.Sprintf("Mounted issue b/%d (%s) in warm worktree slot %s",
					issue.IssueID, SanitizeUntrustedText(issue.State.Title), slotName),
				Details: []ReceiptField{
					{Key: "Directory", Value: symlinkPath},
					{Key: "Branch", Value: branchName},
				},
				URL:  profile.IssueWebURL(int64(issue.IssueID)),
				Next: fmt.Sprintf("cd %s", symlinkPath),
			})
			return nil
		}

		gitArgs := []string{"checkout", "-b", branchName}
		if issueDevelopBase != "" {
			gitArgs = append(gitArgs, issueDevelopBase)
		}

		// Under --json, stdout carries only the JSON payload, so git's own
		// output goes to stderr.
		gitOut := cmd.OutOrStdout()
		if issueDevelopJSON {
			gitOut = cmd.ErrOrStderr()
		}
		if err := cfg.GitClient().Run(ctx, gitOut, cmd.ErrOrStderr(), gitArgs...); err != nil {
			return fmt.Errorf("failed to create development branch %q: %w", branchName, err)
		}
		if issueDevelopJSON {
			return writeIssueDevelopJSON(cmd.OutOrStdout(), map[string]any{
				"issue_id": int64(issue.IssueID),
				"branch":   branchName,
			})
		}

		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: fmt.Sprintf("Checked out new branch %q for issue b/%d (%s)",
				branchName, issue.IssueID, SanitizeUntrustedText(issue.State.Title)),
			Details: []ReceiptField{
				{Key: "Branch", Value: branchName},
			},
			URL:  profile.IssueWebURL(int64(issue.IssueID)),
			Next: "gh pr create",
		})
		if RegisteredWorkspaceIntegration != nil && RegisteredWorkspaceIntegration.IsEnabled() {
			fmt.Fprintf(cmd.ErrOrStderr(), "%s Tip: Run './gh issue develop %d --worktree' (or './gh wt use --issue %d') to open issues in isolated warm worktree slots.\n",
				GlyphArrow, issue.IssueID, issue.IssueID)
		}
		return nil
	},
}

// writeIssueDevelopJSON prints the `issue develop --json` payload: the
// issue (issue_id as a JSON number, as in `wt use --json`) and branch, plus
// the slot and symlink path when a worktree slot was allocated.
func writeIssueDevelopJSON(out io.Writer, payload map[string]any) error {
	data, err := json.MarshalIndent(payload, "", "  ")
	if err != nil {
		return fmt.Errorf("encoding issue develop JSON: %w", err)
	}
	_, err = fmt.Fprintln(out, string(data))
	return err
}

func init() {
	issueViewCmd.Flags().BoolVarP(&issueViewComments, "comments", "c", false, "View issue comments")
	//ghish:flag mode=extension reason="Show all raw issue comments including full bot commit dumps"
	issueViewCmd.Flags().BoolVar(&issueViewAll, "all", false, "Show all raw comments without collapsing bot commit messages (ghish-only)")
	issueViewCmd.Flags().BoolVarP(&issueViewWeb, "web", "w", false, "Open issue in browser")
	issueViewCmd.Flags().StringVar(&issueViewJSON, "json", "", "Output JSON with the specified comma-separated fields")

	issueListCmd.Flags().StringVarP(&issueListAssignee, "assignee", "a", "", "Filter by assignee email (or 'me')")
	issueListCmd.Flags().StringVarP(&issueListAuthor, "author", "A", "", "Filter by reporter/author email (or 'me')")
	issueListCmd.Flags().StringSliceVarP(&issueListLabels, "label", "l", nil, "Filter by priority/severity/type/hotlist/component label (prefix with '-' or '!' to negate)")
	issueListCmd.Flags().StringVarP(&issueListState, "state", "s", "open", "Filter by state: {open|closed|all}")
	issueListCmd.Flags().StringVarP(&issueListSearch, "search", "S", "", "Search issues with a Buganizer query")
	issueListCmd.Flags().IntVarP(&issueListLimit, "limit", "L", 30, "Maximum number of issues to fetch")
	issueListCmd.Flags().Int64VarP(&issueListComponent, "component", "C", 0, "Filter by Buganizer component ID")
	issueListCmd.Flags().BoolVar(&issueListAllComponents, "all-components", false, "Search across all Buganizer components")
	issueListCmd.Flags().StringVar(&issueListJSON, "json", "", "Output JSON with the specified comma-separated fields")

	issueStatusCmd.Flags().StringVar(&issueStatusJSON, "json", "", "Output JSON with the specified comma-separated fields")

	issueCreateCmd.Flags().StringVarP(&issueCreateTitle, "title", "t", "", "Issue title (required)")
	issueCreateCmd.Flags().StringVarP(&issueCreateBody, "body", "b", "", "Issue description body")
	issueCreateCmd.Flags().StringVarP(&issueCreateBodyFile, "body-file", "F", "", "Read description body from file (use \"-\" to read from standard input)")
	issueCreateCmd.Flags().StringVarP(&issueCreateAssignee, "assignee", "a", "", "Assign issue to email (or 'me')")
	issueCreateCmd.Flags().StringSliceVarP(&issueCreateLabels, "label", "l", nil, "Add structured labels (e.g. P1, S2, bug, vulnerability, hotlist:<id|alias>)")
	issueCreateCmd.Flags().StringVarP(&issueCreatePriority, "priority", "P", "", "Buganizer priority (P0..P4)")
	//ghish:flag mode=extension reason="Set Buganizer issue severity S0..S4"
	issueCreateCmd.Flags().StringVarP(&issueCreateSeverity, "severity", "S", "", "Buganizer severity (S0..S4) (ghish-only)")
	issueCreateCmd.Flags().StringVarP(&issueCreateType, "type", "T", "", "Buganizer issue type (bug, feature, task, cleanup, process, vulnerability)")
	issueCreateCmd.Flags().Int64VarP(&issueCreateComponent, "component", "C", 0, "Buganizer component ID")
	issueCreateCmd.Flags().StringSliceVar(&issueCreateCCs, "cc", nil, "Comma-separated emails to CC")
	issueCreateCmd.Flags().BoolVar(&issueCreateAmend, "amend", false, "Amend current Git HEAD commit with 'Bug: b/<new-id>'")

	issueEditCmd.Flags().StringVarP(&issueEditTitle, "title", "t", "", "New issue title")
	issueEditCmd.Flags().StringVar(&issueEditAddAssignee, "add-assignee", "", "Set issue assignee email")
	issueEditCmd.Flags().StringVar(&issueEditRemoveAssignee, "remove-assignee", "", "Remove issue assignee")
	issueEditCmd.Flags().StringSliceVarP(&issueEditAddLabels, "add-label", "l", nil, "Add priority/severity/type/hotlist labels")
	issueEditCmd.Flags().StringSliceVar(&issueEditRemoveLabels, "remove-label", nil, "Remove priority/severity/type/hotlist labels")
	//ghish:flag mode=extension reason="Add issue to Buganizer hotlist ID or alias"
	issueEditCmd.Flags().StringSliceVar(&issueEditAddHotlists, "add-hotlist", nil, "Add issue to Buganizer hotlist ID or alias (ghish-only)")
	//ghish:flag mode=extension reason="Remove issue from Buganizer hotlist ID or alias"
	issueEditCmd.Flags().StringSliceVar(&issueEditRemoveHotlists, "remove-hotlist", nil, "Remove issue from Buganizer hotlist ID or alias (ghish-only)")
	issueEditCmd.Flags().StringVarP(&issueEditPriority, "priority", "P", "", "Set Buganizer priority (P0..P4)")
	//ghish:flag mode=extension reason="Set Buganizer issue severity S0..S4"
	issueEditCmd.Flags().StringVarP(&issueEditSeverity, "severity", "S", "", "Set Buganizer severity (S0..S4) (ghish-only)")
	//ghish:flag mode=extension reason="Set Buganizer issue status"
	issueEditCmd.Flags().StringVar(&issueEditStatus, "status", "", "Set Buganizer status (NEW, ASSIGNED, ACCEPTED, FIXED, VERIFIED, INTENDED_BEHAVIOR, INFEASIBLE, OBSOLETE, DUPLICATE) (ghish-only)")
	issueEditCmd.Flags().StringVarP(&issueEditType, "type", "T", "", "Set Buganizer issue type (bug, feature, task, cleanup, process, vulnerability)")
	issueEditCmd.Flags().Int64VarP(&issueEditComponent, "component", "C", 0, "Move issue to Buganizer component ID")

	issueCloseCmd.Flags().StringVarP(&issueCloseComment, "comment", "c", "", "Post a closing comment")
	//ghish:flag mode=extension reason="Alias for --comment on issue close"
	issueCloseCmd.Flags().StringVarP(&issueCloseMessage, "message", "m", "", "Post a closing comment (alias for --comment) (ghish-only)")
	issueCloseCmd.Flags().StringVarP(&issueCloseReason, "reason", "r", "", "Reason for closing: {completed|not planned|wontfix|not_reproducible|intended_behavior|duplicate}")
	issueCloseCmd.Flags().Int64Var(&issueCloseDuplicateOf, "duplicate-of", 0, "Close as DUPLICATE of another issue ID")

	issueReopenCmd.Flags().StringVarP(&issueReopenComment, "comment", "c", "", "Post a reopening comment")
	//ghish:flag mode=extension reason="Alias for --comment on issue reopen"
	issueReopenCmd.Flags().StringVarP(&issueReopenMessage, "message", "m", "", "Post a reopening comment (alias for --comment) (ghish-only)")

	issueCommentCmd.Flags().StringVarP(&issueCommentBody, "body", "b", "", "Comment text")
	//ghish:flag mode=extension reason="Alias for --body on issue comment"
	issueCommentCmd.Flags().StringVarP(&issueCommentMessage, "message", "m", "", "Comment text (alias for --body) (ghish-only)")
	issueCommentCmd.Flags().StringVarP(&issueCommentBodyFile, "body-file", "F", "", "Read comment text from file (use \"-\" to read from standard input)")

	issueDevelopCmd.Flags().StringVarP(&issueDevelopBase, "base", "b", "", "Base branch to branch from")
	issueDevelopCmd.Flags().StringVarP(&issueDevelopName, "name", "n", "", "Custom branch name")
	issueDevelopCmd.Flags().BoolVarP(&issueDevelopWorktree, "worktree", "w", false, "Allocate or mount an isolated warm worktree slot via gh wt")
	issueDevelopCmd.Flags().BoolVar(&issueDevelopJSON, "json", false, "Output the issue, branch, and any allocated worktree slot as JSON")

	IssueCmd.AddCommand(issueViewCmd)
	IssueCmd.AddCommand(issueListCmd)
	IssueCmd.AddCommand(issueStatusCmd)
	IssueCmd.AddCommand(issueCreateCmd)
	IssueCmd.AddCommand(issueEditCmd)
	IssueCmd.AddCommand(issueCloseCmd)
	IssueCmd.AddCommand(issueReopenCmd)
	IssueCmd.AddCommand(issueCommentCmd)
	IssueCmd.AddCommand(issueDevelopCmd)
}
