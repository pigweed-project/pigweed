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
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	reStatus401 = regexp.MustCompile(`\b401\b`)
	reStatus403 = regexp.MustCompile(`\b403\b`)
	reStatus404 = regexp.MustCompile(`\b404\b`)
	reStatus409 = regexp.MustCompile(`\b409\b`)
)

// ChangeContext encapsulates the resolved change, command context, git config,
// and Gerrit client for a change-focused subcommand.
type ChangeContext struct {
	Context  context.Context
	Cmd      *cobra.Command
	Config   *Config
	Client   *gerrit.Client
	TargetID string
	ChangeID string
	Revision string
}

// ResolveChangeContext resolves the target change ID from args or git branch,
// parses change ID and revision, initializes the Gerrit client, and returns a ChangeContext.
// It fails fast if target ID resolution fails or client creation fails.
func ResolveChangeContext(cmd *cobra.Command, args []string) (*ChangeContext, error) {
	if cmd == nil {
		return nil, fmt.Errorf("internal error: cmd is nil")
	}
	ctx := cmd.Context()
	if ctx == nil {
		ctx = context.Background()
	}
	if err := ctx.Err(); err != nil {
		return nil, fmt.Errorf("context canceled before resolving change: %w", err)
	}

	targetID, err := ResolveTargetChangeID(ctx, cmd, args)
	if err != nil {
		return nil, err
	}
	changeID, revision := ParseChangeAndRevision(targetID)
	if changeID == "" {
		return nil, fmt.Errorf("internal error: parsed change ID is empty from %q", targetID)
	}

	client, err := NewGerritClient(ctx, cmd)
	if err != nil {
		return nil, fmt.Errorf("error creating Gerrit client: %w", err)
	}

	cfg := GetConfig(cmd)

	return &ChangeContext{
		Context:  ctx,
		Cmd:      cmd,
		Config:   cfg,
		Client:   client,
		TargetID: targetID,
		ChangeID: changeID,
		Revision: revision,
	}, nil
}

// GitClient returns the typed GitClient from Config, or an error if uninitialized.
func (c *ChangeContext) GitClient() (GitClient, error) {
	if c == nil {
		return nil, fmt.Errorf("internal error: ChangeContext is nil")
	}
	if c.Config == nil || c.Config.Git == nil {
		return nil, fmt.Errorf("git runner not initialized")
	}
	return c.Config.GitClient(), nil
}

// GetChange fetches the ChangeInfo from Gerrit for this change.
func (c *ChangeContext) GetChange(opt *gerrit.ChangeOptions) (*gerrit.ChangeInfo, error) {
	if c == nil {
		return nil, fmt.Errorf("internal error: ChangeContext is nil")
	}
	if c.Client == nil {
		return nil, fmt.Errorf("internal error: gerrit client is nil")
	}
	if c.ChangeID == "" {
		return nil, fmt.Errorf("internal error: changeID is empty")
	}
	ctx := c.Context
	if ctx == nil {
		ctx = context.Background()
	}
	change, _, err := c.Client.Changes.GetChange(ctx, c.ChangeID, opt)
	if err != nil {
		return nil, c.FormatError(err, "getting")
	}
	if change == nil {
		return nil, fmt.Errorf("gerrit returned empty change info for %s", c.ChangeID)
	}
	return change, nil
}

// trimHostScheme removes http(s) prefixes and trailing /a or slashes from a host name.
func trimHostScheme(host string) string {
	if host == "" {
		return ""
	}
	host = strings.TrimPrefix(strings.TrimPrefix(host, "https://"), "http://")
	host = strings.TrimSuffix(host, "/")
	host = strings.TrimSuffix(host, "/a")
	return strings.TrimSuffix(host, "/")
}

