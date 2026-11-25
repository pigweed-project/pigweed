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
	"cmp"
	"context"
	"errors"
	"fmt"
	"html"
	"io"
	"log/slog"
	"maps"
	"net/url"
	"path"
	"regexp"
	"slices"
	"strings"
)

const (
	defaultMiloHost               = "luci-milo.appspot.com"
	miloInternalService           = "luci.milo.v1.MiloInternal"
	buildbucketBuildersService    = "buildbucket.v2.Builders"
	miloConsoleSnapshotsPageSize  = 100
	bbListBuildersPageSize        = 1000
	maxLUCIDiscoveryPages         = 50
	maxDescriptionLinksPerBuilder = 16
)

// BuilderDescriptionLink represents an HTTP/HTTPS anchor extracted from a
// builder's Buildbucket description_html metadata.
type BuilderDescriptionLink struct {
	Text string `json:"text"`
	URL  string `json:"url"`
}

// ConsoleSelector represents one normalized positional console filter passed to
// `gh run console`. Project may be empty when `--project` supplies the scope.
type ConsoleSelector struct {
	Raw     string
	Project string
	Console string
}

// ConsoleSelectorPlan holds the LUCI projects to query and the console selectors
// to match against `MiloInternal/QueryConsoleSnapshots` results.
type ConsoleSelectorPlan struct {
	ExplicitProjects []string
	Selectors        []ConsoleSelector
	QueryProjects    []string
}

// DiscoveredConsoleBuilder describes a builder referenced by one or more LUCI
// Milo consoles along with metadata from `buildbucket.v2.Builders/ListBuilders`.
type DiscoveredConsoleBuilder struct {
	Project          string
	Bucket           string
	Builder          string
	Consoles         []string
	Experimental     bool
	DescriptionLinks []BuilderDescriptionLink
}

type miloQueryConsoleSnapshotsRequest struct {
	Predicate miloConsolePredicate `json:"predicate"`
	PageSize  int                  `json:"pageSize,omitempty"`
	PageToken string               `json:"pageToken,omitempty"`
}

type miloConsolePredicate struct {
	Project string `json:"project,omitempty"`
}

type miloQueryConsoleSnapshotsResponse struct {
	Snapshots     []miloConsoleSnapshot `json:"snapshots"`
	NextPageToken string                `json:"nextPageToken"`
}

type miloConsoleSnapshot struct {
	Console          *miloConsole          `json:"console"`
	BuilderSnapshots []miloBuilderSnapshot `json:"builderSnapshots"`
}

type miloConsole struct {
	ID              string                  `json:"id"`
	Name            string                  `json:"name"`
	Builders        []miloConsoleBuilderRef `json:"builders"`
	ExternalProject string                  `json:"externalProject"`
	ExternalID      string                  `json:"externalId"`
}

type miloConsoleBuilderRef struct {
	Name      string     `json:"name"`
	ID        *bbBuilder `json:"id"`
	Category  string     `json:"category"`
	ShortName string     `json:"shortName"`
}

type miloBuilderSnapshot struct {
	Builder *bbBuilder `json:"builder"`
}

type bbListBuildersRequest struct {
	Project   string `json:"project,omitempty"`
	Bucket    string `json:"bucket,omitempty"`
	PageSize  int    `json:"pageSize,omitempty"`
	PageToken string `json:"pageToken,omitempty"`
}

type bbListBuildersResponse struct {
	Builders      []bbBuilderItem `json:"builders"`
	NextPageToken string          `json:"nextPageToken"`
}

type bbBuilderItem struct {
	ID     bbBuilder       `json:"id"`
	Config bbBuilderConfig `json:"config"`
}

type bbBuilderConfig struct {
	Name            string         `json:"name"`
	Experimental    string         `json:"experimental"`
	Experiments     map[string]int `json:"experiments"`
	DescriptionHTML string         `json:"descriptionHtml"`
}

type bbConfigBuilderMeta struct {
	experimental     bool
	descriptionLinks []BuilderDescriptionLink
}

var (
	htmlAnchorRe = regexp.MustCompile(`(?is)<a\s+[^>]*?href\s*=\s*(?:"([^"]*)"|'([^']*)')[^>]*>(.*?)</a>`)
	htmlTagRe    = regexp.MustCompile(`<[^>]+>`)
)

func sanitizeSingleLine(s string) string {
	clean := SanitizeUntrustedText(s)
	clean = strings.ReplaceAll(clean, "\r", " ")
	return strings.ReplaceAll(clean, "\n", " ")
}

