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
	"bufio"
	"bytes"
	"context"
	"fmt"
	"io"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

// CodeOwnerInfo represents a single code owner suggestion returned by Gerrit's
// code-owners plugin.
type CodeOwnerInfo struct {
	Account  gerrit.AccountInfo `json:"account"`
	Scorings map[string]float64 `json:"scorings"`
}

type codeOwnersResponse struct {
	CodeOwners []CodeOwnerInfo `json:"code_owners"`
}

// IsOwnersToken reports whether a reviewer argument requests automatic code
// owner resolution (@owners or owners).
//
// Supporting both the @-prefixed form and bare keyword mirrors how `gh pr list`
// and `gh issue list` handle `@me` / `me`, while aligning with upstream GitHub
// CLI's `@<keyword>` special reviewer syntax (e.g. `--add-reviewer @copilot`).
func IsOwnersToken(reviewer string) bool {
	switch strings.ToLower(strings.TrimSpace(reviewer)) {
	case "@owners", "owners":
		return true
	default:
		return false
	}
}

// HasOwnersToken reports whether any entry in reviewers (including
// comma-separated lists) is an @owners token.
func HasOwnersToken(reviewers []string) bool {
	return hasOwnersTokenInItems(splitReviewerArgs(reviewers))
}

func hasOwnersTokenInItems(items []string) bool {
	for _, item := range items {
		if IsOwnersToken(item) {
			return true
		}
	}
	return false
}

// splitReviewerArgs flattens repeated and comma-separated reviewer arguments.
func splitReviewerArgs(raw []string) []string {
	var out []string
	for _, entry := range raw {
		for _, part := range strings.Split(entry, ",") {
			if trimmed := strings.TrimSpace(part); trimmed != "" {
				out = append(out, trimmed)
			}
		}
	}
	return out
}

// ExpandReviewersForChange expands any @owners tokens in reviewers using the
// change's touched files, Gerrit's code-owners API, and local OWNERS files.
// If reviewers contains no @owners tokens, it is returned without extra RPCs.
func ExpandReviewersForChange(cmd *cobra.Command, chCtx *ChangeContext, reviewers []string) ([]string, error) {
	items := splitReviewerArgs(reviewers)
	if !hasOwnersTokenInItems(items) {
		return items, nil
	}
	owners, err := ResolveCodeOwnersForChange(chCtx)
	if err != nil {
		return nil, err
	}
	if cmd != nil {
		fmt.Fprintf(cmd.OutOrStdout(), "Resolved @owners: %s\n", strings.Join(owners, ", "))
	}
	return replaceOwnersTokens(items, owners), nil
}

// ExpandReviewersForPush expands any @owners tokens in reviewers prior to
// pushing a change or patchset. If reviewers contains no @owners tokens, it is
// returned as-is without extra RPCs or git calls.
func ExpandReviewersForPush(ctx context.Context, cmd *cobra.Command, cfg *Config, branch string, existingChange *gerrit.ChangeInfo, reviewers []string) ([]string, error) {
	items := splitReviewerArgs(reviewers)
	if !hasOwnersTokenInItems(items) {
		return items, nil
	}
	owners, err := ResolveCodeOwnersForPush(ctx, cmd, cfg, branch, existingChange)
	if err != nil {
		return nil, err
	}
	if cmd != nil {
		fmt.Fprintf(cmd.OutOrStdout(), "Resolved @owners: %s\n", strings.Join(owners, ", "))
	}
	return replaceOwnersTokens(items, owners), nil
}

func replaceOwnersTokens(items []string, resolvedOwners []string) []string {
	var result []string
	seen := make(map[string]bool)
	add := func(email string) {
		key := strings.ToLower(strings.TrimSpace(email))
		if key == "" || seen[key] {
			return
		}
		seen[key] = true
		result = append(result, strings.TrimSpace(email))
	}

	for _, item := range items {
		if IsOwnersToken(item) {
			for _, owner := range resolvedOwners {
				add(owner)
			}
		} else {
			add(item)
		}
	}
	return result
}