// FormatGerritError translates raw Gerrit REST errors into actionable messages with debug crumbs.
func FormatGerritError(err error, actionDesc string, changeID string, gerritHost string) error {
	if err == nil {
		return nil
	}
	errStr := err.Error()

	// 1. Authentication failures (HTTP 401 / 403)
	if reStatus401.MatchString(errStr) || strings.Contains(errStr, "Unauthorized") ||
		reStatus403.MatchString(errStr) || strings.Contains(errStr, "Forbidden") ||
		strings.Contains(errStr, "Authentication required") {
		cleanHost := trimHostScheme(gerritHost)
		if cleanHost == "" {
			cleanHost = "<host>"
		}
		return fmt.Errorf("error %s change %s: authentication required (HTTP 401/403).\n\n"+
			"To authenticate with %s:\n"+
			"  1. Generate Git cookies/credentials at: https://%s/new-password\n"+
			"  2. Or set a personal access token: export GERRIT_TOKEN=\"<token>\"\n"+
			"  3. (Corp users) Run 'gcert' to refresh single sign-on credentials.\n\n"+
			"Underlying error: %w",
			actionDesc, changeID, cleanHost, cleanHost, err)
	}

	// 2. Change Not Found (HTTP 404)
	if reStatus404.MatchString(errStr) || strings.Contains(errStr, "Not Found") {
		cleanHost := trimHostScheme(gerritHost)
		if cleanHost == "" {
			cleanHost = "Gerrit"
		}
		return fmt.Errorf("error %s change %s: change not found on %s (HTTP 404).\n\n"+
			"To search for changes:\n"+
			"  gh pr list\n"+
			"  gh pr list --state all\n"+
			"  gh pr list --author <username>\n\n"+
			"Underlying error: %w",
			actionDesc, changeID, cleanHost, err)
	}

	// 3. State conflicts (HTTP 409 Conflict)
	if reStatus409.MatchString(errStr) || strings.Contains(strings.ToLower(errStr), "conflict") {
		lower := strings.ToLower(errStr)
		if strings.Contains(lower, "merged") {
			return fmt.Errorf("error %s change %s: change is already merged and cannot be modified.\n\nUnderlying error: %w", actionDesc, changeID, err)
		}
		if strings.Contains(lower, "abandoned") {
			return fmt.Errorf("error %s change %s: change is already closed (abandoned).\n\nUnderlying error: %w", actionDesc, changeID, err)
		}
		if strings.Contains(lower, "not work in progress") || strings.Contains(lower, "not draft") || strings.Contains(lower, "not wip") {
			return fmt.Errorf("error %s change %s: change is already marked as ready for review (not in work-in-progress/draft state).\n\nUnderlying error: %w", actionDesc, changeID, err)
		}
		if strings.Contains(lower, "new") {
			return fmt.Errorf("error %s change %s: change is already open.\n\nUnderlying error: %w", actionDesc, changeID, err)
		}

		if actionDesc == "closing" {
			return fmt.Errorf("error %s change %s: change cannot be closed in its current state (it is likely already merged or closed).\n\nUnderlying error: %w", actionDesc, changeID, err)
		}
		if actionDesc == "reopening" {
			return fmt.Errorf("error %s change %s: change cannot be reopened in its current state (it is likely already open or merged).\n\nUnderlying error: %w", actionDesc, changeID, err)
		}
		if strings.Contains(actionDesc, "ready") {
			return fmt.Errorf("error %s change %s: change is already marked as ready for review (not in work-in-progress/draft state).\n\nUnderlying error: %w", actionDesc, changeID, err)
		}

		return fmt.Errorf("error %s change %s: state conflict (HTTP 409 Conflict).\n\nUnderlying error: %w", actionDesc, changeID, err)
	}

	return fmt.Errorf("error %s change %s: %w", actionDesc, changeID, err)
}

// FormatError translates an error from a Gerrit operation on this change into an actionable error.
func (c *ChangeContext) FormatError(err error, actionDesc string) error {
	if err == nil {
		return nil
	}
	gerritHost := ""
	if c != nil && c.Config != nil {
		if u, uErr := c.Config.GerritURL(c.Context); uErr == nil {
			gerritHost = u
		}
	}
	changeID := ""
	if c != nil {
		changeID = c.ChangeID
	}
	return FormatGerritError(err, actionDesc, changeID, gerritHost)
}

// SetReview applies a review input using the ChangeContext's resolved revision.
func (c *ChangeContext) SetReview(input *gerrit.ReviewInput) error {
	if c == nil {
		return fmt.Errorf("internal error: ChangeContext is nil")
	}
	if err := SetReviewSafe(c.Context, c.Client, c.ChangeID, c.Revision, input); err != nil {
		return c.FormatError(err, "setting review for")
	}
	return nil
}

