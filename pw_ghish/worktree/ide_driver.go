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
	"crypto/sha1"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// IDEIntegrationDriver abstracts IDE/agent harness project registration and archival.
type IDEIntegrationDriver interface {
	CheckHealth() (ChecklistItem, error)
	SyncProject(project *Project, symlinkPath string) error
	ArchiveProject(project *Project) error
}

// NoopIDEDriver does nothing on non-Jetski hosts or when IDE sync is disabled.
type NoopIDEDriver struct{}

func (NoopIDEDriver) CheckHealth() (ChecklistItem, error) {
	return ChecklistItem{
		Category: "IDE Sync Plugin",
		Status:   ChecklistOK,
		Summary:  "Disabled / Non-Jetski environment (POSIX symlinks active)",
	}, nil
}

func (NoopIDEDriver) SyncProject(project *Project, symlinkPath string) error { return nil }
func (NoopIDEDriver) ArchiveProject(project *Project) error                  { return nil }

// DeterministicProjectUUID computes a stable UUID v5 from projectName.
func DeterministicProjectUUID(projectName string) string {
	h := sha1.New()
	h.Write([]byte("pw_ghish:project:" + strings.ToLower(strings.TrimSpace(projectName))))
	sum := h.Sum(nil)
	sum[6] = (sum[6] & 0x0f) | 0x50 // Version 5
	sum[8] = (sum[8] & 0x3f) | 0x80 // Variant RFC4122
	return fmt.Sprintf("%x-%x-%x-%x-%x", sum[0:4], sum[4:6], sum[6:8], sum[8:10], sum[10:16])
}

// JetskiIDEDriver manages live fsnotify project files in ~/.gemini/config/projects/.
type JetskiIDEDriver struct {
	ProjectsDir           string
	Disabled              bool
	circuitBreakerTripped bool
	driftReason           string
}

// NewDefaultJetskiIDEDriver returns a JetskiIDEDriver pointing to the active Antigravity/Jetski
// projects directory (~/.gemini/config/projects or ~/.antigravity/config/projects),
// or a disabled driver if no projects config directory exists on the host.
func NewDefaultJetskiIDEDriver() *JetskiIDEDriver {
	if os.Getenv("GH_ISH_IDE_SYNC") == "0" || os.Getenv("GH_ISH_IDE_SYNC") == "off" {
		return &JetskiIDEDriver{Disabled: true}
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return &JetskiIDEDriver{Disabled: true}
	}
	candidates := []string{
		filepath.Join(home, ".gemini", "config", "projects"),
		filepath.Join(home, ".antigravity", "config", "projects"),
		filepath.Join(home, ".config", "antigravity", "projects"),
	}
	for _, dir := range candidates {
		if info, err := os.Stat(dir); err == nil && info.IsDir() {
			return &JetskiIDEDriver{ProjectsDir: dir}
		}
	}
	return &JetskiIDEDriver{ProjectsDir: candidates[0], Disabled: true}
}

