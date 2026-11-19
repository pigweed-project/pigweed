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
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestGetenvWithName_CanonicalAndLegacyFallback(t *testing.T) {
	t.Setenv("GHISH_AUTH_MODE", "")
	// TODO: b/586589869 - Remove legacy GH_ISH_AUTH_MODE fallback assertions after rollout.
	t.Setenv("GH_ISH_AUTH_MODE", "")

	if v, name := GetenvWithName("GHISH_AUTH_MODE"); v != "" || name != "" {
		t.Errorf("unset: got (%q, %q), want (\"\", \"\")", v, name)
	}

	t.Setenv("GH_ISH_AUTH_MODE", "community")
	if v, name := GetenvWithName("GHISH_AUTH_MODE"); v != "community" || name != "GH_ISH_AUTH_MODE" {
		t.Errorf("legacy fallback: got (%q, %q), want (\"community\", \"GH_ISH_AUTH_MODE\")", v, name)
	}

	t.Setenv("GHISH_AUTH_MODE", "googler")
	if v, name := GetenvWithName("GHISH_AUTH_MODE"); v != "googler" || name != "GHISH_AUTH_MODE" {
		t.Errorf("canonical precedence: got (%q, %q), want (\"googler\", \"GHISH_AUTH_MODE\")", v, name)
	}
}

func TestResolveUserConfigDirIn_MigrationAndFallback(t *testing.T) {
	t.Run("fresh defaults to ghish", func(t *testing.T) {
		base := t.TempDir()
		want := filepath.Join(base, "ghish")
		if got := ResolveUserConfigDirIn(base, true); got != want {
			t.Errorf("got %q, want %q", got, want)
		}
	})

	// TODO: b/586589869 - Remove legacy pw_ghish config directory migration tests after rollout.
	t.Run("read-only preserves legacy dir without mutating", func(t *testing.T) {
		base := t.TempDir()
		legacy := filepath.Join(base, "pw_ghish")
		if err := os.MkdirAll(legacy, 0o755); err != nil {
			t.Fatalf("MkdirAll: %v", err)
		}
		if got := ResolveUserConfigDirIn(base, false); got != legacy {
			t.Errorf("got %q, want %q", got, legacy)
		}
		if _, err := os.Lstat(filepath.Join(base, "ghish")); !os.IsNotExist(err) {
			t.Errorf("read-only call unexpectedly created canonical dir (err=%v)", err)
		}
	})

	t.Run("rejects empty and relative baseConfigDir", func(t *testing.T) {
		if got := ResolveUserConfigDirIn("", true); got != "" {
			t.Errorf("empty baseConfigDir: got %q, want \"\"", got)
		}
		if got := ResolveUserConfigDirIn("relative/config", true); got != "" {
			t.Errorf("relative baseConfigDir: got %q, want \"\"", got)
		}
	})

	t.Run("migrate renames pw_ghish to ghish and symlinks legacy", func(t *testing.T) {
		base := t.TempDir()
		legacy := filepath.Join(base, "pw_ghish")
		if err := os.MkdirAll(legacy, 0o755); err != nil {
			t.Fatalf("MkdirAll: %v", err)
		}
		if err := os.WriteFile(filepath.Join(legacy, "worktrees.json"), []byte(`{"v":1}`), 0o644); err != nil {
			t.Fatalf("WriteFile: %v", err)
		}

		canonical := filepath.Join(base, "ghish")
		got := ResolveUserConfigDirIn(base, true)
		if runtime.GOOS == "windows" {
			if got != legacy {
				t.Fatalf("windows: got %q, want legacy %q preserved", got, legacy)
			}
			return
		}
		if got != canonical {
			t.Fatalf("got %q, want %q", got, canonical)
		}
		data, err := os.ReadFile(filepath.Join(canonical, "worktrees.json"))
		if err != nil || string(data) != `{"v":1}` {
			t.Fatalf("canonical worktrees.json = (%q, %v), want preserved payload", string(data), err)
		}
		target, err := os.Readlink(legacy)
		if err != nil || target != "ghish" {
			t.Errorf("Readlink(%s) = (%q, %v), want (\"ghish\", nil)", legacy, target, err)
		}
		// Reading through legacy path resolves to the exact same file.
		viaLegacy, err := os.ReadFile(filepath.Join(legacy, "worktrees.json"))
		if err != nil || string(viaLegacy) != `{"v":1}` {
			t.Errorf("legacy symlink read = (%q, %v), want preserved payload", string(viaLegacy), err)
		}
	})

	t.Run("custom XDG_CONFIG_HOME preserves and migrates ~/.config/pw_ghish", func(t *testing.T) {
		home := t.TempDir()
		xdg := filepath.Join(t.TempDir(), "custom-xdg")
		homeLegacy := filepath.Join(home, ".config", "pw_ghish")
		if err := os.MkdirAll(homeLegacy, 0o755); err != nil {
			t.Fatalf("MkdirAll: %v", err)
		}
		if err := os.WriteFile(filepath.Join(homeLegacy, "worktrees.json"), []byte(`{"v":2}`), 0o644); err != nil {
			t.Fatalf("WriteFile: %v", err)
		}

		// Read-only falls back to ~/.config/pw_ghish without mutating disk.
		if got := ResolveUserConfigDirWithFallback(xdg, filepath.Join(home, ".config"), false); got != homeLegacy {
			t.Fatalf("read-only custom XDG: got %q, want %q", got, homeLegacy)
		}
		if _, err := os.Lstat(filepath.Join(xdg, "ghish")); !os.IsNotExist(err) {
			t.Fatalf("read-only custom XDG unexpectedly created %s", filepath.Join(xdg, "ghish"))
		}

		canonical := filepath.Join(xdg, "ghish")
		got := ResolveUserConfigDirWithFallback(xdg, filepath.Join(home, ".config"), true)
		if runtime.GOOS == "windows" {
			if got != homeLegacy {
				t.Fatalf("windows custom XDG: got %q, want %q", got, homeLegacy)
			}
			return
		}
		if got != canonical {
			t.Fatalf("migrated custom XDG: got %q, want %q", got, canonical)
		}
		if data, err := os.ReadFile(filepath.Join(canonical, "worktrees.json")); err != nil || string(data) != `{"v":2}` {
			t.Errorf("canonical payload = (%q, %v), want {\"v\":2}", string(data), err)
		}
		if target, err := os.Readlink(homeLegacy); err != nil || target != canonical {
			t.Errorf("Readlink(%s) = (%q, %v), want (%q, nil)", homeLegacy, target, err, canonical)
		}
	})
}