// SetReviewRevision applies a review input targeting an explicit revision.
func (c *ChangeContext) SetReviewRevision(revision string, input *gerrit.ReviewInput) error {
	if c == nil {
		return fmt.Errorf("internal error: ChangeContext is nil")
	}
	if err := SetReviewSafe(c.Context, c.Client, c.ChangeID, revision, input); err != nil {
		return c.FormatError(err, "setting review for")
	}
	return nil
}

// ExtractRevision extracts the requested revision from a change.
func (c *ChangeContext) ExtractRevision(change *gerrit.ChangeInfo) (gerrit.RevisionInfo, error) {
	if c == nil {
		return gerrit.RevisionInfo{}, fmt.Errorf("internal error: ChangeContext is nil")
	}
	return ExtractRevision(change, c.Revision, c.ChangeID)
}

// ExtractFetchRef extracts the git fetch ref from a change and revision.
func (c *ChangeContext) ExtractFetchRef(change *gerrit.ChangeInfo, rev gerrit.RevisionInfo) (string, error) {
	if c == nil {
		return "", fmt.Errorf("internal error: ChangeContext is nil")
	}
	return ExtractFetchRef(change, rev, c.ChangeID)
}

// SetReviewSafe invokes client.Changes.SetReview while safely ignoring the known
// go-gerrit unmarshaling bug where the "labels" field in ReviewResult fails type
// assertion on certain Gerrit versions. All genuine API/network errors are preserved.
func SetReviewSafe(ctx context.Context, client *gerrit.Client, changeID, revision string, input *gerrit.ReviewInput) error {
	if ctx == nil {
		return fmt.Errorf("internal error: context is nil")
	}
	if client == nil {
		return fmt.Errorf("internal error: gerrit client is nil")
	}
	if changeID == "" {
		return fmt.Errorf("internal error: changeID is empty")
	}
	if input == nil {
		return fmt.Errorf("internal error: review input is nil")
	}
	if revision == "" {
		revision = "current"
	}
	_, _, err := client.Changes.SetReview(ctx, changeID, revision, input)
	if err != nil {
		var unmarshalErr *json.UnmarshalTypeError
		if errors.As(err, &unmarshalErr) && strings.Contains(unmarshalErr.Field, "labels") {
			// Ignore labels unmarshaling error as we don't use the result.
			return nil
		}
		return err
	}
	return nil
}

// ExtractRevision finds the requested revision in a ChangeInfo.
// If reqRev is empty or "current", it returns the CurrentRevision.
// Otherwise it matches by patchset number (e.g. "3") or revision ID/hash prefix.
func ExtractRevision(change *gerrit.ChangeInfo, reqRev string, changeID ...string) (gerrit.RevisionInfo, error) {
	if change == nil {
		return gerrit.RevisionInfo{}, fmt.Errorf("internal error: change is nil")
	}
	idStr := ""
	if len(changeID) > 0 && changeID[0] != "" {
		idStr = changeID[0]
	} else {
		idStr = changeIdentifier(change)
	}

	if reqRev != "" && reqRev != "current" {
		targetNum, parseErr := strconv.Atoi(reqRev)
		for _, rev := range change.Revisions {
			if parseErr == nil && rev.Number == targetNum {
				return rev, nil
			}
		}
		if rev, ok := change.Revisions[reqRev]; ok {
			return rev, nil
		}
		if len(reqRev) >= 7 {
			for hash, rev := range change.Revisions {
				if strings.HasPrefix(hash, reqRev) {
					return rev, nil
				}
			}
		}
		available := availablePatchsets(change)
		return gerrit.RevisionInfo{}, fmt.Errorf("revision %s not found for change %s (available patchsets: %s)", reqRev, idStr, available)
	}

	if change.CurrentRevision == "" {
		return gerrit.RevisionInfo{}, fmt.Errorf("current revision not found for change %s", idStr)
	}
	revision, ok := change.Revisions[change.CurrentRevision]
	if !ok {
		return gerrit.RevisionInfo{}, fmt.Errorf("current revision not found for change %s", idStr)
	}
	return revision, nil
}

