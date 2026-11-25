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
	"io"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func installMockAlphaBetaLUCI(server *MockGerritServer) {
	server.OnMiloConsoleSnapshotsByProject(func(req miloQueryConsoleSnapshotsRequest) miloQueryConsoleSnapshotsResponse {
		switch req.Predicate.Project {
		case "alpha":
			return miloQueryConsoleSnapshotsResponse{
				Snapshots: []miloConsoleSnapshot{
					{
						Console: &miloConsole{
							ID:   "alpha-ci",
							Name: "Alpha CI",
							Builders: []miloConsoleBuilderRef{
								{
									Name: "buildbucket/luci.alpha.ci/linux-bazel",
									ID:   &bbBuilder{Project: "alpha", Bucket: "ci", Builder: "linux-bazel"},
								},
								{
									Name: "buildbucket/luci.alpha.ci/mac-gn",
								},
								{
									Name: "buildbucket/luci.alpha.ci/exp-canary",
									ID:   &bbBuilder{Project: "alpha", Bucket: "ci", Builder: "exp-canary"},
								},
								{
									Name: "buildbucket/luci.alpha.ci/nonprod-canary",
									ID:   &bbBuilder{Project: "alpha", Bucket: "ci", Builder: "nonprod-canary"},
								},
								{
									Name: "buildbucket/luci.alpha.ci/cqexp-canary",
									ID:   &bbBuilder{Project: "alpha", Bucket: "ci", Builder: "cqexp-canary"},
								},
							},
						},
					},
					{
						Console: &miloConsole{
							// Exercise Name fallback when ID is empty and BuilderSnapshots fallback when Builders is empty.
							Name: "alpha-roll",
						},
						BuilderSnapshots: []miloBuilderSnapshot{
							{Builder: &bbBuilder{Project: "alpha", Bucket: "luci.alpha.roll", Builder: "dep-roller"}},
						},
					},
				},
			}
		case "beta":
			return miloQueryConsoleSnapshotsResponse{
				Snapshots: []miloConsoleSnapshot{
					{
						Console: &miloConsole{
							ID: "beta-ci",
							Builders: []miloConsoleBuilderRef{
								{
									Name: "buildbucket/luci.beta.ci/host-tests",
									ID:   &bbBuilder{Project: "beta", Bucket: "ci", Builder: "host-tests"},
								},
							},
						},
					},
				},
			}
		default:
			return miloQueryConsoleSnapshotsResponse{Snapshots: []miloConsoleSnapshot{}}
		}
	})

	server.OnListBuildersByProject(func(req bbListBuildersRequest) (bbListBuildersResponse, int, string) {
		switch req.Project {
		case "alpha":
			return bbListBuildersResponse{
				Builders: []bbBuilderItem{
					{
						ID: bbBuilder{Project: "alpha", Bucket: "ci", Builder: "linux-bazel"},
						Config: bbBuilderConfig{
							Name:            "linux-bazel",
							Experiments:     map[string]int{"pigweed.disable_github": 20},
							DescriptionHTML: `<a href="https://example.com/runbook?a=1&amp;b=2"><b>Runbook</b></a> <a href='https://docs.example.com/ops'>Ops Docs</a> <a href="javascript:alert(1)">Bad</a>`,
						},
					},
					{
						ID: bbBuilder{Project: "alpha", Bucket: "ci", Builder: "mac-gn"},
					},
					{
						ID:     bbBuilder{Project: "alpha", Bucket: "ci", Builder: "exp-canary"},
						Config: bbBuilderConfig{Experimental: "YES"},
					},
					{
						ID:     bbBuilder{Project: "alpha", Bucket: "ci", Builder: "nonprod-canary"},
						Config: bbBuilderConfig{Experiments: map[string]int{"luci.non_production": 100}},
					},
					{
						ID:     bbBuilder{Project: "alpha", Bucket: "ci", Builder: "cqexp-canary"},
						Config: bbBuilderConfig{Experiments: map[string]int{"cq_experimental": 100}},
					},
				},
			}, http.StatusOK, ""
		case "beta":
			return bbListBuildersResponse{
				Builders: []bbBuilderItem{
					{ID: bbBuilder{Project: "beta", Bucket: "ci", Builder: "host-tests"}},
				},
			}, http.StatusOK, ""
		default:
			return bbListBuildersResponse{Builders: []bbBuilderItem{}}, http.StatusOK, ""
		}
	})
}