func (j *JetskiIDEDriver) CheckHealth() (ChecklistItem, error) {
	if j.Disabled || j.ProjectsDir == "" {
		return ChecklistItem{
			Category: "Antigravity UI Sync",
			Status:   ChecklistOK,
			Summary:  "Skipped (IDE projects directory not present)",
		}, nil
	}

	// Scan existing project JSON files for schema drift canaries or template availability.
	entries, err := os.ReadDir(j.ProjectsDir)
	if err != nil {
		return ChecklistItem{
			Category: "Antigravity UI Sync",
			Status:   ChecklistWarning,
			Summary:  fmt.Sprintf("Could not read %s: %v", j.ProjectsDir, err),
		}, nil
	}

	activeCount := 0
	archivedCount := 0
	hasTemplate := false

	for _, e := range entries {
		if e.IsDir() || filepath.Ext(e.Name()) != ".json" {
			continue
		}
		filePath := filepath.Join(j.ProjectsDir, e.Name())
		data, err := os.ReadFile(filePath)
		if err != nil {
			continue
		}
		var raw map[string]any
		if err := json.Unmarshal(data, &raw); err != nil {
			continue
		}
		name, _ := raw["name"].(string)
		// Canary check: Antigravity language server rewrites corrupt/unrecognized schemas as "Recovered Project"
		if name == "Recovered Project" && strings.HasPrefix(e.Name(), "pw-") {
			j.circuitBreakerTripped = true
			j.driftReason = fmt.Sprintf("IDE rejected %s as 'Recovered Project' (schema drift detected)", e.Name())
			return ChecklistItem{
				Category: "Antigravity UI Sync",
				Status:   ChecklistWarning,
				Summary:  "Schema divergence detected! Circuit breaker tripped (falling back to POSIX symlinks)",
				Detail:   j.driftReason,
			}, nil
		}
		if _, ok := raw["projectResources"]; ok {
			hasTemplate = true
		}
		if strings.HasPrefix(name, "pw: ") {
			if arch, ok := raw["archived"].(bool); ok && arch {
				archivedCount++
			} else {
				activeCount++
			}
		}
	}

	statusSummary := fmt.Sprintf("Verified (%d mounted & %d parked/archived IDE projects synced)", activeCount, archivedCount)
	if !hasTemplate {
		statusSummary = "Ready (using default v2 permission schema template)"
	}
	return ChecklistItem{
		Category: "Antigravity UI Sync",
		Status:   ChecklistOK,
		Summary:  statusSummary,
	}, nil
}

// sanitizeDonorTemplate copies configuration and future schema fields from a donor project JSON
// while stripping identity and session-specific UI state keys.
func sanitizeDonorTemplate(donor map[string]any) map[string]any {
	clean := make(map[string]any)
	denylist := map[string]bool{
		"name":                     true,
		"projectResources":         true,
		"archived":                 true,
		"id":                       true,
		"uuid":                     true,
		"lastOpenedConversationId": true,
		"windowBounds":             true,
		"activeConversationIds":    true,
		"recentFiles":              true,
		"openTabs":                 true,
		"history":                  true,
	}
	for k, v := range donor {
		if !denylist[k] {
			clean[k] = v
		}
	}
	return clean
}

// loadBaseTemplate finds an existing healthy project JSON (preferring "pigweed") to Clone-and-Patch.
func (j *JetskiIDEDriver) loadBaseTemplate() map[string]any {
	entries, err := os.ReadDir(j.ProjectsDir)
	if err == nil {
		var fallback map[string]any
		for _, e := range entries {
			if e.IsDir() || filepath.Ext(e.Name()) != ".json" {
				continue
			}
			data, err := os.ReadFile(filepath.Join(j.ProjectsDir, e.Name()))
			if err != nil {
				continue
			}
			var raw map[string]any
			if err := json.Unmarshal(data, &raw); err != nil {
				continue
			}
			name, _ := raw["name"].(string)
			if name == "Recovered Project" {
				continue
			}
			if _, hasRes := raw["projectResources"]; !hasRes {
				continue
			}
			if strings.EqualFold(name, "pigweed") {
				return sanitizeDonorTemplate(raw)
			}
			if fallback == nil {
				fallback = raw
			}
		}
		if fallback != nil {
			return sanitizeDonorTemplate(fallback)
		}
	}

	// Default v2 schema template if no existing project JSON is available to clone
	return map[string]any{
		"permissionGrants": map[string]any{
			"permissionGrants": map[string]any{
				"allow": []string{
					"command(bazelisk)",
					"command(./gh)",
					"command(./pw)",
				},
			},
			"v2Migrated": true,
		},
	}
}