// ResolveCodeOwnersForChange finds the minimal covering set of code owners for
// an existing Gerrit change.
func ResolveCodeOwnersForChange(chCtx *ChangeContext) ([]string, error) {
	if chCtx == nil || chCtx.Client == nil {
		return nil, fmt.Errorf("internal error: ChangeContext is not initialized")
	}
	ctx := chCtx.Context
	changeID := chCtx.ChangeID
	client := chCtx.Client

	authorEmail := resolveChangeAuthorEmail(chCtx)
	files := listChangeFiles(chCtx)
	repoRoot := resolveRepoRoot(ctx, chCtx.Config)

	queryFn := func(filePath string) ([]CodeOwnerInfo, error) {
		return queryChangeCodeOwners(ctx, client, changeID, "current", filePath)
	}

	owners := selectOwnersForFiles(files, authorEmail, repoRoot, queryFn)
	if len(owners) == 0 {
		return nil, noEligibleOwnersError(fmt.Sprintf("change %s", changeID))
	}
	return owners, nil
}

// ResolveCodeOwnersForPush finds the minimal covering set of code owners for a
// commit being pushed via `gh pr create` or `gh pr push`.
func ResolveCodeOwnersForPush(ctx context.Context, cmd *cobra.Command, cfg *Config, branch string, existingChange *gerrit.ChangeInfo) ([]string, error) {
	authorEmail := resolveLocalAuthorEmail(ctx, cfg, existingChange)
	files := listHeadChangedFiles(ctx, cfg)
	repoRoot := resolveRepoRoot(ctx, cfg)

	client, _ := NewGerritClient(ctx, cmd)
	var project string
	if cfg != nil {
		project, _ = cfg.GerritProject(ctx)
	}
	if existingChange != nil && existingChange.Project != "" {
		project = existingChange.Project
	}
	if branch == "" && existingChange != nil && existingChange.Branch != "" {
		branch = existingChange.Branch
	}
	if branch == "" && cfg != nil {
		branch = cfg.ResolveDefaultBranch(ctx)
	}
	if branch == "" {
		branch = "main"
	}

	if len(files) == 0 && existingChange != nil && client != nil {
		chID := fmt.Sprintf("%d", existingChange.Number)
		if existingChange.Number == 0 {
			chID = existingChange.ChangeID
		}
		if chID != "" {
			if fMap, _, err := client.Changes.ListFiles(ctx, chID, "current", nil); err == nil {
				files = extractModifiedFilePaths(fMap)
			}
		}
	}

	queryFn := func(filePath string) ([]CodeOwnerInfo, error) {
		if client == nil {
			return nil, fmt.Errorf("no gerrit client")
		}
		if existingChange != nil {
			chID := fmt.Sprintf("%d", existingChange.Number)
			if existingChange.Number == 0 {
				chID = existingChange.ChangeID
			}
			if chID != "" {
				if res, err := queryChangeCodeOwners(ctx, client, chID, "current", filePath); err == nil && len(res) > 0 {
					return res, nil
				}
			}
		}
		if project != "" {
			return queryBranchCodeOwners(ctx, client, project, branch, filePath)
		}
		return nil, fmt.Errorf("gerrit project unknown")
	}

	owners := selectOwnersForFiles(files, authorEmail, repoRoot, queryFn)
	if len(owners) == 0 {
		return nil, noEligibleOwnersError("HEAD commit")
	}
	return owners, nil
}

func noEligibleOwnersError(subject string) error {
	root := RootCmd.CommandPath()
	return fmt.Errorf("cannot resolve @owners for %s: no eligible non-author code owners were found for the modified files.\n\n"+
		"Specify a reviewer explicitly:\n"+
		"  %s pr ready -r <email>\n"+
		"  %s pr edit --add-reviewer <email>\n"+
		"  %s pr push -r <email>",
		subject, root, root, root)
}