func TestDiscoverLUCIConsoleBuilders_PaginationAndFallbacks(t *testing.T) {
	NewMockGit(t)
	server := NewMockGerritServer(t)

	server.OnMiloConsoleSnapshotsByProject(func(req miloQueryConsoleSnapshotsRequest) miloQueryConsoleSnapshotsResponse {
		switch req.PageToken {
		case "":
			return miloQueryConsoleSnapshotsResponse{
				Snapshots: []miloConsoleSnapshot{
					{
						Console: &miloConsole{
							ID: "ci-one",
							Builders: []miloConsoleBuilderRef{
								{ID: &bbBuilder{Project: "alpha", Bucket: "ci", Builder: "builder-a"}},
							},
						},
					},
				},
				NextPageToken: "milo-p2",
			}
		case "milo-p2":
			return miloQueryConsoleSnapshotsResponse{
				Snapshots: []miloConsoleSnapshot{
					{
						Console: &miloConsole{Name: "ci-two"},
						BuilderSnapshots: []miloBuilderSnapshot{
							{Builder: &bbBuilder{Project: "alpha", Bucket: "ci", Builder: "builder-b"}},
						},
					},
				},
			}
		default:
			t.Fatalf("unexpected Milo pageToken %q", req.PageToken)
			return miloQueryConsoleSnapshotsResponse{}
		}
	})

	server.OnListBuildersByProject(func(req bbListBuildersRequest) (bbListBuildersResponse, int, string) {
		switch req.PageToken {
		case "":
			return bbListBuildersResponse{
				Builders: []bbBuilderItem{
					{
						ID:     bbBuilder{Project: "alpha", Bucket: "ci", Builder: "builder-a"},
						Config: bbBuilderConfig{Experiments: map[string]int{"pigweed.disable_github": 20}},
					},
				},
				NextPageToken: "bb-p2",
			}, http.StatusOK, ""
		case "bb-p2":
			return bbListBuildersResponse{
				Builders: []bbBuilderItem{
					{
						ID:     bbBuilder{Project: "alpha", Bucket: "ci", Builder: "builder-b"},
						Config: bbBuilderConfig{DescriptionHTML: "<a href=\"https://example.com/b\x1b[31m\">Desc\nLink</a>"},
					},
				},
			}, http.StatusOK, ""
		default:
			t.Fatalf("unexpected ListBuilders pageToken %q", req.PageToken)
			return bbListBuildersResponse{}, http.StatusBadRequest, ""
		}
	})

	client := NewLUCIClient(server.URL, server.Server.Client())
	plan, err := ParseConsoleSelectors([]string{"alpha"}, nil, "")
	if err != nil {
		t.Fatalf("ParseConsoleSelectors failed: %v", err)
	}
	discovered, err := DiscoverLUCIConsoleBuilders(t.Context(), client, client, plan, false, io.Discard)
	if err != nil {
		t.Fatalf("DiscoverLUCIConsoleBuilders failed: %v", err)
	}
	if len(discovered) != 2 {
		t.Fatalf("len(discovered) = %d, want 2: %+v", len(discovered), discovered)
	}
	if discovered[0].Builder != "builder-a" || discovered[1].Builder != "builder-b" {
		t.Errorf("unexpected builders: %+v", discovered)
	}
	if !slices.Equal(discovered[1].Consoles, []string{"ci-two"}) {
		t.Errorf("discovered[1].Consoles = %v, want [ci-two]", discovered[1].Consoles)
	}
	if len(discovered[1].DescriptionLinks) != 1 ||
		discovered[1].DescriptionLinks[0].Text != "Desc Link" ||
		strings.Contains(discovered[1].DescriptionLinks[0].URL, "\x1b") {
		t.Errorf("unexpected sanitized links: %+v", discovered[1].DescriptionLinks)
	}
}

