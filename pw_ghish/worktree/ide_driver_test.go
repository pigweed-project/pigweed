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
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestJetskiIDEDriver_CloneAndPatchPreservesUnknownFields(t *testing.T) {
	projectsDir := t.TempDir()

	// Create a mock base "pigweed" project JSON containing custom grants and unknown future fields
	basePigweedJSON := `{
  "id": "base-pigweed-uuid",
  "name": "pigweed",
  "projectResources": {
    "resources": [
      {
        "gitFolder": {
          "folderUri": "file:///usr/local/google/home/keir/wrk/pigweed"
        }
      }
    ]
  },
  "permissionGrants": {
    "permissionGrants": {
      "allow": [
        "command(/custom/allowlisted/tool)"
      ]
    },
    "v2Migrated": true
  },
  "futureJetskiSchemaFieldV3": {
    "someNewFlag": true
  }
}`
	if err := os.WriteFile(filepath.Join(projectsDir, "base-pigweed-uuid.json"), []byte(basePigweedJSON), 0644); err != nil {
		t.Fatalf("failed to write base template: %v", err)
	}

	driver := &JetskiIDEDriver{
		ProjectsDir: projectsDir,
	}

	proj := &Project{
		Name:      "sensor-dma",
		Residency: ResidencyMounted,
	}
	symlinkPath := "/usr/local/google/home/keir/wrk/projects/sensor-dma"

	if err := driver.SyncProject(proj, symlinkPath); err != nil {
		t.Fatalf("SyncProject failed: %v", err)
	}

	if proj.JetskiProjectUUID == "" {
		t.Fatalf("expected deterministic UUID to be assigned")
	}

	generatedFile := filepath.Join(projectsDir, proj.JetskiProjectUUID+".json")
	data, err := os.ReadFile(generatedFile)
	if err != nil {
		t.Fatalf("failed to read generated project file: %v", err)
	}

	var parsed map[string]any
	if err := json.Unmarshal(data, &parsed); err != nil {
		t.Fatalf("failed to unmarshal generated JSON: %v", err)
	}

	if parsed["name"] != "pw: sensor-dma" {
		t.Errorf("expected name 'pw: sensor-dma', got %v", parsed["name"])
	}
	if parsed["archived"] != false {
		t.Errorf("expected archived=false, got %v", parsed["archived"])
	}

	// Verify unknown future field was preserved by Clone-and-Patch!
	futureField, ok := parsed["futureJetskiSchemaFieldV3"].(map[string]any)
	if !ok || futureField["someNewFlag"] != true {
		t.Errorf("expected unknown future schema field to be preserved, got: %v", parsed["futureJetskiSchemaFieldV3"])
	}

	// Verify ArchiveProject sets archived: true
	if err := driver.ArchiveProject(proj); err != nil {
		t.Fatalf("ArchiveProject failed: %v", err)
	}
	dataArch, _ := os.ReadFile(generatedFile)
	var parsedArch map[string]any
	_ = json.Unmarshal(dataArch, &parsedArch)
	if parsedArch["archived"] != true {
		t.Errorf("expected archived=true after ArchiveProject, got %v", parsedArch["archived"])
	}
}

func TestJetskiIDEDriver_SchemaDriftCanaryCircuitBreaker(t *testing.T) {
	projectsDir := t.TempDir()
	driver := &JetskiIDEDriver{
		ProjectsDir: projectsDir,
	}

	// Simulate Jetski rejecting a generated file and rewriting its name to "Recovered Project"
	corruptUUID := DeterministicProjectUUID("bad-proj")
	recoveredJSON := `{
  "id": "` + corruptUUID + `",
  "name": "Recovered Project"
}`
	if err := os.WriteFile(filepath.Join(projectsDir, "pw-"+corruptUUID+".json"), []byte(recoveredJSON), 0644); err != nil {
		t.Fatalf("failed to write recovered file: %v", err)
	}

	item, err := driver.CheckHealth()
	if err != nil {
		t.Fatalf("CheckHealth returned error: %v", err)
	}
	if item.Status != ChecklistWarning {
		t.Errorf("expected ChecklistWarning when canary detects 'Recovered Project', got %s", item.Status)
	}
	if !driver.circuitBreakerTripped {
		t.Errorf("expected circuitBreakerTripped=true after canary detection")
	}

	// Verify subsequent SyncProject calls are safely no-ops instead of failing or writing more bad files
	newProj := &Project{Name: "another-proj"}
	if err := driver.SyncProject(newProj, "/some/path"); err != nil {
		t.Errorf("expected SyncProject to succeed gracefully as no-op when circuit breaker is tripped, got: %v", err)
	}
}
