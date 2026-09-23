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
	"fmt"
	"net/url"
	"regexp"
	"sort"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

// autoSubmitLabelPattern matches the label a Gerrit host uses to request
// automated submission once review and checks pass.
//
// The name is not standardized. Pigweed calls it Pigweed-Auto-Submit, Fuchsia
// calls it Fuchsia-Auto-Submit, Chromium and chrome-internal call it
// Auto-Submit, and some hosts spell it Autosubmit. Recognizing the shape of
// the name instead of enumerating hosts means a host nobody taught pw_ghish
// about still works, and a host that renames its label stops being voted on
// under a name it no longer has.
//
// Only the separators a label name actually uses are allowed between the two
// words. Matching any character there would let an unrelated label such as
// "autoXsubmit" be voted as though it submitted the change.
var autoSubmitLabelPattern = regexp.MustCompile(`(?i)auto[-_ ]?submit`)

// commitQueueLabelPattern matches a Commit Queue label. Unlike the auto-submit
// label the name is near-universal, but the separator varies (Commit-Queue,
// CommitQueue), so it is matched by shape for the same reasons.
//
// This is anchored where autoSubmitLabelPattern is not: "Commit-Queue" is a
// whole label name, whereas auto-submit labels are routinely prefixed with the
// project ("Pigweed-Auto-Submit").
var commitQueueLabelPattern = regexp.MustCompile(`(?i)^commit[-_ ]?queue$`)

// AutoSubmitDecision is what --auto should do against a particular host.
type AutoSubmitDecision struct {
	// Vote is the label to cast. The zero value means there is nothing to cast.
	Vote LabelVote

	// Unsupported, when non-nil, reports that the host cannot auto-submit.
	//
	// Vote may be set even when this is non-nil: a host with a Commit Queue but
	// no auto-submit label can still verify the change, so a dry run is
	// requested and this is reported afterwards. Callers must cast Vote first
	// and return this second, so that the part of the request that can be
	// honored is honored, and the user still hears that nothing is going to
	// submit their change.
	Unsupported error
}

// DecideAutoSubmit decides what --auto should do, given the labels a host
// reports. subject names what those labels belong to ("change 12345",
// "project foo/bar") and appears in diagnostics.
//
// Every outcome other than "found it" is a failure of some kind, because
// --auto is a request for the change to be submitted and pw_ghish either
// arranges that or does not:
//
//   - one auto-submit label: vote it.
//   - several: an error. They are not interchangeable.
//   - none, but the host has a Commit Queue: a dry-run vote plus
//     Unsupported, so the change is still verified while the user is told
//     that nothing will submit it.
//   - none at all: an error, cast before anything is done.
func DecideAutoSubmit(labels map[string]gerrit.LabelInfo, subject string) (AutoSubmitDecision, error) {
	if len(labels) == 0 {
		return AutoSubmitDecision{}, fmt.Errorf("--auto: %s reports no labels, so pw_ghish cannot tell which one requests automatic submission.\n\n"+
			"To vote a label anyway, name it:\n"+
			"  %s pr edit <id> --add-label <Label>=<Score>    (existing change)\n"+
			"  %s pr create -o l=<Label>+1                    (new change)",
			subject, RootCmd.CommandPath(), RootCmd.CommandPath())
	}

	vote, ok, err := FindAutoSubmitLabel(labels)
	if err != nil {
		return AutoSubmitDecision{}, fmt.Errorf("cannot auto-submit %s: %w", subject, err)
	}
	if ok {
		return AutoSubmitDecision{Vote: vote}, nil
	}

	dryRun, hasCQ := commitQueueDryRunVote(labels)
	unsupported := noAutoSubmitError(subject, labels, dryRun, hasCQ)
	if !hasCQ {
		// There is nothing useful to do on this host, so refuse up front
		// rather than acting and then complaining.
		return AutoSubmitDecision{}, unsupported
	}
	return AutoSubmitDecision{Vote: dryRun, Unsupported: unsupported}, nil
}

