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
	"io"
	"os/exec"
	"strings"
	"sync"
)

// GerritChangeRef represents reference coordinates for a Gerrit change.
type GerritChangeRef struct {
	Host     string
	Project  string
	ChangeID int
	Patchset int
}

// LabelVote represents a Gerrit label and score (e.g. "Code-Review", 2).
type LabelVote struct {
	Name  string
	Value int
}

// PushOptions represents options when creating or pushing a patchset to Gerrit.
type PushOptions struct {
	Reviewers  []string
	CC         []string
	Draft      bool
	AutoSubmit bool
	// AutoSubmitLabel names the label to vote when AutoSubmit is set. It is
	// resolved from the labels the host reports (the change's, or the
	// project's for a change that does not exist yet) before the push runs.
	// There is no compiled-in default: a host that names no auto-submit label
	// cannot auto-submit, and is reported rather than guessed at.
	AutoSubmitLabel LabelVote
	// AutoSubmitUnsupported is non-nil when AutoSubmitLabel is a Commit-Queue
	// dry run settled for because the host has no auto-submit label.
	AutoSubmitUnsupported error
	Wip                   bool
	Ready                 bool
	CQ                    int
	Publish               bool
	Topic                 string
	Hashtags              []string
	ExtraOptions          []string
}

// ProjectProfile defines project-specific Gerrit and CI conventions.
type ProjectProfile interface {
	// Name returns the unique identifier for this profile (e.g., "pigweed", "fuchsia", "generic").
	Name() string

	// DefaultGerritHost returns the default review host (e.g., "https://pigweed-review.googlesource.com/a").
	DefaultGerritHost() string

	// CQLabel returns the commit-queue label for the project (e.g., "Commit-Queue", 2).
	//
	// There is deliberately no AutoSubmitLabel counterpart: auto-submit labels
	// are named inconsistently across hosts, so the name is read from the host
	// (see DecideAutoSubmit) rather than compiled in per profile.
	CQLabel() (LabelVote, bool)

	// ReviewLabel returns the approval label for human code review (e.g., "Code-Review", 2).
	ReviewLabel() LabelVote

	// BuildbucketProject returns the LUCI project name for CI/checks (e.g., "pigweed", "fuchsia").
	BuildbucketProject() string

	// DefaultBuildbucketBucket returns the default try bucket for CI checks (e.g. "pigweed.try").
	DefaultBuildbucketBucket() string

	// FormatRerunCommand returns the project-specific CLI command to rerun a check builder.
	FormatRerunCommand(change GerritChangeRef, builder string) string

	// RerunCheck invokes the local tool (e.g. `bb add`) to rerun a builder.
	RerunCheck(ctx context.Context, change GerritChangeRef, builder string, stdout, stderr io.Writer) error

	// FormatPushRef formats the git push destination ref, including % options.
	FormatPushRef(branch string, opts PushOptions) string

	// IssueTrackerAPIEndpoint returns the Google Issue Tracker REST API base URL.
	IssueTrackerAPIEndpoint() string

	// DefaultComponentID returns the default Buganizer component ID for the project (0 if none).
	DefaultComponentID() int64

	// IssueWebURL returns the canonical web URL for viewing a Buganizer issue.
	IssueWebURL(issueID int64) string
}

func defaultFormatPushRef(branch string, opts PushOptions, extraOptions ...string) string {
	ref := "refs/for/" + branch
	var options []string
	for _, r := range opts.Reviewers {
		options = append(options, "r="+r)
	}
	for _, c := range opts.CC {
		options = append(options, "cc="+c)
	}
	if opts.Draft || opts.Wip {
		options = append(options, "wip")
	}
	if opts.Ready {
		options = append(options, "ready")
	}
	if opts.Publish {
		options = append(options, "publish-comments")
	}
	cqLabelName := "Commit-Queue"
	if opts.CQ > 0 {
		options = append(options, fmt.Sprintf("l=%s+%d", cqLabelName, opts.CQ))
	}
	// The name was resolved from the host before the push; an empty one means
	// there was nothing to vote. --cq is an explicit request for a specific
	// score, so it outranks the dry run --auto settles for on a host with no
	// auto-submit label.
	if opts.AutoSubmit && opts.AutoSubmitLabel.Name != "" &&
		!(opts.CQ > 0 && strings.EqualFold(opts.AutoSubmitLabel.Name, cqLabelName)) {
		options = append(options, fmt.Sprintf("l=%s%+d", opts.AutoSubmitLabel.Name, opts.AutoSubmitLabel.Value))
	}
	if opts.Topic != "" {
		options = append(options, "topic="+opts.Topic)
	}
	for _, h := range opts.Hashtags {
		if h != "" {
			options = append(options, "t="+h)
		}
	}
	options = append(options, extraOptions...)
	options = append(options, opts.ExtraOptions...)
	if len(options) > 0 {
		ref += "%" + strings.Join(options, ",")
	}
	return ref
}