func queryChangeCodeOwners(ctx context.Context, client *gerrit.Client, changeID, revision, filePath string) ([]CodeOwnerInfo, error) {
	if client == nil || changeID == "" || filePath == "" {
		return nil, fmt.Errorf("invalid arguments for queryChangeCodeOwners")
	}
	if revision == "" {
		revision = "current"
	}
	endpoint := fmt.Sprintf("changes/%s/revisions/%s/code_owners/%s?limit=10&o=DETAILS",
		url.PathEscape(changeID),
		url.PathEscape(revision),
		url.PathEscape(filePath),
	)
	var resp codeOwnersResponse
	if _, err := client.Call(ctx, "GET", endpoint, nil, &resp); err != nil {
		return nil, err
	}
	return resp.CodeOwners, nil
}

func queryBranchCodeOwners(ctx context.Context, client *gerrit.Client, project, branch, filePath string) ([]CodeOwnerInfo, error) {
	if client == nil || project == "" || branch == "" || filePath == "" {
		return nil, fmt.Errorf("invalid arguments for queryBranchCodeOwners")
	}
	endpoint := fmt.Sprintf("projects/%s/branches/%s/code_owners/%s?limit=10&o=DETAILS",
		url.PathEscape(project),
		url.PathEscape(branch),
		url.PathEscape(filePath),
	)
	var resp codeOwnersResponse
	if _, err := client.Call(ctx, "GET", endpoint, nil, &resp); err != nil {
		return nil, err
	}
	return resp.CodeOwners, nil
}

func resolveChangeAuthorEmail(chCtx *ChangeContext) string {
	if chCtx == nil {
		return ""
	}
	if change, err := chCtx.GetChange(&gerrit.ChangeOptions{
		AdditionalFields: []string{"DETAILED_ACCOUNTS"},
	}); err == nil && change != nil {
		// gerrit.ChangeInfo.Owner is a value struct (gerrit.AccountInfo), not a pointer.
		if e := strings.TrimSpace(change.Owner.Email); e != "" {
			return e
		}
	}
	return resolveLocalAuthorEmail(chCtx.Context, chCtx.Config, nil)
}

func resolveLocalAuthorEmail(ctx context.Context, cfg *Config, existing *gerrit.ChangeInfo) string {
	// gerrit.ChangeInfo.Owner is a value struct (gerrit.AccountInfo), not a pointer.
	if existing != nil && strings.TrimSpace(existing.Owner.Email) != "" {
		return strings.TrimSpace(existing.Owner.Email)
	}
	if cfg != nil {
		git := cfg.GitClient()
		if email, err := git.HeadAuthorEmail(ctx); err == nil && email != "" {
			return email
		}
		if email, err := git.UserEmail(ctx); err == nil && email != "" {
			return email
		}
	}
	return ""
}

func listChangeFiles(chCtx *ChangeContext) []string {
	if chCtx == nil || chCtx.Client == nil {
		return nil
	}
	rev := chCtx.Revision
	if rev == "" {
		rev = "current"
	}
	if fMap, _, err := chCtx.Client.Changes.ListFiles(chCtx.Context, chCtx.ChangeID, rev, nil); err == nil && len(fMap) > 0 {
		if paths := extractModifiedFilePaths(fMap); len(paths) > 0 {
			return paths
		}
	}
	return listHeadChangedFiles(chCtx.Context, chCtx.Config)
}

func extractModifiedFilePaths(fMap map[string]gerrit.FileInfo) []string {
	var paths []string
	for p := range fMap {
		if p == "/COMMIT_MSG" || p == "COMMIT_MSG" || p == "/MERGE_LIST" || p == "MERGE_LIST" || p == "/PATCHSET_LEVEL" || p == "PATCHSET_LEVEL" {
			continue
		}
		paths = append(paths, strings.TrimPrefix(p, "/"))
	}
	sort.Strings(paths)
	return paths
}

func listHeadChangedFiles(ctx context.Context, cfg *Config) []string {
	if cfg == nil {
		return nil
	}
	var stdout bytes.Buffer
	if err := cfg.GitClient().Run(ctx, &stdout, io.Discard, "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"); err != nil {
		return nil
	}
	var files []string
	for _, line := range strings.Split(stdout.String(), "\n") {
		if trimmed := strings.TrimSpace(line); trimmed != "" {
			files = append(files, trimmed)
		}
	}
	sort.Strings(files)
	return files
}