// FindAutoSubmitLabel picks the auto-submit label out of the labels a host
// reports, along with the vote that requests submission.
//
// labels comes from ChangeInfo.Labels or a project's label list, both of which
// enumerate every label that applies -- so a label missing from it is a label
// the host will reject a vote on.
//
// ok is false when nothing matches. More than one match is an error rather
// than a pick: the labels are not interchangeable, and choosing wrong votes a
// label that does not submit anything.
func FindAutoSubmitLabel(labels map[string]gerrit.LabelInfo) (LabelVote, bool, error) {
	matches := matchingLabels(labels, autoSubmitLabelPattern)
	if len(matches) == 0 {
		return LabelVote{}, false, nil
	}
	if len(matches) > 1 {
		return LabelVote{}, false, ambiguousAutoSubmitError(matches)
	}
	name := matches[0]
	return LabelVote{Name: name, Value: autoSubmitVote(labels[name])}, true, nil
}

// matchingLabels returns the sorted names of the labels matching pattern.
func matchingLabels(labels map[string]gerrit.LabelInfo, pattern *regexp.Regexp) []string {
	var matches []string
	for name := range labels {
		if pattern.MatchString(name) {
			matches = append(matches, name)
		}
	}
	sort.Strings(matches)
	return matches
}

// ambiguousAutoSubmitError reports labels that all look like auto-submit.
//
// Guessing here fails quietly in the worst way: the push or vote succeeds, the
// user believes the change is queued to submit, and it sits untouched because
// the label that actually gates submission was never voted. So the ambiguity
// is handed back to the caller, who knows which label they meant.
func ambiguousAutoSubmitError(matches []string) error {
	root := RootCmd.CommandPath()
	return fmt.Errorf("found %d labels that look like auto-submit labels: %s\n\n"+
		"These are not interchangeable, and voting the wrong one would leave the change\n"+
		"looking handed off while nothing submits it, so pw_ghish will not choose.\n\n"+
		"Name the label you want instead of using --auto:\n"+
		"  %s pr edit <id> --add-label <Label>=<Score>    (existing change)\n"+
		"  %s pr create -o l=<Label>+1                    (new change)",
		len(matches), strings.Join(matches, ", "), root, root)
}

// noAutoSubmitError reports that a host has no auto-submit label, and so
// cannot honor --auto.
//
// This is an error and not a warning because --auto is a request for the
// change to end up submitted. Quietly doing something adjacent -- voting the
// Commit Queue and calling it auto-submit, as pw_ghish used to -- means the
// user walks away believing the change is handed off. If it is not, they find
// out days later.
func noAutoSubmitError(subject string, labels map[string]gerrit.LabelInfo, dryRun LabelVote, hasCQ bool) error {
	root := RootCmd.CommandPath()
	if hasCQ {
		return fmt.Errorf("--auto: %s has no auto-submit label, so nothing will submit this change once it passes.\n\n"+
			"%s%+d was requested instead, which runs the presubmits but does not submit.\n\n"+
			"Labels here: %s\n\n"+
			"To submit as soon as checks pass, use --cq instead of --auto.\n"+
			"To vote a different label, name it:\n"+
			"  %s pr edit <id> --add-label <Label>=<Score>    (existing change)\n"+
			"  %s pr create -o l=<Label>+1                    (new change)",
			subject, dryRun.Name, dryRun.Value, FormatLabelNames(labels), root, root)
	}
	return fmt.Errorf("--auto: %s has no auto-submit label and no Commit Queue, so pw_ghish cannot arrange for it to be submitted automatically.\n\n"+
		"Labels here: %s\n\n"+
		"To submit it yourself once it is ready:\n"+
		"  %s pr merge <id>\n"+
		"To vote a label this host does have:\n"+
		"  %s pr edit <id> --add-label <Label>=<Score>    (existing change)\n"+
		"  %s pr create -o l=<Label>+1                    (new change)",
		subject, FormatLabelNames(labels), root, root, root)
}

// autoSubmitVote returns the score that asks for auto-submission: the highest
// value the label permits, because "submit this when it is ready" is the
// label's maximum and nothing weaker means it.
//
// The permitted range only arrives with DETAILED_LABELS. Without it the vote
// falls back to +1, which is the maximum of every auto-submit label in use
// (they are all [0, +1] or [-1, +1] toggles).
func autoSubmitVote(info gerrit.LabelInfo) int {
	if _, high, ok := labelRange(info); ok && high > 0 {
		return high
	}
	return 1
}