func extractDescriptionLinks(descHTML string) []BuilderDescriptionLink {
	if strings.TrimSpace(descHTML) == "" {
		return []BuilderDescriptionLink{}
	}
	matches := htmlAnchorRe.FindAllStringSubmatch(descHTML, -1)
	if len(matches) == 0 {
		return []BuilderDescriptionLink{}
	}
	out := make([]BuilderDescriptionLink, 0, min(len(matches), maxDescriptionLinksPerBuilder))
	seen := make(map[string]bool, len(matches))
	for _, m := range matches {
		rawHref := cmp.Or(m[1], m[2])
		href := sanitizeSingleLine(strings.TrimSpace(html.UnescapeString(rawHref)))
		u, err := url.Parse(href)
		if err != nil || (!strings.EqualFold(u.Scheme, "https") && !strings.EqualFold(u.Scheme, "http")) || u.Host == "" {
			continue
		}
		if seen[href] {
			continue
		}
		seen[href] = true
		text := htmlTagRe.ReplaceAllString(m[3], "")
		text = sanitizeSingleLine(strings.TrimSpace(html.UnescapeString(text)))
		text = strings.Join(strings.Fields(text), " ")
		out = append(out, BuilderDescriptionLink{
			Text: cmp.Or(text, href),
			URL:  href,
		})
		if len(out) >= maxDescriptionLinksPerBuilder {
			break
		}
	}
	return out
}

func selectorsNeedDefaultProject(args []string, flagProjects []string) bool {
	if len(flagProjects) > 0 {
		return false
	}
	if len(args) == 0 {
		return true
	}
	for _, raw := range args {
		if !strings.Contains(strings.TrimSpace(raw), "/") {
			return true
		}
	}
	return false
}

func hasGlobMeta(s string) bool {
	return strings.ContainsAny(s, "*?[")
}

// ParseConsoleSelectors normalizes `--project` flags and positional
// `<console>` or `<project>/<console>` arguments into a query plan.
func ParseConsoleSelectors(flagProjects []string, args []string, defaultProject string) (ConsoleSelectorPlan, error) {
	var explicitProjects []string
	explicitSeen := make(map[string]bool)
	for _, rawProj := range flagProjects {
		proj := strings.TrimSpace(rawProj)
		if proj == "" {
			return ConsoleSelectorPlan{}, fmt.Errorf("invalid --project flag: project name cannot be empty")
		}
		if strings.Contains(proj, "/") || hasGlobMeta(proj) {
			return ConsoleSelectorPlan{}, fmt.Errorf("invalid --project %q: must be a concrete LUCI project name without '/' or glob wildcards", rawProj)
		}
		if !explicitSeen[proj] {
			explicitSeen[proj] = true
			explicitProjects = append(explicitProjects, proj)
		}
	}

	defaultProj := strings.TrimSpace(defaultProject)
	var selectors []ConsoleSelector
	for _, rawArg := range args {
		trimmed := strings.TrimSpace(rawArg)
		if trimmed == "" {
			return ConsoleSelectorPlan{}, fmt.Errorf("invalid empty console selector")
		}
		var selProj, selConsole string
		if projPart, consPart, hasSlash := strings.Cut(trimmed, "/"); hasSlash {
			selProj = strings.TrimSpace(projPart)
			selConsole = strings.TrimSpace(consPart)
			if selProj == "" || selConsole == "" || strings.Contains(selConsole, "/") {
				return ConsoleSelectorPlan{}, fmt.Errorf("invalid console selector %q: expected <project>/<console>", rawArg)
			}
			if hasGlobMeta(selProj) {
				return ConsoleSelectorPlan{}, fmt.Errorf("invalid console selector %q: project name cannot contain glob wildcards", rawArg)
			}
		} else {
			selConsole = trimmed
			if len(explicitProjects) == 0 {
				if defaultProj == "" {
					return ConsoleSelectorPlan{}, fmt.Errorf("cannot resolve default LUCI project for console selector %q.\n\n"+
						"Specify a project explicitly or configure ci.try_buckets in .ghish.toml:\n"+
						"  gh run console --project <project> %s\n"+
						"  gh run console <project>/%s", rawArg, rawArg, rawArg)
				}
				selProj = defaultProj
			}
		}
		if _, err := path.Match(selConsole, ""); err != nil {
			return ConsoleSelectorPlan{}, fmt.Errorf("invalid console pattern %q: %w", rawArg, err)
		}
		selectors = append(selectors, ConsoleSelector{
			Raw:     trimmed,
			Project: selProj,
			Console: selConsole,
		})
	}

	if len(explicitProjects) > 0 && len(selectors) > 0 &&
		!slices.ContainsFunc(selectors, func(s ConsoleSelector) bool { return s.Project == "" }) {
		return ConsoleSelectorPlan{}, fmt.Errorf("--project cannot be combined with only qualified <project>/<console> selectors; omit --project or pass bare <console> filters")
	}

	if len(explicitProjects) == 0 && len(selectors) == 0 {
		if defaultProj == "" {
			return ConsoleSelectorPlan{}, fmt.Errorf("no LUCI project specified for 'gh run console'.\n\n" +
				"Specify a project or qualified console selector (or configure ci.try_buckets in .ghish.toml):\n" +
				"  gh run console --project <project> [--json]\n" +
				"  gh run console <project>/<console> [--json]")
		}
		explicitProjects = []string{defaultProj}
	}

	var queryProjects []string
	querySeen := make(map[string]bool)
	addQueryProject := func(p string) {
		if p != "" && !querySeen[p] {
			querySeen[p] = true
			queryProjects = append(queryProjects, p)
		}
	}
	for _, p := range explicitProjects {
		addQueryProject(p)
	}
	for _, sel := range selectors {
		addQueryProject(sel.Project)
	}

	return ConsoleSelectorPlan{
		ExplicitProjects: explicitProjects,
		Selectors:        selectors,
		QueryProjects:    queryProjects,
	}, nil
}