func TestDiscoverLUCIConsoleBuilders_CrossProjectFailOpenAndTargetFailClosed(t *testing.T) {
	NewMockGit(t)
	server := NewMockGerritServer(t)

	server.OnMiloConsoleSnapshots(miloConsoleSnapshot{
		Console: &miloConsole{
			ID: "auto-submit",
			Builders: []miloConsoleBuilderRef{
				{ID: &bbBuilder{Project: "pigweed", Bucket: "ci", Builder: "pub-builder"}},
				{ID: &bbBuilder{Project: "pigweed-internal", Bucket: "ci", Builder: "priv-builder"}},
			},
		},
	})
	server.OnListBuildersByProject(func(req bbListBuildersRequest) (bbListBuildersResponse, int, string) {
		if req.Project == "pigweed-internal" {
			return bbListBuildersResponse{}, http.StatusForbidden, "permission denied"
		}
		return bbListBuildersResponse{
			Builders: []bbBuilderItem{
				{ID: bbBuilder{Project: "pigweed", Bucket: "ci", Builder: "pub-builder"}},
			},
		}, http.StatusOK, ""
	})

	client := NewLUCIClient(server.URL, server.Server.Client())
	plan, err := ParseConsoleSelectors([]string{"pigweed"}, nil, "")
	if err != nil {
		t.Fatalf("ParseConsoleSelectors failed: %v", err)
	}
	// Cross-project ACL failure on pigweed-internal must fail open.
	discovered, err := DiscoverLUCIConsoleBuilders(t.Context(), client, client, plan, false, io.Discard)
	if err != nil {
		t.Fatalf("expected cross-project ListBuilders 403 to fail open, got: %v", err)
	}
	if len(discovered) != 2 || discovered[0].Project != "pigweed-internal" || discovered[1].Project != "pigweed" {
		t.Errorf("expected both cross-project and pub builders preserved in sorted order, got: %+v", discovered)
	}

	// Targeted project ListBuilders failure must fail closed.
	targetInternalPlan, err := ParseConsoleSelectors([]string{"pigweed-internal"}, nil, "")
	if err != nil {
		t.Fatalf("ParseConsoleSelectors failed: %v", err)
	}
	server.OnMiloConsoleSnapshots(miloConsoleSnapshot{
		Console: &miloConsole{
			ID: "internal-ci",
			Builders: []miloConsoleBuilderRef{
				{ID: &bbBuilder{Project: "pigweed-internal", Bucket: "ci", Builder: "priv-builder"}},
			},
		},
	})
	if _, err := DiscoverLUCIConsoleBuilders(t.Context(), client, client, targetInternalPlan, false, io.Discard); err == nil {
		t.Errorf("expected targeted ListBuilders 403 to fail closed")
	}

	// Context cancellation in cross-project fetch must propagate rather than be swallowed.
	canceledCtx, cancel := context.WithCancel(t.Context())
	cancel()
	if _, err := tryFetchCrossProjectBuilderMeta(canceledCtx, client, "pigweed-internal"); !errors.Is(err, context.Canceled) {
		t.Errorf("expected context.Canceled to propagate, got: %v", err)
	}
}