func resolveRepoRoot(ctx context.Context, cfg *Config) string {
	if cfg != nil {
		if top, err := cfg.GitClient().RevParse(ctx, "--show-toplevel"); err == nil && top != "" {
			return top
		}
		if cfg.CWD != "" {
			return cfg.CWD
		}
	}
	if wd, err := os.Getwd(); err == nil {
		return wd
	}
	return ""
}

// representativeFilesByDir picks one representative file path per unique
// directory so that a change touching 30 files in one module only queries
// code_owners once for that directory.
func representativeFilesByDir(files []string) []string {
	if len(files) == 0 {
		return []string{"OWNERS"}
	}
	seenDir := make(map[string]bool)
	var reps []string
	for _, f := range files {
		clean := filepath.ToSlash(strings.TrimPrefix(strings.TrimSpace(f), "/"))
		if clean == "" {
			continue
		}
		dir := path.Dir(clean)
		if !seenDir[dir] {
			seenDir[dir] = true
			reps = append(reps, clean)
		}
	}
	if len(reps) == 0 {
		return []string{"OWNERS"}
	}
	return reps
}

func isServiceAccountEmail(email string) bool {
	lower := strings.ToLower(strings.TrimSpace(email))
	return strings.HasSuffix(lower, "gserviceaccount.com") || strings.HasPrefix(lower, "gwsq-")
}

// selectOwnersForFiles resolves the minimal set of non-author owners covering
// all modified directories.
//
// For each touched directory, resolution checks:
//  1. Specific human module/directory owners from Gerrit's code-owners API
//     (DISTANCE <= 1, non-service-account).
//  2. Local subdirectory <dir>/OWNERS files in the checkout (handles newly
//     added OWNERS files before they land on the remote branch, and prefers
//     active members listed in root OWNERS / EXTENDED_OWNERS).
//  3. Fallback suggestions from Gerrit's code-owners API (such as gwsq-pigweed
//     or root OWNERS when a module has no dedicated OWNERS file).
//  4. Local root OWNERS / EXTENDED_OWNERS files when offline or when the
//     remote host does not run the code-owners plugin.
func selectOwnersForFiles(
	files []string,
	authorEmail string,
	repoRoot string,
	queryGerrit func(filePath string) ([]CodeOwnerInfo, error),
) []string {
	reps := representativeFilesByDir(files)
	activePool := loadActiveOwnersPool(repoRoot)

	var selected []string
	selectedSet := make(map[string]bool)

	addSelected := func(email string) {
		key := strings.ToLower(strings.TrimSpace(email))
		if key == "" || selectedSet[key] {
			return
		}
		selectedSet[key] = true
		selected = append(selected, strings.TrimSpace(email))
	}

	alreadyCoveredBy := func(candidates []string) bool {
		for _, c := range candidates {
			if selectedSet[strings.ToLower(strings.TrimSpace(c))] {
				return true
			}
		}
		return false
	}

	for _, rep := range reps {
		var gerritSpecificHumans []string
		var gerritFallbacks []string

		if queryGerrit != nil {
			if infos, err := queryGerrit(rep); err == nil {
				for _, info := range infos {
					email := strings.TrimSpace(info.Account.Email)
					if email == "" || strings.EqualFold(email, authorEmail) {
						continue
					}
					dist, hasDist := info.Scorings["DISTANCE"]
					if !isServiceAccountEmail(email) && (!hasDist || dist <= 1) {
						gerritSpecificHumans = append(gerritSpecificHumans, email)
					} else {
						gerritFallbacks = append(gerritFallbacks, email)
					}
				}
			}
		}

		// 1. Specific module/directory human owners from Gerrit code-owners API.
		if len(gerritSpecificHumans) > 0 {
			if !alreadyCoveredBy(gerritSpecificHumans) {
				addSelected(gerritSpecificHumans[0])
			}
			continue
		}

		// 2. Local subdirectory OWNERS file in the checkout.
		localSubdirOwners := findLocalSubdirOwners(repoRoot, rep, authorEmail, activePool)
		if len(localSubdirOwners) > 0 {
			if !alreadyCoveredBy(localSubdirOwners) {
				addSelected(localSubdirOwners[0])
			}
			continue
		}

		// 3. Fallback suggestions from Gerrit code-owners API (e.g. gwsq-pigweed or root owners).
		if len(gerritFallbacks) > 0 {
			if !alreadyCoveredBy(gerritFallbacks) {
				addSelected(gerritFallbacks[0])
			}
			continue
		}

		// 4. Local root OWNERS / EXTENDED_OWNERS fallback.
		localRootOwners := findLocalRootOwners(repoRoot, authorEmail)
		if len(localRootOwners) > 0 {
			if !alreadyCoveredBy(localRootOwners) {
				addSelected(localRootOwners[0])
			}
		}
	}

	return selected
}