// QueryConsoleSnapshots calls `luci.milo.v1.MiloInternal/QueryConsoleSnapshots`
// across all pages for project.
func (c *LUCIClient) QueryConsoleSnapshots(ctx context.Context, project string) ([]miloConsoleSnapshot, error) {
	if strings.TrimSpace(project) == "" {
		return nil, fmt.Errorf("project is required for QueryConsoleSnapshots")
	}
	var all []miloConsoleSnapshot
	pageToken := ""
	for page := 0; page < maxLUCIDiscoveryPages; page++ {
		req := miloQueryConsoleSnapshotsRequest{
			Predicate: miloConsolePredicate{
				Project: project,
			},
			PageSize:  miloConsoleSnapshotsPageSize,
			PageToken: pageToken,
		}
		var resp miloQueryConsoleSnapshotsResponse
		if err := c.callPRPCWithMaxBytes(ctx, miloInternalService, "QueryConsoleSnapshots", req, &resp, maxBatchHTTPBodyBytes); err != nil {
			return nil, fmt.Errorf("querying LUCI Milo consoles for %q: %w", project, err)
		}
		all = append(all, resp.Snapshots...)
		next := strings.TrimSpace(resp.NextPageToken)
		if next == "" {
			return all, nil
		}
		if next == pageToken {
			return nil, fmt.Errorf("Milo QueryConsoleSnapshots returned duplicate page token for %q", project)
		}
		pageToken = next
	}
	return nil, fmt.Errorf("Milo QueryConsoleSnapshots exceeded %d pages for %q", maxLUCIDiscoveryPages, project)
}

// ListBuilders calls `buildbucket.v2.Builders/ListBuilders` across all pages
// for project (and optional bucket).
func (c *LUCIClient) ListBuilders(ctx context.Context, project, bucket string) ([]bbBuilderItem, error) {
	if strings.TrimSpace(project) == "" {
		return nil, fmt.Errorf("project is required for ListBuilders")
	}
	var all []bbBuilderItem
	pageToken := ""
	for page := 0; page < maxLUCIDiscoveryPages; page++ {
		req := bbListBuildersRequest{
			Project:   project,
			Bucket:    bucket,
			PageSize:  bbListBuildersPageSize,
			PageToken: pageToken,
		}
		var resp bbListBuildersResponse
		if err := c.callPRPCWithMaxBytes(ctx, buildbucketBuildersService, "ListBuilders", req, &resp, maxBatchHTTPBodyBytes); err != nil {
			return nil, fmt.Errorf("listing Buildbucket builders for %q: %w", project, err)
		}
		all = append(all, resp.Builders...)
		next := strings.TrimSpace(resp.NextPageToken)
		if next == "" {
			return all, nil
		}
		if next == pageToken {
			return nil, fmt.Errorf("Buildbucket ListBuilders returned duplicate page token for %q", project)
		}
		pageToken = next
	}
	return nil, fmt.Errorf("Buildbucket ListBuilders exceeded %d pages for %q", maxLUCIDiscoveryPages, project)
}

func isBuilderExperimental(cfg bbBuilderConfig) bool {
	if strings.EqualFold(strings.TrimSpace(cfg.Experimental), "YES") {
		return true
	}
	for k, pct := range cfg.Experiments {
		if pct <= 0 {
			continue
		}
		lk := strings.ToLower(strings.TrimSpace(k))
		if strings.Contains(lk, "non_production") || lk == "cq_experimental" || strings.HasSuffix(lk, ".cq_experimental") {
			return true
		}
	}
	return false
}