// commitQueueDryRunVote returns a Commit-Queue+1 vote if the host has a Commit
// Queue to run.
//
// +1 is the dry run: it verifies the change without submitting it. The higher
// vote would submit, and a host that was never asked to auto-submit anything
// should not have its changes submitted because pw_ghish could not find the
// label the user actually wanted.
//
// ok is false when the host has no Commit Queue, when it has more than one
// label that could be one, or when the label does not permit +1. In each case
// there is no vote that is obviously the right one, and the caller reports the
// missing auto-submit label on its own.
func commitQueueDryRunVote(labels map[string]gerrit.LabelInfo) (LabelVote, bool) {
	matches := matchingLabels(labels, commitQueueLabelPattern)
	if len(matches) != 1 {
		return LabelVote{}, false
	}
	name := matches[0]
	if low, high, ok := labelRange(labels[name]); ok && (1 < low || 1 > high) {
		return LabelVote{}, false
	}
	return LabelVote{Name: name, Value: 1}, true
}

// FormatLabelNames renders a host's label names for diagnostics, so an error
// about a missing label can show what the host does offer instead.
func FormatLabelNames(labels map[string]gerrit.LabelInfo) string {
	if len(labels) == 0 {
		return "none reported"
	}
	names := make([]string, 0, len(labels))
	for name := range labels {
		names = append(names, name)
	}
	sort.Strings(names)
	return strings.Join(names, ", ")
}

// DecideAutoSubmit resolves what --auto should do to this change, by reading
// the labels the change itself reports.
//
// Every command that offers --auto goes through here, so they all vote the
// same label and fail the same way rather than each one hardcoding a name.
func (c *ChangeContext) DecideAutoSubmit() (AutoSubmitDecision, error) {
	if c == nil {
		return AutoSubmitDecision{}, fmt.Errorf("internal error: ChangeContext is nil")
	}
	change, err := c.GetChange(&gerrit.ChangeOptions{
		AdditionalFields: []string{"DETAILED_LABELS"},
	})
	if err != nil {
		return AutoSubmitDecision{}, err
	}
	return DecideAutoSubmit(change.Labels, fmt.Sprintf("change %s", c.ChangeID))
}

// LabelDefinition is one entry of a project's label list, as returned by
// Gerrit's "GET /projects/{project}/labels/" endpoint. Only the fields needed
// to identify a label and its range are decoded.
type LabelDefinition struct {
	Name   string            `json:"name"`
	Values map[string]string `json:"values"`
}

// ListProjectLabels returns the labels a project defines.
//
// This is how a label name can be learned before any change exists, which is
// the situation `gh pr create --auto` is in: there is no ChangeInfo to read,
// but the project already knows what its labels are called.
func ListProjectLabels(ctx context.Context, client *gerrit.Client, project string) ([]LabelDefinition, error) {
	if client == nil {
		return nil, fmt.Errorf("internal error: gerrit client is nil")
	}
	if project == "" {
		return nil, fmt.Errorf("internal error: project is empty")
	}
	var labels []LabelDefinition
	// The project name contains slashes ("pigweed/pigweed") and has to be
	// escaped, or Gerrit reads the rest of the name as more path segments.
	path := fmt.Sprintf("projects/%s/labels/", url.PathEscape(project))
	if _, err := client.Call(ctx, "GET", path, nil, &labels); err != nil {
		return nil, err
	}
	return labels, nil
}

// labelInfoFromDefinitions adapts project label definitions to the same shape
// a change reports, so both sources go through one matcher and one idea of
// what score to cast.
func labelInfoFromDefinitions(defs []LabelDefinition) map[string]gerrit.LabelInfo {
	labels := make(map[string]gerrit.LabelInfo, len(defs))
	for _, def := range defs {
		if def.Name == "" {
			continue
		}
		labels[def.Name] = gerrit.LabelInfo{Values: def.Values}
	}
	return labels
}

