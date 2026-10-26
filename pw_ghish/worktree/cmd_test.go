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

package worktree

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"
)

func execWTCommand(t *testing.T, mgr *Manager, args ...string) (string, error) {
	t.Helper()
	cmd := NewCommand(mgr)
	var out bytes.Buffer
	cmd.SetOut(&out)
	cmd.SetErr(&out)
	cmd.SetArgs(args)
	err := cmd.Execute()
	return out.String(), err
}

func TestWTCommands_EndToEndCLIAndJSON(t *testing.T) {
	mgr, _, _, _ := setupTestManager(t, 2)

	// 1. `wt init --check` and `wt audit` alias (human readable & JSON)
	outInit, err := execWTCommand(t, mgr, "init", "--check")
	if err != nil {
		t.Fatalf("wt init --check failed: %v\nOutput: %s", err, outInit)
	}
	if !strings.Contains(outInit, "Inspecting gh-ish Worktree Environment") {
		t.Errorf("expected init banner in output, got:\n%s", outInit)
	}
	if strings.Contains(outInit, "===") || strings.Contains(outInit, "[✓]") || strings.Contains(outInit, "[!]") {
		t.Errorf("expected wt init output without === walls or bracketed status glyphs, got:\n%s", outInit)
	}
	if !strings.Contains(outInit, "Verdict:") || !strings.Contains(outInit, "Next:") {
		t.Errorf("expected Verdict: and Next: in wt init output, got:\n%s", outInit)
	}

	outAudit, err := execWTCommand(t, mgr, "audit")
	if err != nil {
		t.Fatalf("wt audit failed: %v\nOutput: %s", err, outAudit)
	}
	if !strings.Contains(outAudit, "Inspecting gh-ish Worktree Environment (Read-Only Check)...") {
		t.Errorf("expected wt audit to run in read-only check mode, got:\n%s", outAudit)
	}
	if strings.Contains(outAudit, "===") || strings.Contains(outAudit, "[✓]") {
		t.Errorf("expected wt audit without === walls or [✓] brackets, got:\n%s", outAudit)
	}
	if !strings.Contains(outAudit, "Verdict:") || !strings.Contains(outAudit, "Next:") {
		t.Errorf("expected Verdict: and Next: in wt audit output, got:\n%s", outAudit)
	}

	outInitJSON, err := execWTCommand(t, mgr, "init", "--check", "--json")
	if err != nil {
		t.Fatalf("wt init --json failed: %v", err)
	}
	var initPayload map[string]any
	if err := json.Unmarshal([]byte(outInitJSON), &initPayload); err != nil {
		t.Fatalf("failed to parse init JSON: %v\nRaw: %s", err, outInitJSON)
	}
	if _, ok := initPayload["checklist"]; !ok {
		t.Errorf("expected 'checklist' key in init JSON payload")
	}

	// 2. `wt use <project>` (human receipt & JSON)
	outUse, err := execWTCommand(t, mgr, "use", "cli-test-proj")
	if err != nil {
		t.Fatalf("wt use failed: %v", err)
	}
	if !strings.HasPrefix(strings.TrimSpace(outUse), "✓ Project") || !strings.Contains(outUse, "Next:") {
		t.Errorf("expected wt use receipt to start with '✓ Project' and include 'Next:', got: %s", outUse)
	}

	outUseJSON, err := execWTCommand(t, mgr, "use", "cli-test-proj", "--json")
	if err != nil {
		t.Fatalf("wt use --json failed: %v", err)
	}
	var useRes UseResult
	if err := json.Unmarshal([]byte(outUseJSON), &useRes); err != nil {
		t.Fatalf("failed to unmarshal use JSON: %v", err)
	}
	if useRes.Project != "cli-test-proj" || useRes.Slot != "pw-01" {
		t.Errorf("unexpected UseResult: %+v", useRes)
	}

	// 3. Invalid lease mode error check
	_, err = execWTCommand(t, mgr, "use", "cli-test-proj", "--mode=invalid")
	if err == nil || !strings.Contains(err.Error(), "Remediation:") {
		t.Errorf("expected actionable Remediation error for invalid --mode, got: %v", err)
	}

	// 4. `wt list` (human table & JSON)
	outList, err := execWTCommand(t, mgr, "list")
	if err != nil {
		t.Fatalf("wt list failed: %v", err)
	}
	if !strings.Contains(outList, "cli-test-proj") || !strings.Contains(outList, "CLEAN_SYNCED") {
		t.Errorf("expected cli-test-proj in CLEAN_SYNCED state in list output, got:\n%s", outList)
	}
	if strings.Contains(outList, "===") || strings.Contains(outList, "RECOMMENDED ACTION") {
		t.Errorf("expected compact wt list without === walls or 5th column, got:\n%s", outList)
	}
	if !strings.Contains(outList, "Parked projects: none") {
		t.Errorf("expected compact 'Parked projects: none' footer when 0 parked projects, got:\n%s", outList)
	}
	if !strings.Contains(outList, "Verdict: 1/2 slots occupied (1 available)") || !strings.Contains(outList, "Next:    gh wt use <project>") {
		t.Errorf("expected Verdict: and Next: footer in wt list, got:\n%s", outList)
	}

	outListJSON, err := execWTCommand(t, mgr, "list", "--json")
	if err != nil {
		t.Fatalf("wt list --json failed: %v", err)
	}
	var listReport DashboardReport
	if err := json.Unmarshal([]byte(outListJSON), &listReport); err != nil {
		t.Fatalf("failed to unmarshal list JSON: %v", err)
	}
	if len(listReport.MountedProjects) != 1 {
		t.Errorf("expected 1 mounted project in JSON dashboard, got %d", len(listReport.MountedProjects))
	}

	// 5. `wt next <project>`
	outNext, err := execWTCommand(t, mgr, "next", "cli-test-proj")
	if err != nil {
		t.Fatalf("wt next failed: %v", err)
	}
	if !strings.HasPrefix(strings.TrimSpace(outNext), "✓ Rebased project") || !strings.Contains(outNext, "Next:   gh pr create") {
		t.Errorf("expected wt next receipt to start with '✓ Rebased project' and include Next:, got: %s", outNext)
	}

	// 6. `wt park <project>`
	outPark, err := execWTCommand(t, mgr, "park", "cli-test-proj")
	if err != nil {
		t.Fatalf("wt park failed: %v", err)
	}
	if !strings.HasPrefix(strings.TrimSpace(outPark), "✓ Parked project") || !strings.Contains(outPark, "Next:   gh wt use cli-test-proj") {
		t.Errorf("expected wt park receipt to start with '✓ Parked project' and include Next:, got: %s", outPark)
	}

	// 7. `wt close <project>`
	outClose, err := execWTCommand(t, mgr, "close", "cli-test-proj")
	if err != nil {
		t.Fatalf("wt close failed: %v", err)
	}
	if !strings.HasPrefix(strings.TrimSpace(outClose), "✓ Closed project") || !strings.Contains(outClose, "Next:   gh wt list") {
		t.Errorf("expected wt close receipt to start with '✓ Closed project' and include Next:, got: %s", outClose)
	}

	// 8. `wt gc --dry-run`
	outGC, err := execWTCommand(t, mgr, "gc", "--dry-run")
	if err != nil {
		t.Fatalf("wt gc failed: %v", err)
	}
	if !strings.Contains(outGC, "No orphaned Bazel output bases found") || !strings.Contains(outGC, "Next:   gh wt list") {
		t.Errorf("unexpected wt gc output: %s", outGC)
	}
}