func normalizeBuilderKey(raw bbBuilder, defaultProject string) (bbBuilder, error) {
	proj := cmp.Or(strings.TrimSpace(raw.Project), defaultProject)
	bucket := strings.TrimSpace(raw.Bucket)
	builder := strings.TrimSpace(raw.Builder)
	if proj == "" || bucket == "" || builder == "" {
		return bbBuilder{}, fmt.Errorf("incomplete builder ID (%q/%q/%q)", proj, bucket, builder)
	}
	if p, shortBucket, ok := splitLUCIBucket(bucket, proj); ok {
		if p != "" {
			proj = p
		}
		bucket = shortBucket
	}
	return bbBuilder{Project: proj, Bucket: bucket, Builder: builder}, nil
}

func buildBuilderMetaMap(items []bbBuilderItem, defaultProject string) map[string]bbConfigBuilderMeta {
	out := make(map[string]bbConfigBuilderMeta, len(items))
	for _, it := range items {
		norm, err := normalizeBuilderKey(it.ID, defaultProject)
		if err != nil {
			continue
		}
		fullKey := norm.Project + "/" + norm.Bucket + "/" + norm.Builder
		out[fullKey] = bbConfigBuilderMeta{
			experimental:     isBuilderExperimental(it.Config),
			descriptionLinks: extractDescriptionLinks(it.Config.DescriptionHTML),
		}
	}
	return out
}

func tryFetchCrossProjectBuilderMeta(ctx context.Context, bbClient *LUCIClient, project string) (map[string]bbConfigBuilderMeta, error) {
	items, err := bbClient.ListBuilders(ctx, project, "")
	if err != nil {
		if ctx.Err() != nil || errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return nil, err
		}
		// OK: cross-project builder metadata (experimental flags and description links)
		// is best-effort when a public console embeds builders from another project
		// whose ACLs the caller might not hold.
		slog.Debug("skipping cross-project Buildbucket builder metadata", "project", project, "error", err)
		return nil, nil
	}
	return buildBuilderMetaMap(items, project), nil
}

func parseMiloBuilderRef(ref miloConsoleBuilderRef, defaultProject string) (bbBuilder, error) {
	if ref.ID != nil {
		if norm, err := normalizeBuilderKey(*ref.ID, defaultProject); err == nil {
			return norm, nil
		}
	}
	raw := strings.TrimSpace(ref.Name)
	if raw == "" {
		return bbBuilder{}, fmt.Errorf("empty builder reference")
	}
	raw = strings.TrimPrefix(raw, "buildbucket/")
	return ParseBuilderSelector(raw, defaultProject)
}

func extractSnapshotBuilders(snap miloConsoleSnapshot, defaultProject, consoleID string, errOut io.Writer) []bbBuilder {
	if snap.Console != nil && len(snap.Console.Builders) > 0 {
		out := make([]bbBuilder, 0, len(snap.Console.Builders))
		for _, ref := range snap.Console.Builders {
			parsed, err := parseMiloBuilderRef(ref, defaultProject)
			if err != nil {
				if errOut != nil {
					fmt.Fprintf(errOut, "Warning: skipping console %q builder %q: %v\n",
						sanitizeSingleLine(consoleID), sanitizeSingleLine(ref.Name), err)
				}
				continue
			}
			out = append(out, parsed)
		}
		return out
	}

	out := make([]bbBuilder, 0, len(snap.BuilderSnapshots))
	for _, bs := range snap.BuilderSnapshots {
		if bs.Builder == nil {
			continue
		}
		parsed, err := normalizeBuilderKey(*bs.Builder, defaultProject)
		if err != nil {
			if errOut != nil {
				fmt.Fprintf(errOut, "Warning: skipping console %q builder snapshot: %v\n",
					sanitizeSingleLine(consoleID), err)
			}
			continue
		}
		out = append(out, parsed)
	}
	return out
}

func consoleSelectorMatches(sel ConsoleSelector, project, consoleID string, explicitProjects map[string]bool) bool {
	if sel.Project != "" {
		if project != sel.Project {
			return false
		}
	} else if !explicitProjects[project] {
		return false
	}
	if consoleID == sel.Console || strings.EqualFold(consoleID, sel.Console) {
		return true
	}
	ok, _ := path.Match(sel.Console, consoleID)
	return ok
}