// ExtractFetchRef extracts the git fetch ref from a change and revision,
// checking protocols in deterministic order ("http", "anonymous http", "sso", "ssh", or sorted)
// and falling back to rev.Ref or standard Gerrit ref formatting "refs/changes/XX/YYYY/ZZ".
func ExtractFetchRef(change *gerrit.ChangeInfo, rev gerrit.RevisionInfo, changeID ...string) (string, error) {
	if change == nil {
		return "", fmt.Errorf("internal error: change is nil")
	}
	var ref string
	if f, ok := rev.Fetch["http"]; ok && f.Ref != "" {
		ref = f.Ref
	} else if f, ok := rev.Fetch["anonymous http"]; ok && f.Ref != "" {
		ref = f.Ref
	} else if f, ok := rev.Fetch["sso"]; ok && f.Ref != "" {
		ref = f.Ref
	} else if f, ok := rev.Fetch["ssh"]; ok && f.Ref != "" {
		ref = f.Ref
	} else if len(rev.Fetch) > 0 {
		keys := make([]string, 0, len(rev.Fetch))
		for k := range rev.Fetch {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			if f := rev.Fetch[k]; f.Ref != "" {
				ref = f.Ref
				break
			}
		}
	}

	if ref == "" && rev.Ref != "" {
		ref = rev.Ref
	}

	if ref == "" && change.Number > 0 && rev.Number > 0 {
		ref = fmt.Sprintf("refs/changes/%02d/%d/%d", change.Number%100, change.Number, rev.Number)
	}

	if ref == "" {
		idStr := ""
		if len(changeID) > 0 && changeID[0] != "" {
			idStr = changeID[0]
		} else {
			idStr = changeIdentifier(change)
		}
		return "", fmt.Errorf("fetch ref not found for change %s", idStr)
	}
	return ref, nil
}

func changeIdentifier(change *gerrit.ChangeInfo) string {
	if change == nil {
		return "unknown"
	}
	if change.Number != 0 {
		return strconv.Itoa(change.Number)
	}
	if change.ChangeID != "" {
		return change.ChangeID
	}
	if change.ID != "" {
		return change.ID
	}
	return "unknown"
}

func availablePatchsets(change *gerrit.ChangeInfo) string {
	if change == nil || len(change.Revisions) == 0 {
		return "none"
	}
	nums := make([]int, 0, len(change.Revisions))
	for _, rev := range change.Revisions {
		if rev.Number > 0 {
			nums = append(nums, rev.Number)
		}
	}
	if len(nums) == 0 {
		return "none"
	}
	sort.Ints(nums)
	var sb strings.Builder
	for i, n := range nums {
		if i > 0 {
			sb.WriteString(", ")
		}
		sb.WriteString(strconv.Itoa(n))
	}
	return sb.String()
}

// ResolveProfile resolves the active ProjectProfile using the following precedence:
// 1. Explicit profile from Config (if set and non-generic).
// 2. Profile detected from the gerrit host and explicit ProfileFlag.
// 3. Fallback to change project name if generic and no explicit ProfileFlag was set.
func ResolveProfile(ctx context.Context, cfg *Config, gerritHost, project string) (ProjectProfile, error) {
	var profile ProjectProfile
	if cfg != nil {
		profile = cfg.GetProfile(ctx)
	}
	if profile == nil || profile.Name() == "generic" {
		p, err := DetectProfile("", gerritHost, ProfileFlag)
		if err != nil {
			return nil, err
		}
		profile = p
		if profile.Name() == "generic" && project != "" && ProfileFlag == "" {
			if p2, err := DetectProfile(project, "", ""); err == nil && p2.Name() != "generic" {
				profile = p2
			}
		}
	}
	return profile, nil
}

// ResolveProfile resolves the active project profile for this change context.
func (c *ChangeContext) ResolveProfile(project ...string) (ProjectProfile, error) {
	if c == nil {
		return DetectProfile("", HostFlag, ProfileFlag)
	}
	var proj string
	if len(project) > 0 {
		proj = project[0]
	}
	var gHost string
	if c.Client != nil {
		u := c.Client.BaseURL()
		gHost = trimHostScheme(u.String())
	}
	if gHost == "" && c.Config != nil {
		if u, err := c.Config.GerritURL(c.Context); err == nil {
			gHost = trimHostScheme(u)
		}
	}
	if gHost == "" {
		gHost = HostFlag
	}
	return ResolveProfile(c.Context, c.Config, gHost, proj)
}