// DefaultFormatRerunCommand formats a standard 'bb add -cl ...' CLI command
// for rerunning a Buildbucket builder. If project is empty, it falls back to change.Project.
func DefaultFormatRerunCommand(project, bucket string, change GerritChangeRef, builder string) string {
	if project == "" {
		project = change.Project
	}
	clURL := fmt.Sprintf("https://%s/c/%s/+/%d/%d", change.Host, change.Project, change.ChangeID, change.Patchset)
	builderTarget := fmt.Sprintf("%s/%s/%s", project, bucket, builder)
	return fmt.Sprintf("bb add -cl %s %s", clURL, builderTarget)
}

// FormatProfileRerunCommand formats the rerun command for a ProjectProfile using
// its configured Buildbucket project and default try bucket.
func FormatProfileRerunCommand(p ProjectProfile, change GerritChangeRef, builder string) string {
	return DefaultFormatRerunCommand(p.BuildbucketProject(), p.DefaultBuildbucketBucket(), change, builder)
}

var (
	lookPathFn = exec.LookPath
	execCmdFn  = func(ctx context.Context, name string, args []string, stdout, stderr io.Writer) error {
		cmd := exec.CommandContext(ctx, name, args...)
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		return cmd.Run()
	}
)

// SetRerunExecForTesting configures custom lookPath and execCommand functions for testing.
// Returns a restore function to reset to the previous behavior (e.g., in t.Cleanup or defer).
func SetRerunExecForTesting(
	lookPath func(string) (string, error),
	execCmd func(context.Context, string, []string, io.Writer, io.Writer) error,
) func() {
	origLookPath := lookPathFn
	origExecCmd := execCmdFn
	lookPathFn = lookPath
	execCmdFn = execCmd
	return func() {
		lookPathFn = origLookPath
		execCmdFn = origExecCmd
	}
}

// DefaultRerunCheck executes the rerun command using the local 'bb' binary on PATH.
// If 'bb' is not found, it returns an actionable error containing the manual command and optional hint.
func DefaultRerunCheck(ctx context.Context, cmdStr string, hint string, stdout, stderr io.Writer) error {
	fields := strings.Fields(cmdStr)
	if len(fields) < 2 {
		return fmt.Errorf("invalid rerun command: %q", cmdStr)
	}

	bbPath, err := lookPathFn("bb")
	if err != nil {
		msg := fmt.Sprintf("'bb' CLI not found on PATH.\nTo run manually:\n  %s", cmdStr)
		if hint != "" {
			msg += "\n" + hint
		}
		return fmt.Errorf("%s", msg)
	}

	args := fields[1:]
	return execCmdFn(ctx, bbPath, args, stdout, stderr)
}

// --- Pigweed Profile ---

type pigweedProfile struct{}

func (p *pigweedProfile) Name() string {
	return "pigweed"
}

func (p *pigweedProfile) DefaultGerritHost() string {
	return "https://pigweed-review.googlesource.com/a"
}

func (p *pigweedProfile) CQLabel() (LabelVote, bool) {
	return LabelVote{Name: "Commit-Queue", Value: 2}, true
}

func (p *pigweedProfile) ReviewLabel() LabelVote {
	return LabelVote{Name: "Code-Review", Value: 2}
}

func (p *pigweedProfile) BuildbucketProject() string {
	return "pigweed"
}

func (p *pigweedProfile) DefaultBuildbucketBucket() string {
	return "pigweed.try"
}

func (p *pigweedProfile) FormatRerunCommand(change GerritChangeRef, builder string) string {
	return FormatProfileRerunCommand(p, change, builder)
}

func (p *pigweedProfile) RerunCheck(ctx context.Context, change GerritChangeRef, builder string, stdout, stderr io.Writer) error {
	cmdStr := p.FormatRerunCommand(change, builder)
	return DefaultRerunCheck(ctx, cmdStr, "(Hint: activate the Pigweed environment via 'source activate.sh')", stdout, stderr)
}

func (p *pigweedProfile) FormatPushRef(branch string, opts PushOptions) string {
	return defaultFormatPushRef(branch, opts)
}

func (p *pigweedProfile) IssueTrackerAPIEndpoint() string {
	return DefaultIssueTrackerEndpoint
}

func (p *pigweedProfile) DefaultComponentID() int64 {
	// 1194524 corresponds to "Public Trackers > Pigweed".
	return 1194524
}

func (p *pigweedProfile) IssueWebURL(issueID int64) string {
	return fmt.Sprintf("https://issues.pigweed.dev/issues/%d", issueID)
}

// --- Fuchsia Profile ---

type fuchsiaProfile struct{}

func (p *fuchsiaProfile) Name() string {
	return "fuchsia"
}

func (p *fuchsiaProfile) DefaultGerritHost() string {
	return "https://fuchsia-review.googlesource.com/a"
}

func (p *fuchsiaProfile) CQLabel() (LabelVote, bool) {
	return LabelVote{Name: "Commit-Queue", Value: 2}, true
}