// DiscoverLUCIConsoleBuilders queries LUCI Milo console snapshots and
// Buildbucket builder metadata according to plan, returning deduplicated sorted
// builders filtered by experimental/non_production status unless
// includeExperimental is true.
func DiscoverLUCIConsoleBuilders(
	ctx context.Context,
	miloClient, bbClient *LUCIClient,
	plan ConsoleSelectorPlan,
	includeExperimental bool,
	errOut io.Writer,
) ([]DiscoveredConsoleBuilder, error) {
	explicitSet := make(map[string]bool, len(plan.ExplicitProjects))
	for _, p := range plan.ExplicitProjects {
		explicitSet[p] = true
	}
	querySet := make(map[string]bool, len(plan.QueryProjects))
	for _, p := range plan.QueryProjects {
		querySet[p] = true
	}

	matchedSelector := make([]bool, len(plan.Selectors))
	availableConsoles := make([]string, 0)
	byKey := make(map[string]*DiscoveredConsoleBuilder)
	builderProjects := make(map[string]bool)

	for _, proj := range plan.QueryProjects {
		snapshots, err := miloClient.QueryConsoleSnapshots(ctx, proj)
		if err != nil {
			return nil, err
		}
		for _, snap := range snapshots {
			if snap.Console == nil {
				continue
			}
			consoleID := strings.TrimSpace(cmp.Or(snap.Console.ID, snap.Console.Name))
			if consoleID == "" {
				continue
			}
			availableConsoles = append(availableConsoles, sanitizeSingleLine(proj+"/"+consoleID))

			matched := len(plan.Selectors) == 0
			for i, sel := range plan.Selectors {
				if consoleSelectorMatches(sel, proj, consoleID, explicitSet) {
					matchedSelector[i] = true
					matched = true
				}
			}
			if !matched {
				continue
			}

			for _, parsed := range extractSnapshotBuilders(snap, proj, consoleID, errOut) {
				fullKey := parsed.Project + "/" + parsed.Bucket + "/" + parsed.Builder
				builderProjects[parsed.Project] = true
				entry, exists := byKey[fullKey]
				if !exists {
					byKey[fullKey] = &DiscoveredConsoleBuilder{
						Project:          parsed.Project,
						Bucket:           parsed.Bucket,
						Builder:          parsed.Builder,
						Consoles:         []string{consoleID},
						DescriptionLinks: []BuilderDescriptionLink{},
					}
				} else {
					entry.Consoles = append(entry.Consoles, consoleID)
				}
			}
		}
	}

	if len(plan.Selectors) > 0 {
		var unmatched []string
		for i, sel := range plan.Selectors {
			if !matchedSelector[i] {
				unmatched = append(unmatched, sanitizeSingleLine(sel.Raw))
			}
		}
		if len(unmatched) > 0 {
			availableConsoles = dedup(availableConsoles)
			slices.Sort(availableConsoles)
			if len(availableConsoles) > 0 {
				return nil, fmt.Errorf("no consoles matching %s.\n\nAvailable consoles:\n  - %s",
					strings.Join(unmatched, ", "), strings.Join(availableConsoles, "\n  - "))
			}
			return nil, fmt.Errorf("no consoles found matching %s in project(s) %s",
				strings.Join(unmatched, ", "), strings.Join(plan.QueryProjects, ", "))
		}
	}

	if len(byKey) == 0 {
		return []DiscoveredConsoleBuilder{}, nil
	}

	bbMeta := make(map[string]bbConfigBuilderMeta)
	for _, proj := range slices.Sorted(maps.Keys(builderProjects)) {
		var projMeta map[string]bbConfigBuilderMeta
		if querySet[proj] {
			items, err := bbClient.ListBuilders(ctx, proj, "")
			if err != nil {
				return nil, err
			}
			projMeta = buildBuilderMetaMap(items, proj)
		} else {
			meta, err := tryFetchCrossProjectBuilderMeta(ctx, bbClient, proj)
			if err != nil {
				return nil, err
			}
			projMeta = meta
		}
		maps.Copy(bbMeta, projMeta)
	}

	out := make([]DiscoveredConsoleBuilder, 0, len(byKey))
	for _, fullKey := range slices.Sorted(maps.Keys(byKey)) {
		entry := byKey[fullKey]
		if meta, hasMeta := bbMeta[fullKey]; hasMeta {
			entry.Experimental = meta.experimental
			if len(meta.descriptionLinks) > 0 {
				entry.DescriptionLinks = meta.descriptionLinks
			}
		}
		if !includeExperimental && entry.Experimental {
			continue
		}
		entry.Consoles = dedup(entry.Consoles)
		slices.Sort(entry.Consoles)
		out = append(out, *entry)
	}
	return out, nil
}
