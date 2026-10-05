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
	"os"
	"strings"

	"github.com/andygrunwald/go-gerrit"
)

// Type aliases unifying CIProvider signatures with existing LUCI and Gerrit types.
type (
	CIBuild          = bbBuild
	bbBuildDetails   = LUCIBuildDetails
	bbStep           = LUCIStep
	bbBuilderID      = bbBuilder
	GerritChangeInfo = gerrit.ChangeInfo
	GerritClient     = gerrit.Client
	ChangeRef        = GerritChangeRef
)

// FailureReportOptions configures failure log extraction across CI providers.
type FailureReportOptions struct {
	MaxLogLines            int
	PreferredLogs          []string
	IncludeSummaryMarkdown bool
	Target                 string
}

// CIProvider abstracts CI check discovery, step/log inspection, failure extraction,
// and check reruns across backends (LUCI Buildbucket and Android Busytown / TreeHugger).
type CIProvider interface {
	Name() string
	SearchBuilds(ctx context.Context, host string, changeNum int, patchsetNum int, change *GerritChangeInfo) ([]bbBuild, error)
	GetBuildDetails(ctx context.Context, buildID string, b *bbBuild) (*bbBuildDetails, error)
	FetchFailureReportWithOptions(ctx context.Context, b bbBuild, opts FailureReportOptions) (*FailureReport, error)
	RerunBuilds(ctx context.Context, ch ChangeRef, builds []bbBuild, dryRun bool, out io.Writer) error
}

// BuildbucketProvider implements CIProvider for LUCI Buildbucket v2 and LogDog.
type BuildbucketProvider struct {
	LUCIClient   *LUCIClient
	GerritClient *GerritClient
	Cfg          *Config
	Profile      ProjectProfile
	ErrOut       io.Writer
}

func (p *BuildbucketProvider) Name() string {
	return "buildbucket"
}

func (p *BuildbucketProvider) luciClient(ctx context.Context) *LUCIClient {
	if p.LUCIClient != nil {
		return p.LUCIClient
	}
	return NewLUCIClient(buildbucketHost, getLUCIHTTPClient(ctx, buildbucketHost))
}

func stampBuildbucketBuilds(builds []bbBuild) {
	for i := range builds {
		if builds[i].Provider == "" {
			builds[i].Provider = "buildbucket"
		}
		if builds[i].ViewURL == "" && builds[i].ID != "" {
			builds[i].ViewURL = fmt.Sprintf("https://ci.chromium.org/b/%s", builds[i].ID)
		}
	}
}

func (p *BuildbucketProvider) SearchBuilds(ctx context.Context, host string, changeNum int, patchsetNum int, change *GerritChangeInfo) ([]bbBuild, error) {
	project := ""
	if change != nil {
		project = change.Project
	}
	cleanHost := CleanGerritHost(host)
	builds, err := p.luciClient(ctx).SearchBuilds(ctx, cleanHost, project, changeNum, patchsetNum)
	if err != nil {
		return nil, err
	}
	stampBuildbucketBuilds(builds)
	return builds, nil
}

// SearchBuildsByPatchset queries Buildbucket concurrently across all patchsets in patchsets.
func (p *BuildbucketProvider) SearchBuildsByPatchset(ctx context.Context, host string, changeNum int, patchsets []int, change *GerritChangeInfo) ([][]bbBuild, error) {
	project := ""
	if change != nil {
		project = change.Project
	}
	cleanHost := CleanGerritHost(host)
	perPS, err := p.luciClient(ctx).SearchBuildsByPatchset(ctx, cleanHost, project, changeNum, patchsets)
	if err != nil {
		return nil, err
	}
	for i := range perPS {
		stampBuildbucketBuilds(perPS[i])
	}
	return perPS, nil
}

func (p *BuildbucketProvider) GetBuildDetails(ctx context.Context, buildID string, b *bbBuild) (*bbBuildDetails, error) {
	details, err := p.luciClient(ctx).GetBuildDetails(ctx, buildID)
	if err != nil {
		return nil, err
	}
	if details != nil && details.Provider == "" {
		details.Provider = "buildbucket"
	}
	return details, nil
}

func (p *BuildbucketProvider) FetchFailureReportWithOptions(ctx context.Context, b bbBuild, opts FailureReportOptions) (*FailureReport, error) {
	maxLines := opts.MaxLogLines
	if maxLines <= 0 {
		maxLines = 40
	}
	details, err := p.GetBuildDetails(ctx, b.ID, &b)
	if err != nil {
		return nil, err
	}
	report := p.luciClient(ctx).ExtractFailureReportWithOptions(ctx, details, maxLines, opts.PreferredLogs, opts.IncludeSummaryMarkdown)
	return report, nil
}