func TestBaseConfigDir_HonorsXDGConfigHome(t *testing.T) {
	home, err := os.UserHomeDir()
	if err != nil {
		t.Skip("os.UserHomeDir unavailable")
	}
	wantXDG := filepath.Join(t.TempDir(), "xdg")
	t.Setenv("XDG_CONFIG_HOME", wantXDG)
	if got := BaseConfigDir(home); got != wantXDG {
		t.Errorf("BaseConfigDir(realHome) = %q, want %q", got, wantXDG)
	}
	otherHome := t.TempDir()
	if got := BaseConfigDir(otherHome); got != filepath.Join(otherHome, ".config") {
		t.Errorf("BaseConfigDir(otherHome) = %q, want %q", got, filepath.Join(otherHome, ".config"))
	}
	t.Setenv("XDG_CONFIG_HOME", "relative/xdg")
	if got := BaseConfigDir(home); got != filepath.Join(home, ".config") {
		t.Errorf("BaseConfigDir(relative XDG_CONFIG_HOME) = %q, want %q", got, filepath.Join(home, ".config"))
	}
}

func TestAgentFlag(t *testing.T) {
	tests := []struct {
		in        string
		wantAgent any // marker under an agent: true, false, or nil (unset)
		wantHuman any
	}{
		{in: "auto", wantAgent: true, wantHuman: nil},
		{in: "true", wantAgent: true, wantHuman: true},
		{in: "1", wantAgent: true, wantHuman: true},
		{in: "false", wantAgent: false, wantHuman: false},
	}
	deref := func(b *bool) any {
		if b == nil {
			return nil
		}
		return *b
	}
	for _, tc := range tests {
		var f agentFlag
		if err := f.Set(tc.in); err != nil {
			t.Fatalf("Set(%q): %v", tc.in, err)
		}
		t.Run(tc.in+" under agent", func(t *testing.T) {
			t.Setenv("ANTIGRAVITY_AGENT", "1")
			if got := deref(f.isAI()); got != tc.wantAgent {
				t.Errorf("got %v, want %v", got, tc.wantAgent)
			}
		})
		t.Run(tc.in+" under human", func(t *testing.T) {
			if got := deref(f.isAI()); got != tc.wantHuman {
				t.Errorf("got %v, want %v", got, tc.wantHuman)
			}
		})
	}
	var f agentFlag
	if err := f.Set("banana"); err == nil {
		t.Error("Set(banana) succeeded, want error")
	}
}
