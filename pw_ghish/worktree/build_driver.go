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
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// ChecklistStatus represents the status icon of a preflight item.
type ChecklistStatus string

const (
	ChecklistOK       ChecklistStatus = "✓"
	ChecklistRepaired ChecklistStatus = "+"
	ChecklistWarning  ChecklistStatus = "!"
	ChecklistError    ChecklistStatus = "✗"
)

// ChecklistItem represents a single diagnostic line output by `gh wt init`.
type ChecklistItem struct {
	Category string          `json:"category"`
	Status   ChecklistStatus `json:"status"`
	Summary  string          `json:"summary"`
	Detail   string          `json:"detail,omitempty"`
}

// OrphanedOutputBase describes an unused Bazel output_base directory found in _bazel_$USER.
type OrphanedOutputBase struct {
	OutputBaseDir string `json:"output_base_dir"`
	WorkspacePath string `json:"workspace_path"`
	Reason        string `json:"reason"`
}

// GCReport summarizes the results of `gh wt gc`.
type GCReport struct {
	OrphansFound []OrphanedOutputBase `json:"orphans_found"`
	RemovedCount int                  `json:"removed_count"`
	DryRun       bool                 `json:"dry_run"`
	Warnings     []DiagnosticWarning  `json:"warnings,omitempty"`
}

// BuildEnvDriver abstracts build-system-specific cache configuration and garbage collection.
type BuildEnvDriver interface {
	CheckAndConfigure(fix bool, validWorkspacePaths map[string]bool) ([]ChecklistItem, error)
	GarbageCollect(dryRun bool, validWorkspacePaths map[string]bool) (GCReport, error)
}

// BazelDriver manages the non-invasive try-import .bazelrc snippet and _bazel_$USER output base GC.
type BazelDriver struct {
	HomeDir        string
	BazelCacheRoot string // e.g. ~/.cache/bazel/_bazel_$USER
}

// NewDefaultBazelDriver constructs a BazelDriver for the current user.
func NewDefaultBazelDriver() (*BazelDriver, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return nil, fmt.Errorf("failed to resolve home directory: %w", err)
	}
	user := os.Getenv("USER")
	if user == "" {
		user = filepath.Base(home)
	}
	return &BazelDriver{
		HomeDir:        home,
		BazelCacheRoot: filepath.Join(home, ".cache", "bazel", "_bazel_"+user),
	}, nil
}

func (b *BazelDriver) snippetPath() string {
	return filepath.Join(b.HomeDir, ".config", "pw_ghish", "bazelrc.worktrees")
}

func (b *BazelDriver) userBazelrcPath() string {
	return filepath.Join(b.HomeDir, ".bazelrc")
}

func (b *BazelDriver) repoCacheDir() string {
	return filepath.Join(b.HomeDir, ".cache", "bazel-repo-cache")
}

func (b *BazelDriver) diskCacheDir() string {
	return filepath.Join(b.HomeDir, ".cache", "bazel-disk-cache")
}

func (b *BazelDriver) expectedSnippetContent() string {
	return fmt.Sprintf(`# DO NOT EDIT: Automatically managed by 'gh wt init' (pw_ghish).
# To customize your personal Bazel settings, edit ~/.bazelrc directly.

# 1. Shared Repository Cache with Hardlinks
common --repository_cache=%s
common --experimental_repository_cache_hardlinks

# 2. Shared Action Disk Cache with Automatic Background Garbage Collection (Bazel 8+)
common --disk_cache=%s
common --experimental_disk_cache_gc_max_size=80G
common --experimental_disk_cache_gc_max_age=14d
common --experimental_disk_cache_gc_idle_delay=5m
`, b.repoCacheDir(), b.diskCacheDir())
}

