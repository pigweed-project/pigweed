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
	"fmt"
	"os"
	"path/filepath"
)

// SafeModifyFile reads an existing file (or passes nil, false if and only if os.IsNotExist(err)),
// executes transformFn to compute the updated content, and atomically writes the result via
// a temporary file and rename.
//
// CRITICAL INVARIANT: Any non-ENOENT read error (e.g. EACCES, EIO, EISDIR, ELOOP) immediately aborts
// BEFORE transformFn is invoked, preventing accidental truncation or clobbering of user files.
func SafeModifyFile(path string, perm os.FileMode, transformFn func(existing []byte, exists bool) ([]byte, error)) error {
	existing, err := os.ReadFile(path)
	if err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("refusing to modify %s: failed to read existing file: %w", path, err)
	}
	exists := err == nil

	updated, err := transformFn(existing, exists)
	if err != nil {
		return err
	}

	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("failed to create parent directory %s: %w", dir, err)
	}

	tmpFile := path + ".tmp"
	f, err := os.OpenFile(tmpFile, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, perm)
	if err != nil {
		return fmt.Errorf("failed to create temporary file %s: %w", tmpFile, err)
	}
	if _, err := f.Write(updated); err != nil {
		_ = f.Close()
		_ = os.Remove(tmpFile)
		return fmt.Errorf("failed to write temporary file %s: %w", tmpFile, err)
	}
	if err := f.Sync(); err != nil {
		_ = f.Close()
		_ = os.Remove(tmpFile)
		return fmt.Errorf("failed to sync temporary file %s: %w", tmpFile, err)
	}
	if err := f.Close(); err != nil {
		_ = os.Remove(tmpFile)
		return fmt.Errorf("failed to close temporary file %s: %w", tmpFile, err)
	}

	if err := os.Rename(tmpFile, path); err != nil {
		_ = os.Remove(tmpFile)
		return fmt.Errorf("failed to atomically replace %s: %w", path, err)
	}
	return nil
}

// SafeModifyJSON reads an existing JSON file (or passes nil, false if os.IsNotExist(err)),
// unmarshals it, runs transformFn, and atomically writes the updated JSON.
//
// CRITICAL INVARIANT: If the file exists on disk but fails JSON unmarshaling, SafeModifyJSON
// aborts immediately and returns an error rather than overwriting the file with a default template.
func SafeModifyJSON(path string, perm os.FileMode, transformFn func(existing map[string]any, exists bool) (map[string]any, error)) error {
	return SafeModifyFile(path, perm, func(rawBytes []byte, exists bool) ([]byte, error) {
		var doc map[string]any
		if exists && len(rawBytes) > 0 {
			if err := json.Unmarshal(rawBytes, &doc); err != nil {
				return nil, fmt.Errorf("refusing to overwrite %s: existing file contains invalid JSON: %w", path, err)
			}
		}
		updatedDoc, err := transformFn(doc, exists)
		if err != nil {
			return nil, err
		}
		out, err := json.MarshalIndent(updatedDoc, "", "  ")
		if err != nil {
			return nil, fmt.Errorf("failed to serialize JSON for %s: %w", path, err)
		}
		return append(out, '\n'), nil
	})
}