type failingHealthIDEDriver struct{}

func (failingHealthIDEDriver) SyncProject(*Project, string) error { return nil }
func (failingHealthIDEDriver) ArchiveProject(*Project) error      { return nil }
func (failingHealthIDEDriver) CheckHealth() (ChecklistItem, error) {
	return ChecklistItem{
		Category: "Jetski IDE",
		Status:   ChecklistError,
		Summary:  "broken IDE config",
	}, nil
}

func TestWTAudit_ReturnsErrorWhenUnhealthy(t *testing.T) {
	mgr, _, _, _ := setupTestManager(t, 2)
	mgr.IDEDriver = failingHealthIDEDriver{}

	outText, err := execWTCommand(t, mgr, "audit")
	if err == nil {
		t.Fatalf("expected wt audit to return non-zero error when checklist has errors, got nil\nOutput:\n%s", outText)
	}
	if !strings.Contains(outText, "Verdict: unhealthy (1 errors") {
		t.Errorf("expected unhealthy verdict in wt audit output, got:\n%s", outText)
	}

	outJSON, errJSON := execWTCommand(t, mgr, "audit", "--json")
	if errJSON == nil {
		t.Fatalf("expected wt audit --json to return non-zero error when checklist has errors, got nil\nOutput:\n%s", outJSON)
	}
	var payload map[string]any
	if uErr := json.Unmarshal([]byte(outJSON), &payload); uErr != nil {
		t.Fatalf("expected valid JSON output even on unhealthy audit, got error %v\nRaw:\n%s", uErr, outJSON)
	}
}