func (j *JetskiIDEDriver) SyncProject(project *Project, symlinkPath string) error {
	if j.Disabled || j.circuitBreakerTripped || j.ProjectsDir == "" {
		return nil
	}
	if project.JetskiProjectUUID == "" {
		project.JetskiProjectUUID = DeterministicProjectUUID(project.Name)
	}

	targetFile := filepath.Join(j.ProjectsDir, project.JetskiProjectUUID+".json")

	// Check for Recovered Project circuit breaker first if file exists and is valid JSON
	if existingData, err := os.ReadFile(targetFile); err == nil {
		var checkDoc map[string]any
		if json.Unmarshal(existingData, &checkDoc) == nil {
			if name, _ := checkDoc["name"].(string); name == "Recovered Project" {
				j.circuitBreakerTripped = true
				if rmErr := os.Remove(targetFile); rmErr != nil && !os.IsNotExist(rmErr) {
					return fmt.Errorf("schema canary tripped; failed to remove corrupt target %s: %w", targetFile, rmErr)
				}
				return nil
			}
		}
	}

	return SafeModifyJSON(targetFile, 0644, func(existing map[string]any, exists bool) (map[string]any, error) {
		payload := existing
		if !exists || payload == nil {
			payload = j.loadBaseTemplate()
		}

		// Patch only the 4 required fields + ensure TURBO/EAGER permissions
		payload["id"] = project.JetskiProjectUUID
		if project.IssueID > 0 {
			payload["name"] = fmt.Sprintf("pw: b/%d - %s", project.IssueID, project.Name)
		} else {
			payload["name"] = "pw: " + project.Name
		}
		payload["archived"] = false
		payload["projectResources"] = map[string]any{
			"resources": []any{
				map[string]any{
					"gitFolder": map[string]any{
						"folderUri":     "file://" + symlinkPath,
						"defaultBranch": "main",
						"allowWrite":    true,
					},
				},
			},
		}

		// Ensure optimal agent execution settings without wiping existing securityPlugins
		settings, ok := payload["settings"].(map[string]any)
		if !ok {
			settings = make(map[string]any)
		}
		settings["fileAccessPolicy"] = "AGENT_SETTING_POLICY_ALLOW"
		settings["sandboxMode"] = false
		settings["autoExecutionPolicy"] = "CASCADE_COMMANDS_AUTO_EXECUTION_EAGER"
		settings["permissionPreset"] = "AGENT_PERMISSION_PRESET_TURBO"
		payload["settings"] = settings

		return payload, nil
	})
}

func (j *JetskiIDEDriver) ArchiveProject(project *Project) error {
	if j.Disabled || j.circuitBreakerTripped || j.ProjectsDir == "" {
		return nil
	}
	uuid := project.JetskiProjectUUID
	if uuid == "" {
		uuid = DeterministicProjectUUID(project.Name)
	}
	targetFile := filepath.Join(j.ProjectsDir, uuid+".json")
	if _, err := os.Stat(targetFile); os.IsNotExist(err) {
		return nil
	}
	return SafeModifyJSON(targetFile, 0644, func(existing map[string]any, exists bool) (map[string]any, error) {
		if !exists || existing == nil {
			return nil, fmt.Errorf("project file %s disappeared during archive", targetFile)
		}
		existing["archived"] = true
		return existing, nil
	})
}

func (j *JetskiIDEDriver) atomicWriteJSON(targetFile string, payload map[string]any) error {
	data, err := json.MarshalIndent(payload, "", "  ")
	if err != nil {
		return fmt.Errorf("failed to serialize Jetski project JSON: %w", err)
	}
	tmpFile := targetFile + ".tmp"
	if err := os.WriteFile(tmpFile, data, 0644); err != nil {
		return fmt.Errorf("failed to write temporary Jetski project file %s: %w", tmpFile, err)
	}
	if err := os.Rename(tmpFile, targetFile); err != nil {
		return fmt.Errorf("failed to atomically update Jetski project file %s: %w", targetFile, err)
	}
	return nil
}