func (p *fuchsiaProfile) ReviewLabel() LabelVote {
	return LabelVote{Name: "Code-Review", Value: 2}
}

func (p *fuchsiaProfile) BuildbucketProject() string {
	return "fuchsia"
}

func (p *fuchsiaProfile) DefaultBuildbucketBucket() string {
	return "try"
}

func (p *fuchsiaProfile) FormatRerunCommand(change GerritChangeRef, builder string) string {
	return FormatProfileRerunCommand(p, change, builder)
}

func (p *fuchsiaProfile) RerunCheck(ctx context.Context, change GerritChangeRef, builder string, stdout, stderr io.Writer) error {
	cmdStr := p.FormatRerunCommand(change, builder)
	return DefaultRerunCheck(ctx, cmdStr, "", stdout, stderr)
}

func (p *fuchsiaProfile) FormatPushRef(branch string, opts PushOptions) string {
	return defaultFormatPushRef(branch, opts)
}

func (p *fuchsiaProfile) IssueTrackerAPIEndpoint() string {
	return DefaultIssueTrackerEndpoint
}

func (p *fuchsiaProfile) DefaultComponentID() int64 {
	return 1363195
}

func (p *fuchsiaProfile) IssueWebURL(issueID int64) string {
	return fmt.Sprintf("https://issues.fuchsia.dev/issues/%d", issueID)
}

// --- Generic Profile ---

type genericProfile struct{}

func (p *genericProfile) Name() string {
	return "generic"
}

func (p *genericProfile) DefaultGerritHost() string {
	return ""
}

func (p *genericProfile) CQLabel() (LabelVote, bool) {
	return LabelVote{}, false
}

func (p *genericProfile) ReviewLabel() LabelVote {
	return LabelVote{Name: "Code-Review", Value: 2}
}

func (p *genericProfile) BuildbucketProject() string {
	return ""
}

func (p *genericProfile) DefaultBuildbucketBucket() string {
	return "try"
}

func (p *genericProfile) FormatRerunCommand(change GerritChangeRef, builder string) string {
	return FormatProfileRerunCommand(p, change, builder)
}

func (p *genericProfile) RerunCheck(ctx context.Context, change GerritChangeRef, builder string, stdout, stderr io.Writer) error {
	cmdStr := p.FormatRerunCommand(change, builder)
	return DefaultRerunCheck(ctx, cmdStr, "", stdout, stderr)
}

func (p *genericProfile) FormatPushRef(branch string, opts PushOptions) string {
	return defaultFormatPushRef(branch, opts)
}

func (p *genericProfile) IssueTrackerAPIEndpoint() string {
	return DefaultIssueTrackerEndpoint
}

func (p *genericProfile) DefaultComponentID() int64 {
	return 0
}

func (p *genericProfile) IssueWebURL(issueID int64) string {
	return fmt.Sprintf("https://issuetracker.google.com/issues/%d", issueID)
}

// --- Chromium Profile ---
// Note: ChromiumProfile can be implemented for Chromium / WebRTC Gerrit workflows when needed.
// Chromium uses Commit-Queue+1 (dry run), Commit-Queue+2 (commit), and Auto-Submit+1 labels.

// --- Registry & Detection ---

var (
	profileMu sync.RWMutex
	profiles  = map[string]ProjectProfile{
		"pigweed": &pigweedProfile{},
		"fuchsia": &fuchsiaProfile{},
		"generic": &genericProfile{},
	}
)

// RegisterProfile registers or overrides a project profile.
func RegisterProfile(p ProjectProfile) {
	profileMu.Lock()
	defer profileMu.Unlock()
	profiles[strings.ToLower(p.Name())] = p
}

// GetProfile retrieves a registered profile by name.
func GetProfile(name string) (ProjectProfile, bool) {
	profileMu.RLock()
	defer profileMu.RUnlock()
	p, ok := profiles[strings.ToLower(name)]
	return p, ok
}

// DetectProfile detects the active project profile based on explicit flag, git remote, or host.
func DetectProfile(remoteURL, host, explicitProfile string) (ProjectProfile, error) {
	if explicitProfile != "" {
		if p, ok := GetProfile(explicitProfile); ok {
			return p, nil
		}
		return nil, fmt.Errorf("unknown profile: %q", explicitProfile)
	}

	lowerRemote := strings.ToLower(remoteURL)
	lowerHost := strings.ToLower(host)

	combined := lowerRemote + " " + lowerHost

	if strings.Contains(combined, "pigweed") {
		if p, ok := GetProfile("pigweed"); ok {
			return p, nil
		}
		return nil, fmt.Errorf("profile %q not registered", "pigweed")
	}
	if strings.Contains(combined, "fuchsia") || strings.Contains(combined, "turquoise") {
		if p, ok := GetProfile("fuchsia"); ok {
			return p, nil
		}
		return nil, fmt.Errorf("profile %q not registered", "fuchsia")
	}

	if p, ok := GetProfile("generic"); ok {
		return p, nil
	}
	return nil, fmt.Errorf("default profile %q not registered", "generic")
}