// DecideAutoSubmitForNewChange resolves what --auto should do to a change that
// does not exist yet, by asking the project which labels it defines.
func DecideAutoSubmitForNewChange(ctx context.Context, client *gerrit.Client, project string) (AutoSubmitDecision, error) {
	defs, err := ListProjectLabels(ctx, client, project)
	if err != nil {
		root := RootCmd.CommandPath()
		return AutoSubmitDecision{}, fmt.Errorf("--auto: cannot read the labels of project %s, so pw_ghish cannot tell which one requests automatic submission: %w\n\n"+
			"To push and vote a label you name yourself:\n"+
			"  %s pr create -o l=<Label>+1\n"+
			"To look up what a sibling change uses:\n"+
			"  %s pr view <id> --json labels\n"+
			"To push without requesting automatic submission, drop --auto.",
			project, err, root, root)
	}
	return DecideAutoSubmit(labelInfoFromDefinitions(defs), fmt.Sprintf("project %s", project))
}

// decideProjectAutoSubmit resolves what --auto should do to a change that is
// about to be created from the current checkout.
func decideProjectAutoSubmit(ctx context.Context, cmd *cobra.Command, cfg *Config) (AutoSubmitDecision, error) {
	project, err := cfg.GerritProject(ctx)
	if err != nil {
		return AutoSubmitDecision{}, fmt.Errorf("--auto: cannot tell which project this change is going to, and so cannot tell which label requests automatic submission.\n\n%w", err)
	}

	client, err := NewGerritClient(ctx, cmd)
	if err != nil {
		return AutoSubmitDecision{}, fmt.Errorf("--auto: cannot reach Gerrit to read the labels of project %s: %w", project, err)
	}

	return DecideAutoSubmitForNewChange(ctx, client, project)
}

// GerritProjectFromRemote extracts the Gerrit project (repository) name from a
// git remote URL. The project is the URL path, which is what Gerrit's
// project-scoped REST endpoints are keyed on:
//
//	https://pigweed.googlesource.com/pigweed/pigweed -> pigweed/pigweed
//	sso://chrome-internal/infradata/config           -> infradata/config
//	git@host:team/repo.git                           -> team/repo
//
// Returns "" when the remote has no recognizable path.
func GerritProjectFromRemote(remoteURL string) string {
	raw := strings.TrimSpace(remoteURL)
	raw = strings.TrimSuffix(raw, "/")
	raw = strings.TrimSuffix(raw, ".git")
	if raw == "" {
		return ""
	}

	var path string
	if parsed, err := url.Parse(raw); err == nil && parsed.Host != "" {
		path = parsed.Path
	} else if _, after, found := strings.Cut(raw, ":"); found {
		// scp-style remote, e.g. git@host:team/repo.
		path = after
	} else {
		return ""
	}

	path = strings.Trim(path, "/")
	// Gerrit serves authenticated requests under /a/; that prefix belongs to
	// the endpoint, not to the project name.
	if path == "a" {
		return ""
	}
	path = strings.TrimPrefix(path, "a/")
	return path
}

// GerritProject returns the Gerrit project name for the current checkout.
func (c *Config) GerritProject(ctx context.Context) (string, error) {
	if c == nil || c.Git == nil {
		return "", fmt.Errorf("internal error: git runner is not initialized")
	}
	urlStr, err := c.GitClient().ConfigGet(ctx, "remote.origin.url")
	if err != nil || strings.TrimSpace(urlStr) == "" {
		return "", fmt.Errorf("could not determine the Gerrit project: 'remote.origin.url' is not configured in git config.\n\n" +
			"To set it:\n" +
			"  git remote add origin https://<host>.googlesource.com/<project>")
	}
	project := GerritProjectFromRemote(urlStr)
	if project == "" {
		return "", fmt.Errorf("could not determine the Gerrit project from remote.origin.url %q: the URL has no project path.\n\n"+
			"Expected a remote such as:\n"+
			"  https://<host>.googlesource.com/<project>\n"+
			"  sso://<host>/<project>", strings.TrimSpace(urlStr))
	}
	return project, nil
}
