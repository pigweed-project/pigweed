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

	// 1. `wt init --check` (human readable & JSON)
	outInit, err := execWTCommand(t, mgr, "init", "--check")
	if err != nil {
		t.Fatalf("wt init --check failed: %v\nOutput: %s", err, outInit)
	}
	if !strings.Contains(outInit, "Inspecting gh-ish Worktree Environment") {
		t.Errorf("expected init banner in output, got:\n%s", outInit)
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

	// 2. `wt use <project> --json`
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
	if !strings.Contains(outNext, "Rebased project") {
		t.Errorf("unexpected wt next output: %s", outNext)
	}

	// 6. `wt park <project>`
	outPark, err := execWTCommand(t, mgr, "park", "cli-test-proj")
	if err != nil {
		t.Fatalf("wt park failed: %v", err)
	}
	if !strings.Contains(outPark, "Parked project") {
		t.Errorf("unexpected wt park output: %s", outPark)
	}

	// 7. `wt close <project>`
	outClose, err := execWTCommand(t, mgr, "close", "cli-test-proj")
	if err != nil {
		t.Fatalf("wt close failed: %v", err)
	}
	if !strings.Contains(outClose, "Closed project") {
		t.Errorf("unexpected wt close output: %s", outClose)
	}

	// 8. `wt gc --dry-run`
	outGC, err := execWTCommand(t, mgr, "gc", "--dry-run")
	if err != nil {
		t.Fatalf("wt gc failed: %v", err)
	}
	if !strings.Contains(outGC, "No orphaned Bazel output bases found") {
		t.Errorf("unexpected wt gc output: %s", outGC)
	}
}