func (p *BuildbucketProvider) RerunBuilds(ctx context.Context, ch ChangeRef, builds []bbBuild, dryRun bool, out io.Writer) error {
	if out == nil {
		out = os.Stdout
	}
	errOut := p.ErrOut
	if errOut == nil {
		errOut = os.Stderr
	}
	prof := p.Profile
	if prof == nil {
		prof, _ = DetectProfile("", ch.Host, ProfileFlag)
	}

	for _, b := range builds {
		bName := b.Builder.Builder
		rerunSpec := bName
		if b.Builder.Project != "" && b.Builder.Bucket != "" {
			rerunSpec = fmt.Sprintf("%s/%s/%s", b.Builder.Project, b.Builder.Bucket, bName)
		}
		if dryRun {
			cmdStr := prof.FormatRerunCommand(ch, rerunSpec)
			fmt.Fprintf(out, "[dry-run] %s\n", cmdStr)
			continue
		}

		fmt.Fprintf(out, "Rerunning check: %s...\n", bName)
		if err := prof.RerunCheck(ctx, ch, rerunSpec, out, errOut); err != nil {
			return fmt.Errorf("failed to rerun %s: %w", bName, err)
		}
	}
	return nil
}

// CompositeCIProvider fans out CI operations across multiple configured CIProviders
// (for example, repositories that run both LUCI Buildbucket and Android Busytown checks).
type CompositeCIProvider struct {
	Providers []CIProvider
	AutoMode  bool
	ErrOut    io.Writer
}

func (c *CompositeCIProvider) Name() string {
	return "composite"
}

type providerFailure struct {
	name string
	err  error
}

func (c *CompositeCIProvider) SearchBuilds(ctx context.Context, host string, changeNum int, patchsetNum int, change *GerritChangeInfo) ([]bbBuild, error) {
	var all []bbBuild
	var firstErr error
	var failedProviders []providerFailure

	for _, p := range c.Providers {
		builds, err := p.SearchBuilds(ctx, host, changeNum, patchsetNum, change)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			failedProviders = append(failedProviders, providerFailure{name: p.Name(), err: err})
			continue
		}
		all = append(all, builds...)
	}

	if len(all) == 0 && firstErr != nil {
		return nil, firstErr
	}
	if len(all) > 0 && c.ErrOut != nil {
		for _, pf := range failedProviders {
			fmt.Fprintf(c.ErrOut, "Warning: CI provider %s failed: %v\n", pf.name, pf.err)
		}
	}
	return all, nil
}

// SearchBuildsByPatchset queries all configured providers across patchsets and merges per-patchset builds.
func (c *CompositeCIProvider) SearchBuildsByPatchset(ctx context.Context, host string, changeNum int, patchsets []int, change *GerritChangeInfo) ([][]bbBuild, error) {
	if len(patchsets) == 0 {
		return nil, fmt.Errorf("patchsets cannot be empty")
	}
	results := make([][]bbBuild, len(patchsets))
	totalBuilds := 0
	var firstErr error
	var failedProviders []providerFailure

	for _, p := range c.Providers {
		subResults, err := SearchProviderBuildsByPatchset(ctx, p, host, changeNum, patchsets, change)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			failedProviders = append(failedProviders, providerFailure{name: p.Name(), err: err})
			continue
		}
		for i := range patchsets {
			if i < len(subResults) {
				results[i] = append(results[i], subResults[i]...)
				totalBuilds += len(subResults[i])
			}
		}
	}

	if totalBuilds == 0 && firstErr != nil {
		return nil, firstErr
	}
	if totalBuilds > 0 && c.ErrOut != nil {
		for _, pf := range failedProviders {
			fmt.Fprintf(c.ErrOut, "Warning: CI provider %s failed: %v\n", pf.name, pf.err)
		}
	}
	return results, nil
}

// SearchProviderBuildsByPatchset queries a CIProvider for builds on each of the given patchsets.
func SearchProviderBuildsByPatchset(ctx context.Context, p CIProvider, host string, changeNum int, patchsets []int, change *GerritChangeInfo) ([][]bbBuild, error) {
	if p == nil {
		return nil, fmt.Errorf("CIProvider is nil")
	}
	if len(patchsets) == 0 {
		return nil, fmt.Errorf("patchsets cannot be empty")
	}
	switch v := p.(type) {
	case *BuildbucketProvider:
		return v.SearchBuildsByPatchset(ctx, host, changeNum, patchsets, change)
	case *CompositeCIProvider:
		return v.SearchBuildsByPatchset(ctx, host, changeNum, patchsets, change)
	default:
		results := make([][]bbBuild, len(patchsets))
		for i, ps := range patchsets {
			builds, err := p.SearchBuilds(ctx, host, changeNum, ps, change)
			if err != nil {
				return nil, err
			}
			for j := range builds {
				if builds[j].Patchset == 0 {
					builds[j].Patchset = ps
				}
			}
			results[i] = builds
		}
		return results, nil
	}
}

// SearchProviderBuildsForPatchsets queries a CIProvider across one or more equivalent patchsets
// (ordered newest first) and returns the concatenated builds in that order.
func SearchProviderBuildsForPatchsets(ctx context.Context, p CIProvider, host string, changeNum int, patchsets []int, change *GerritChangeInfo) ([]bbBuild, error) {
	perPS, err := SearchProviderBuildsByPatchset(ctx, p, host, changeNum, patchsets, change)
	if err != nil {
		return nil, err
	}
	var all []bbBuild
	for _, builds := range perPS {
		all = append(all, builds...)
	}
	return all, nil
}