func (b *BazelDriver) CheckAndConfigure(fix bool, validWorkspacePaths map[string]bool) ([]ChecklistItem, error) {
	var items []ChecklistItem

	// 1. Check/create cache directories
	for _, dir := range []string{b.repoCacheDir(), b.diskCacheDir()} {
		if _, err := os.Stat(dir); os.IsNotExist(err) && fix {
			if err := os.MkdirAll(dir, 0755); err != nil {
				return nil, fmt.Errorf("failed to create Bazel cache directory %s: %w", dir, err)
			}
		}
	}

	// 2. Check/generate bazelrc.worktrees snippet
	snippetFile := b.snippetPath()
	expectedSnippet := b.expectedSnippetContent()
	existingSnippet, err := os.ReadFile(snippetFile)
	if err == nil && strings.Contains(string(existingSnippet), "--experimental_repository_cache_hardlinks") &&
		strings.Contains(string(existingSnippet), "--experimental_disk_cache_gc_max_size=80G") {
		items = append(items, ChecklistItem{
			Category: "Bazel Config Snippet",
			Status:   ChecklistOK,
			Summary:  fmt.Sprintf("Active at %s (80 GB auto-GC disk cache + repo hardlinks)", snippetFile),
		})
	} else if fix {
		if err := os.MkdirAll(filepath.Dir(snippetFile), 0755); err != nil {
			return items, fmt.Errorf("failed to create snippet directory: %w", err)
		}
		if err := os.WriteFile(snippetFile, []byte(expectedSnippet), 0644); err != nil {
			return items, fmt.Errorf("failed to write %s: %w", snippetFile, err)
		}
		items = append(items, ChecklistItem{
			Category: "Bazel Config Snippet",
			Status:   ChecklistRepaired,
			Summary:  fmt.Sprintf("Generated %s (80 GB auto-GC cache + repo hardlinks)", snippetFile),
		})
	} else {
		items = append(items, ChecklistItem{
			Category: "Bazel Config Snippet",
			Status:   ChecklistWarning,
			Summary:  fmt.Sprintf("Missing or outdated %s (run `./gh wt init` to generate)", snippetFile),
		})
	}

	// 3. Check/append try-import line in ~/.bazelrc
	tryImportLine := fmt.Sprintf("try-import %s", snippetFile)
	userRc := b.userBazelrcPath()
	rcData, err := os.ReadFile(userRc)
	if err != nil && !os.IsNotExist(err) {
		return items, fmt.Errorf("refusing to modify %s: failed to read existing file: %w", userRc, err)
	}
	hasImport := err == nil && strings.Contains(string(rcData), tryImportLine)

	if hasImport {
		items = append(items, ChecklistItem{
			Category: "User ~/.bazelrc Hook",
			Status:   ChecklistOK,
			Summary:  fmt.Sprintf("`%s` present in %s", tryImportLine, userRc),
		})
	} else if fix {
		err := SafeModifyFile(userRc, 0644, func(existing []byte, exists bool) ([]byte, error) {
			if exists && strings.Contains(string(existing), tryImportLine) {
				return existing, nil
			}
			var newContent string
			if len(existing) > 0 {
				newContent = strings.TrimRight(string(existing), "\n") + "\n\n"
			}
			newContent += fmt.Sprintf("# Added by `gh wt init` (pw_ghish) - non-fatal if file is removed:\n%s\n", tryImportLine)
			return []byte(newContent), nil
		})
		if err != nil {
			return items, fmt.Errorf("failed to update %s: %w", userRc, err)
		}
		items = append(items, ChecklistItem{
			Category: "User ~/.bazelrc Hook",
			Status:   ChecklistRepaired,
			Summary:  fmt.Sprintf("Added `%s` to %s (preserved existing lines)", tryImportLine, userRc),
		})
	} else {
		items = append(items, ChecklistItem{
			Category: "User ~/.bazelrc Hook",
			Status:   ChecklistWarning,
			Summary:  fmt.Sprintf("`%s` missing from %s (run `./gh wt init` to add)", tryImportLine, userRc),
		})
	}

	// 4. Scan for orphaned output bases
	report, err := b.GarbageCollect(true, validWorkspacePaths)
	if err == nil {
		if len(report.OrphansFound) == 0 {
			items = append(items, ChecklistItem{
				Category: "Bazel Output Bases",
				Status:   ChecklistOK,
				Summary:  "0 orphaned output bases found in _bazel_$USER",
			})
		} else {
			var details []string
			for _, o := range report.OrphansFound {
				details = append(details, fmt.Sprintf("%s -> %s (%s)", filepath.Base(o.OutputBaseDir), o.WorkspacePath, o.Reason))
			}
			items = append(items, ChecklistItem{
				Category: "Bazel Output Bases",
				Status:   ChecklistWarning,
				Summary:  fmt.Sprintf("%d orphaned output base(s) found in %s", len(report.OrphansFound), b.BazelCacheRoot),
				Detail:   fmt.Sprintf("Run `./gh wt gc` to clean up: %s", strings.Join(details, "; ")),
			})
		}
	}

	return items, nil
}