func TestWTList_Compact4ColumnTableUnder100Cols(t *testing.T) {
	mgr, mockGit, mockGerrit, _ := setupTestManager(t, 4)

	// Mount 4 projects covering different states:
	// 1. merged-proj: CL_MERGED
	resMerged, err := mgr.Use("merged-proj", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use merged-proj failed: %v", err)
	}
	cidMerged := "I1111111111111111111111111111111111111111"
	mockGit.ChangeIDs[resMerged.SlotPath] = cidMerged
	mockGerrit.Statuses[cidMerged] = ChangeStatus{
		ChangeID: cidMerged,
		Number:   477945,
		Status:   "MERGED",
	}

	// 2. wip-proj: LOCAL_WIP (local commit, no CL uploaded)
	resWIP, err := mgr.Use("wip-proj", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use wip-proj failed: %v", err)
	}
	mockGit.AheadCounts[resWIP.SlotPath] = 1

	// 3. ready-proj: READY_TO_LAND
	resReady, err := mgr.Use("ready-proj", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use ready-proj failed: %v", err)
	}
	cidReady := "I2222222222222222222222222222222222222222"
	mockGit.ChangeIDs[resReady.SlotPath] = cidReady
	mockGerrit.Statuses[cidReady] = ChangeStatus{
		ChangeID:        cidReady,
		Number:          480001,
		Status:          "NEW",
		CodeReviewScore: 2,
	}

	// 4. parked-attn: PARKED project needing attention
	resParked, err := mgr.Use("parked-attn", "", "", LeaseModeWrite, "")
	if err != nil {
		t.Fatalf("Use parked-attn failed: %v", err)
	}
	cidParked := "I3333333333333333333333333333333333333333"
	mockGit.ChangeIDs[resParked.SlotPath] = cidParked
	mockGerrit.Statuses[cidParked] = ChangeStatus{
		ChangeID:          cidParked,
		Number:            480002,
		Status:            "NEW",
		CodeReviewScore:   -1,
		UnresolvedThreads: 2,
	}
	if err := mgr.Park("parked-attn", false); err != nil {
		t.Fatalf("Park parked-attn failed: %v", err)
	}

	outList, err := execWTCommand(t, mgr, "list")
	if err != nil {
		t.Fatalf("wt list failed: %v", err)
	}

	for _, want := range []string{
		"PROJECT", "SLOT", "STATUS", "DETAILS",
		"pwrev/477945 (Merged) → ./gh wt next",
		"Local commits (no CL) → ./gh pr create",
		"pwrev/480001 (CR+2, CQ ready) → ./gh pr merge --cq",
		"pwrev/480002 (CR:-1, 2 threads) → ./gh wt use parked-attn",
	} {
		if !strings.Contains(outList, want) {
			t.Errorf("expected wt list output to contain %q, got:\n%s", want, outList)
		}
	}

	for lineNum, line := range strings.Split(outList, "\n") {
		cols := utf8.RuneCountInString(line)
		if cols >= 100 {
			t.Errorf("line %d exceeds 100 columns (%d cols): %q", lineNum+1, cols, line)
		}
	}
}