func TestRunConsole_SelectorsExperimentalLinksJSONAndText(t *testing.T) {
	NewMockGit(t)
	MockCWD = t.TempDir()
	server := NewMockGerritServer(t)
	installMockAlphaBetaLUCI(server)

	server.OnSearchBuilds()
	server.OnBuildsBatch(func(sub bbBatchSubRequest) bbBatchSubResponse {
		b := sub.SearchBuilds.Predicate.Builder.Builder
		switch b {
		case "linux-bazel":
			return BatchSearchResp(
				FakeBuild("9001", "linux-bazel", "CANCELED"),
				FakeBuild("9002", "linux-bazel", "FAILURE"),
				FakeBuild("9003", "linux-bazel", "INFRA_FAILURE"),
				FakeBuild("9004", "linux-bazel", "SUCCESS"),
			)
		case "mac-gn":
			return BatchSearchResp(FakeBuild("9010", "mac-gn", "SUCCESS"))
		case "host-tests":
			return BatchErrorResp(7, "permission denied")
		default:
			return BatchSearchResp()
		}
	})

	// 1. Repeatable --project + --json filters out experimental/non_production/cq_experimental builders
	//    while preserving disable_github experiment builders, description links, bare console IDs, and streaks.
	var jsonOut, jsonErr strings.Builder
	err := executeCommandIO(RootCmd, strings.NewReader(""), &jsonOut, &jsonErr,
		"run", "console", "--project", "alpha", "--project", "beta", "--json")
	if err != nil {
		t.Fatalf("run console --json failed: %v\nStderr: %s", err, jsonErr.String())
	}
	var items []ConsoleBuilderItem
	if err := json.Unmarshal([]byte(jsonOut.String()), &items); err != nil {
		t.Fatalf("unmarshal []ConsoleBuilderItem: %v\n%s", err, jsonOut.String())
	}
	if len(items) != 4 {
		t.Fatalf("len(items) = %d, want 4: %+v", len(items), items)
	}
	if items[0].Builder != "linux-bazel" || items[0].Status != "FAILURE" || items[0].ConsecutiveFailures != 2 || len(items[0].RecentBuilds) != 4 {
		t.Errorf("items[0] = %+v, want linux-bazel FAILURE streak=2", items[0])
	}
	if !slices.Equal(items[0].Consoles, []string{"alpha-ci"}) {
		t.Errorf("items[0].Consoles = %v, want bare [alpha-ci]", items[0].Consoles)
	}
	if len(items[0].DescriptionLinks) != 2 ||
		items[0].DescriptionLinks[0].Text != "Runbook" ||
		items[0].DescriptionLinks[0].URL != "https://example.com/runbook?a=1&b=2" ||
		items[0].DescriptionLinks[1].URL != "https://docs.example.com/ops" {
		t.Errorf("unexpected descriptionLinks: %+v", items[0].DescriptionLinks)
	}
	if items[1].Builder != "host-tests" || items[1].Status != "ERROR" || items[1].Error != "permission denied" {
		t.Errorf("items[1] = %+v, want host-tests ERROR", items[1])
	}
	if items[2].Builder != "mac-gn" || items[2].Status != "SUCCESS" {
		t.Errorf("items[2] = %+v, want mac-gn SUCCESS", items[2])
	}
	if items[3].Builder != "dep-roller" || items[3].Status != "UNBUILT" || len(items[3].RecentBuilds) != 0 {
		t.Errorf("items[3] = %+v, want dep-roller UNBUILT", items[3])
	}

	// 2. Qualified <project>/<glob> selector renders text output with Verdict and Next without --config-dir.
	textOut, err := executeCommand(RootCmd, "run", "console", "alpha/alpha-*")
	if err != nil {
		t.Fatalf("run console text failed: %v\n%s", err, textOut)
	}
	if !strings.Contains(textOut, "Verdict: FAILED (1 of 3 builders failing)") ||
		!strings.Contains(textOut, "Next:    gh run view 9002 --log-failed") ||
		strings.Contains(textOut, "--config-dir") {
		t.Errorf("unexpected text output:\n%s", textOut)
	}

	// 3. -e/--experimental includes experimental, non_production, and cq_experimental builders.
	expOut, err := executeCommand(RootCmd, "run", "console", "alpha/alpha-ci", "-e", "--json")
	if err != nil {
		t.Fatalf("run console -e --json failed: %v\n%s", err, expOut)
	}
	var expItems []ConsoleBuilderItem
	if err := json.Unmarshal([]byte(expOut), &expItems); err != nil {
		t.Fatalf("unmarshal -e output: %v", err)
	}
	if len(expItems) != 5 {
		t.Fatalf("len(expItems) with -e = %d, want 5: %+v", len(expItems), expItems)
	}

	// 4. Default project resolved from .ghish.toml ci.try_buckets when selectors omit project.
	repoDir := t.TempDir()
	if err := os.Mkdir(filepath.Join(repoDir, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repoDir, ".ghish.toml"), []byte("[ci]\ntry_buckets = [\"alpha/try\"]\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	MockCWD = repoDir
	defOut, err := executeCommand(RootCmd, "run", "console", "alpha-roll", "--json")
	if err != nil {
		t.Fatalf("default project run console failed: %v\n%s", err, defOut)
	}
	var defItems []ConsoleBuilderItem
	if err := json.Unmarshal([]byte(defOut), &defItems); err != nil {
		t.Fatalf("unmarshal default project output: %v", err)
	}
	if len(defItems) != 1 || defItems[0].Builder != "dep-roller" {
		t.Errorf("got %+v, want [dep-roller]", defItems)
	}

	// 5. -L/--limit and -s/--status forward PageSize and Predicate.Status accurately,
	//    while validation and missing project/unknown console errors fail closed.
	var seenBatchLimit int
	var seenBatchStatus string
	server.OnBuildsBatch(func(sub bbBatchSubRequest) bbBatchSubResponse {
		seenBatchLimit = sub.SearchBuilds.PageSize
		seenBatchStatus = sub.SearchBuilds.Predicate.Status
		return BatchSearchResp(FakeBuild("9100", sub.SearchBuilds.Predicate.Builder.Builder, "SUCCESS"))
	})
	if _, err := executeCommand(RootCmd, "run", "console", "--project", "alpha", "alpha-ci", "-L", "3", "-s", "completed", "--json"); err != nil {
		t.Fatalf("run console --project alpha alpha-ci -L 3 -s completed failed: %v", err)
	}
	if seenBatchLimit != 3 || seenBatchStatus != "ENDED_MASK" {
		t.Errorf("forwarded batch args = (%d, %q), want (3, %q)", seenBatchLimit, seenBatchStatus, "ENDED_MASK")
	}

	// Empty console snapshot discovery emits [] (not null) under --json.
	server.OnMiloConsoleSnapshots()
	emptyJSON, err := executeCommand(RootCmd, "run", "console", "--project", "alpha", "--json")
	if err != nil {
		t.Fatalf("empty run console --json failed: %v", err)
	}
	if strings.TrimSpace(emptyJSON) != "[]" {
		t.Errorf("expected [] JSON for empty console discovery, got %q", emptyJSON)
	}

	installMockAlphaBetaLUCI(server)
	MockCWD = t.TempDir()
	if _, err := executeCommand(RootCmd, "run", "console"); err == nil || !strings.Contains(err.Error(), "no LUCI project specified") {
		t.Errorf("expected missing LUCI project error, got: %v", err)
	}
	if _, err := executeCommand(RootCmd, "run", "console", "alpha/nonexistent"); err == nil ||
		!strings.Contains(err.Error(), "alpha/alpha-ci") {
		t.Errorf("expected unknown console error listing alpha/alpha-ci, got: %v", err)
	}
	for _, tc := range []struct {
		args    []string
		wantErr string
	}{
		{[]string{"run", "console", "--project", "alpha", "-L", "0"}, "invalid --limit"},
		{[]string{"run", "console", "--project", "alpha", "-L", "101"}, "invalid --limit"},
		{[]string{"run", "console", "--project", "alpha", "-s", "bogus"}, "invalid --status"},
		{[]string{"run", "console", "--project", "alpha", "beta/beta-ci"}, "--project cannot be combined"},
	} {
		if _, err := executeCommand(RootCmd, tc.args...); err == nil || !strings.Contains(err.Error(), tc.wantErr) {
			t.Errorf("args %v: expected error containing %q, got %v", tc.args, tc.wantErr, err)
		}
	}
}

func TestParseConsoleSelectors_ValidationAndEdgeCases(t *testing.T) {
	if plan, err := ParseConsoleSelectors([]string{"alpha"}, []string{"alpha-ci"}, ""); err != nil ||
		!slices.Equal(plan.QueryProjects, []string{"alpha"}) || len(plan.Selectors) != 1 || plan.Selectors[0].Project != "" {
		t.Fatalf("expected --project alpha + bare alpha-ci valid, got (%+v, %v)", plan, err)
	}
	for _, tc := range []struct {
		name     string
		projects []string
		args     []string
		defProj  string
	}{
		{"empty --project", []string{""}, nil, ""},
		{"slash in --project", []string{"a/b"}, nil, ""},
		{"glob in --project", []string{"a*"}, nil, ""},
		{"empty arg", nil, []string{"  "}, "alpha"},
		{"multi slash selector", nil, []string{"a/b/c"}, ""},
		{"glob in project part", nil, []string{"a*/b"}, ""},
		{"bad glob pattern", nil, []string{"alpha/["}, ""},
		{"missing default for bare", nil, []string{"alpha-ci"}, ""},
		{"--project with only qualified selectors", []string{"alpha"}, []string{"alpha/alpha-ci"}, ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if plan, err := ParseConsoleSelectors(tc.projects, tc.args, tc.defProj); err == nil {
				t.Errorf("expected error for %s, got plan %+v", tc.name, plan)
			}
		})
	}
}

func TestRunConsole_CollapsingVerdictsAndUntrustedBuildbucketHost(t *testing.T) {
	NewMockGit(t)
	MockCWD = t.TempDir()
	server := NewMockGerritServer(t)

	sevenBuilders := make([]miloConsoleBuilderRef, 7)
	sevenItems := make([]bbBuilderItem, 7)
	for i := range 7 {
		name := "builder-" + string(rune('a'+i))
		sevenBuilders[i] = miloConsoleBuilderRef{
			ID: &bbBuilder{Project: "proj", Bucket: "ci", Builder: name},
		}
		sevenItems[i] = bbBuilderItem{ID: bbBuilder{Project: "proj", Bucket: "ci", Builder: name}}
	}

	server.OnMiloConsoleSnapshots(miloConsoleSnapshot{
		Console: &miloConsole{ID: "ci", Builders: sevenBuilders},
	})
	server.OnListBuilders(sevenItems...)
	server.OnBuildsBatch(func(bbBatchSubRequest) bbBatchSubResponse {
		return BatchSearchResp(FakeBuild("7001", "", "SUCCESS"))
	})

	collapsedOut, err := executeCommand(RootCmd, "run", "console", "--project", "proj")
	if err != nil {
		t.Fatalf("collapsed run console failed: %v\n%s", err, collapsedOut)
	}
	if !strings.Contains(collapsedOut, "... and 2 more passing/unbuilt builder(s)") ||
		!strings.Contains(collapsedOut, "Verdict: PASSED (7 passed)") ||
		!strings.Contains(collapsedOut, "Next:    gh run console --project proj --json") {
		t.Errorf("unexpected collapsed summary:\n%s", collapsedOut)
	}

	expandedOut, err := executeCommand(RootCmd, "run", "console", "--project", "proj", "--all")
	if err != nil {
		t.Fatalf("expanded run console failed: %v\n%s", err, expandedOut)
	}
	if strings.Contains(expandedOut, "more passing/unbuilt") || !strings.Contains(expandedOut, "builder-g") {
		t.Errorf("expected all 7 builders shown with --all, got:\n%s", expandedOut)
	}

	// Switch to 2 builders to test PENDING, CANCELED, INCONCLUSIVE, ERROR, and NO BUILDERS verdicts.
	server.OnMiloConsoleSnapshots(miloConsoleSnapshot{
		Console: &miloConsole{ID: "ci", Builders: sevenBuilders[:2]},
	})
	server.OnListBuilders(sevenItems[:2]...)
	mode := "pending"
	server.OnBuildsBatch(func(sub bbBatchSubRequest) bbBatchSubResponse {
		switch mode {
		case "pending":
			return BatchSearchResp(FakeBuild("6001", "", "STARTED"))
		case "canceled":
			return BatchSearchResp(FakeBuild("6002", "", "CANCELED"))
		case "error":
			if sub.SearchBuilds.Predicate.Builder.Builder == "builder-a" {
				return BatchErrorResp(13, "backend error")
			}
			return BatchSearchResp()
		default:
			return BatchSearchResp()
		}
	})

	pendingOut, err := executeCommand(RootCmd, "run", "console", "proj/ci")
	if err != nil || !strings.Contains(pendingOut, "Verdict: PENDING (2 running)") {
		t.Fatalf("expected Verdict: PENDING (2 running), err=%v\n%s", err, pendingOut)
	}

	mode = "canceled"
	canceledOut, err := executeCommand(RootCmd, "run", "console", "proj/ci")
	if err != nil || !strings.Contains(canceledOut, "Verdict: CANCELED (2 canceled)") {
		t.Fatalf("expected Verdict: CANCELED (2 canceled), err=%v\n%s", err, canceledOut)
	}

	mode = "unbuilt"
	unbuiltOut, err := executeCommand(RootCmd, "run", "console", "proj/ci")
	if err != nil || !strings.Contains(unbuiltOut, "Verdict: INCONCLUSIVE") {
		t.Fatalf("expected Verdict: INCONCLUSIVE, err=%v\n%s", err, unbuiltOut)
	}

	mode = "error"
	errorOut, err := executeCommand(RootCmd, "run", "console", "proj/ci")
	if err != nil || !strings.Contains(errorOut, "Verdict: FAILED (1 of 2 builders failed to query)") {
		t.Fatalf("expected Verdict: FAILED query error, err=%v\n%s", err, errorOut)
	}

	server.OnListBuilders(
		bbBuilderItem{ID: bbBuilder{Project: "proj", Bucket: "ci", Builder: "builder-a"}, Config: bbBuilderConfig{Experimental: "YES"}},
		bbBuilderItem{ID: bbBuilder{Project: "proj", Bucket: "ci", Builder: "builder-b"}, Config: bbBuilderConfig{Experimental: "YES"}},
	)
	noBuildersOut, err := executeCommand(RootCmd, "run", "console", "proj/ci")
	if err != nil || !strings.Contains(noBuildersOut, "Verdict: NO BUILDERS") || !strings.Contains(noBuildersOut, "Next:    gh run console proj/ci -e") {
		t.Fatalf("expected Verdict: NO BUILDERS with -e hint, err=%v\n%s", err, noBuildersOut)
	}

	// Empty project with -e already enabled should suggest --json instead of redundant -e.
	server.OnMiloConsoleSnapshots()
	noBuildersExpOut, err := executeCommand(RootCmd, "run", "console", "--project", "empty-proj", "-e")
	if err != nil || !strings.Contains(noBuildersExpOut, "Next:    gh run console --project empty-proj --json") {
		t.Fatalf("expected --json Next hint when -e already set, err=%v\n%s", err, noBuildersExpOut)
	}

	// Console builder names with spaces are URL-escaped in builderUrl.
	if got := luciBuilderConsoleURL("my proj", "ci", "Win7 Tests (1)"); got != "https://ci.chromium.org/p/my%20proj/builders/ci/Win7%20Tests%20%281%29" {
		t.Errorf("luciBuilderConsoleURL escaping = %q", got)
	}

	// Untrusted ci.buildbucket_host in .ghish.toml must fail closed.
	untrustedRepo := t.TempDir()
	if err := os.Mkdir(filepath.Join(untrustedRepo, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(untrustedRepo, ".ghish.toml"), []byte("[ci]\nbuildbucket_host = \"evil.example.com\"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	MockCWD = untrustedRepo
	if _, err := executeCommand(RootCmd, "run", "console", "--project", "proj"); err == nil || !strings.Contains(err.Error(), "untrusted ci.buildbucket_host") {
		t.Errorf("expected untrusted ci.buildbucket_host error, got: %v", err)
	}
}