// GarbageCollect scans BazelCacheRoot for directories containing a README file with `WORKSPACE: <path>`.
// If <path> does not exist on disk or is not in validWorkspacePaths, it is identified as orphaned.
func (b *BazelDriver) GarbageCollect(dryRun bool, validWorkspacePaths map[string]bool) (GCReport, error) {
	var report GCReport
	report.DryRun = dryRun

	entries, err := os.ReadDir(b.BazelCacheRoot)
	if err != nil {
		if os.IsNotExist(err) {
			return report, nil
		}
		return report, fmt.Errorf("failed to read Bazel cache directory %s: %w", b.BazelCacheRoot, err)
	}

	poolRoot := filepath.Join(b.HomeDir, "wrk", "slots")

	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		dirPath := filepath.Join(b.BazelCacheRoot, entry.Name())
		wsPath, err := parseWorkspaceFromOutputBase(dirPath)
		if err != nil || wsPath == "" {
			continue // Not a workspace output_base directory (e.g. "cache" or "install")
		}

		// An output_base is only orphaned if:
		// 1. Its workspace directory has been deleted from disk, OR
		// 2. Its workspace directory is inside ~/wrk/slots/ and is no longer an active slot.
		// Live directories outside ~/wrk/slots/ (e.g. ~/wrk/pigweed, ~/wrk/pw-wt) are NEVER touched.
		reason := ""
		if _, statErr := os.Stat(wsPath); os.IsNotExist(statErr) {
			reason = "workspace directory deleted from disk"
		} else if len(validWorkspacePaths) > 0 && strings.HasPrefix(wsPath, poolRoot+string(filepath.Separator)) {
			realWs, err := filepath.EvalSymlinks(wsPath)
			if err != nil {
				realWs = wsPath
			}
			if !validWorkspacePaths[wsPath] && !validWorkspacePaths[realWs] {
				reason = "slot retired from worktree pool"
			}
		}

		if reason != "" {
			orphan := OrphanedOutputBase{
				OutputBaseDir: dirPath,
				WorkspacePath: wsPath,
				Reason:        reason,
			}
			report.OrphansFound = append(report.OrphansFound, orphan)
			if !dryRun {
				// Bazel marks execroot/ and external/ directories read-only (0555).
				// Restore write permissions recursively before RemoveAll so unlink succeeds.
				_ = filepath.WalkDir(dirPath, func(p string, d os.DirEntry, walkErr error) error {
					if walkErr == nil && d.IsDir() {
						_ = os.Chmod(p, 0755)
					}
					return nil
				})
				if err := os.RemoveAll(dirPath); err != nil {
					report.Warnings = append(report.Warnings, DiagnosticWarning{
						Subsystem:   "Bazel GC",
						Message:     fmt.Sprintf("Failed to delete orphaned output base %s: %v", dirPath, err),
						Remediation: fmt.Sprintf("Run `chmod -R u+w %s && rm -rf %s`", dirPath, dirPath),
					})
				} else {
					report.RemovedCount++
				}
			}
		}
	}
	return report, nil
}

func parseWorkspaceFromOutputBase(outputBaseDir string) (string, error) {
	// 1. Check DO_NOT_BUILD_HERE (standard single-line workspace path marker)
	dnbhPath := filepath.Join(outputBaseDir, "DO_NOT_BUILD_HERE")
	if data, err := os.ReadFile(dnbhPath); err == nil {
		if ws := strings.TrimSpace(string(data)); ws != "" {
			return ws, nil
		}
	}

	// 2. Fallback to README ("WORKSPACE: /path/to/workspace")
	readmePath := filepath.Join(outputBaseDir, "README")
	f, err := os.Open(readmePath)
	if err != nil {
		return "", err
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "WORKSPACE:") {
			return strings.TrimSpace(strings.TrimPrefix(line, "WORKSPACE:")), nil
		}
	}
	return "", scanner.Err()
}