type parsedOwnerEntry struct {
	Email        string
	IsLastResort bool
}

func parseLocalOwnersFile(path string) []parsedOwnerEntry {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()

	var entries []parsedOwnerEntry
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") ||
			strings.HasPrefix(line, "include ") || strings.HasPrefix(line, "per-file ") ||
			strings.HasPrefix(line, "set ") || line == "*" {
			continue
		}
		isLastResort := strings.Contains(line, "{LAST_RESORT_SUGGESTION}")
		emailPart := line
		if idx := strings.IndexByte(line, '#'); idx != -1 {
			emailPart = strings.TrimSpace(line[:idx])
		}
		if idx := strings.IndexAny(emailPart, " \t{"); idx != -1 {
			emailPart = strings.TrimSpace(emailPart[:idx])
		}
		if emailPart != "" && strings.Contains(emailPart, "@") {
			entries = append(entries, parsedOwnerEntry{
				Email:        emailPart,
				IsLastResort: isLastResort,
			})
		}
	}
	return entries
}

func loadActiveOwnersPool(repoRoot string) map[string]bool {
	pool := make(map[string]bool)
	if repoRoot == "" {
		return pool
	}
	for _, name := range []string{"OWNERS", "EXTENDED_OWNERS", "WORKSPACE_OWNERS"} {
		for _, entry := range parseLocalOwnersFile(filepath.Join(repoRoot, name)) {
			if !isServiceAccountEmail(entry.Email) {
				pool[strings.ToLower(entry.Email)] = true
			}
		}
	}
	return pool
}

func findLocalSubdirOwners(repoRoot, relFile, authorEmail string, activePool map[string]bool) []string {
	if repoRoot == "" {
		return nil
	}
	cleanRoot := filepath.Clean(repoRoot)
	absFile := filepath.Join(cleanRoot, filepath.FromSlash(relFile))
	dir := filepath.Dir(absFile)

	for strings.HasPrefix(dir, cleanRoot+string(filepath.Separator)) && dir != cleanRoot {
		ownersPath := filepath.Join(dir, "OWNERS")
		entries := parseLocalOwnersFile(ownersPath)
		if len(entries) > 0 {
			var activeCandidates []string
			var otherCandidates []string
			for _, e := range entries {
				if strings.EqualFold(e.Email, authorEmail) || isServiceAccountEmail(e.Email) {
					continue
				}
				if len(activePool) == 0 || activePool[strings.ToLower(e.Email)] {
					activeCandidates = append(activeCandidates, e.Email)
				} else {
					otherCandidates = append(otherCandidates, e.Email)
				}
			}
			if len(activeCandidates) > 0 {
				return activeCandidates
			}
			if len(otherCandidates) > 0 {
				return otherCandidates
			}
		}
		dir = filepath.Dir(dir)
	}
	return nil
}

func findLocalRootOwners(repoRoot, authorEmail string) []string {
	if repoRoot == "" {
		return nil
	}
	var primary []string
	var lastResort []string
	for _, name := range []string{"OWNERS", "EXTENDED_OWNERS"} {
		for _, e := range parseLocalOwnersFile(filepath.Join(repoRoot, name)) {
			if strings.EqualFold(e.Email, authorEmail) || isServiceAccountEmail(e.Email) {
				continue
			}
			if e.IsLastResort {
				lastResort = append(lastResort, e.Email)
			} else {
				primary = append(primary, e.Email)
			}
		}
	}
	if len(primary) > 0 {
		return primary
	}
	return lastResort
}