func (c *CompositeCIProvider) providerForBuild(buildID string, b *bbBuild) CIProvider {
	if b != nil {
		if b.IsBusytown() {
			for _, p := range c.Providers {
				if p.Name() == "busytown" {
					return p
				}
			}
		}
		if strings.EqualFold(b.Provider, "buildbucket") {
			for _, p := range c.Providers {
				if p.Name() == "buildbucket" {
					return p
				}
			}
		}
	}
	if IsBusytownBuildID(buildID) {
		for _, p := range c.Providers {
			if p.Name() == "busytown" {
				return p
			}
		}
	}
	for _, p := range c.Providers {
		if p.Name() == "buildbucket" {
			return p
		}
	}
	if len(c.Providers) > 0 {
		return c.Providers[0]
	}
	return nil
}

func (c *CompositeCIProvider) GetBuildDetails(ctx context.Context, buildID string, b *bbBuild) (*bbBuildDetails, error) {
	primary := c.providerForBuild(buildID, b)
	if primary != nil {
		details, err := primary.GetBuildDetails(ctx, buildID, b)
		if err == nil {
			return details, nil
		}
		for _, p := range c.Providers {
			if p == primary {
				continue
			}
			if d, fallbackErr := p.GetBuildDetails(ctx, buildID, b); fallbackErr == nil {
				return d, nil
			}
		}
		return nil, err
	}
	return nil, fmt.Errorf("no CI provider configured for build %q", buildID)
}

func (c *CompositeCIProvider) FetchFailureReportWithOptions(ctx context.Context, b bbBuild, opts FailureReportOptions) (*FailureReport, error) {
	p := c.providerForBuild(b.ID, &b)
	if p == nil {
		return nil, fmt.Errorf("no CI provider configured for build %q", b.ID)
	}
	return p.FetchFailureReportWithOptions(ctx, b, opts)
}

func (c *CompositeCIProvider) RerunBuilds(ctx context.Context, ch ChangeRef, builds []bbBuild, dryRun bool, out io.Writer) error {
	var bbBuilds []bbBuild
	var btBuilds []bbBuild
	for _, b := range builds {
		if b.IsBusytown() {
			btBuilds = append(btBuilds, b)
		} else {
			bbBuilds = append(bbBuilds, b)
		}
	}

	for _, p := range c.Providers {
		switch p.Name() {
		case "buildbucket":
			if len(bbBuilds) > 0 {
				if err := p.RerunBuilds(ctx, ch, bbBuilds, dryRun, out); err != nil {
					return err
				}
			}
		case "busytown":
			if len(btBuilds) > 0 {
				if err := p.RerunBuilds(ctx, ch, btBuilds, dryRun, out); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

func isBusytownCandidate(host string, change *GerritChangeInfo) bool {
	lowerHost := strings.ToLower(host)
	if strings.Contains(lowerHost, "android") {
		return true
	}
	if change != nil {
		for labelName := range change.Labels {
			if strings.EqualFold(labelName, "Presubmit-Ready") ||
				strings.EqualFold(labelName, "Presubmit-Verified") ||
				strings.EqualFold(labelName, "Autosubmit") {
				return true
			}
		}
		for _, msg := range change.Messages {
			if strings.HasPrefix(msg.Tag, "autogenerated:TreeHugger") ||
				strings.Contains(msg.Message, "TreeHugger") ||
				strings.Contains(msg.Message, "ci.android.com") {
				return true
			}
		}
	}
	return false
}

// ResolveCIProvider selects and constructs the active CIProvider based on
// `.ghish.toml` `[ci] providers` configuration or auto-detection from the Gerrit host/change.
func ResolveCIProvider(cfg *Config, pcfg *ProjectConfig, gerritClient *GerritClient, host string, change *GerritChangeInfo) CIProvider {
	bbProv := &BuildbucketProvider{
		GerritClient: gerritClient,
		Cfg:          cfg,
	}
	btProv := &BusytownProvider{
		GerritClient: gerritClient,
		Cfg:          cfg,
	}

	var configured []string
	if pcfg != nil {
		configured = pcfg.CI.Providers
	}

	wantAuto := len(configured) == 0
	wantBB := false
	wantBT := false
	for _, raw := range configured {
		switch strings.ToLower(strings.TrimSpace(raw)) {
		case "", "auto":
			wantAuto = true
		case "buildbucket", "luci":
			wantBB = true
		case "busytown", "android-build", "android_build", "treehugger", "treetop":
			wantBT = true
		}
	}

	if wantBB && wantBT {
		return &CompositeCIProvider{
			Providers: []CIProvider{bbProv, btProv},
			AutoMode:  false,
		}
	}
	if wantBT && !wantBB && !wantAuto {
		return btProv
	}
	if wantBB && !wantBT && !wantAuto {
		return bbProv
	}

	if isBusytownCandidate(host, change) {
		return &CompositeCIProvider{
			Providers: []CIProvider{bbProv, btProv},
			AutoMode:  true,
		}
	}
	return bbProv
}
