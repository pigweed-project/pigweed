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
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
)

// legacyEnvFallbacks maps canonical GHISH_* (and internal _GHISH_*) environment
// variables to their legacy pre-rename names during the soft transition.
//
// TODO: b/586589869 - Remove legacyEnvFallbacks, Getenv, and GetenvWithName
// after the naming rollout completes.
var legacyEnvFallbacks = map[string]string{
	"GHISH_AUTH_MODE":      "GH_ISH_AUTH_MODE",
	"GHISH_ACTIVE":         "GH_ISH_ACTIVE",
	"GHISH_ALLOW_RAW_PUSH": "GH_ISH_ALLOW_RAW_PUSH",
	"GHISH_INVOKED_AS":     "GH_ISH_INVOKED_AS",
	"GHISH_ONCALL_FILE":    "GH_ISH_ONCALL_FILE",
	"GHISH_IDE_SYNC":       "GH_ISH_IDE_SYNC",
	"_GHISH_SCRIPT_DIR":    "_PW_GH_SCRIPT_DIR",
}

// Getenv returns the value of key from the environment, falling back to any
// legacy pre-rename variable when key is unset or whitespace-only.
func Getenv(key string) string {
	val, _ := GetenvWithName(key)
	return val
}

// GetenvWithName returns the value of key and the environment variable name
// that supplied it, falling back to any legacy pre-rename variable when key is
// unset or whitespace-only.
func GetenvWithName(key string) (string, string) {
	if v := os.Getenv(key); strings.TrimSpace(v) != "" {
		return v, key
	}
	// TODO: b/586589869 - Remove legacy environment variable fallback after rollout.
	if legacy, ok := legacyEnvFallbacks[key]; ok {
		if v := os.Getenv(legacy); strings.TrimSpace(v) != "" {
			slog.Debug("using deprecated environment variable; switch to canonical name", "legacy", legacy, "canonical", key)
			return v, legacy
		}
	}
	return "", ""
}

const (
	canonicalConfigDirName = "ghish"
	// TODO: b/586589869 - Remove legacy config directory fallback after rollout.
	legacyConfigDirName = "pw_ghish"
)

// BaseConfigDir returns the XDG config parent directory for homeDir. It honors
// XDG_CONFIG_HOME when homeDir matches os.UserHomeDir (or is empty), and falls
// back to filepath.Join(homeDir, ".config").
func BaseConfigDir(homeDir string) string {
	actualHome, _ := os.UserHomeDir()
	if homeDir == "" {
		homeDir = actualHome
	}
	if xdg := strings.TrimSpace(os.Getenv("XDG_CONFIG_HOME")); xdg != "" && filepath.IsAbs(xdg) && (homeDir == "" || homeDir == actualHome) {
		return xdg
	}
	if homeDir == "" {
		return ""
	}
	return filepath.Join(homeDir, ".config")
}

// ResolveUserConfigDirIn resolves the ghish user configuration directory inside
// baseConfigDir (e.g. ~/.config/ghish). See ResolveUserConfigDirWithFallback
// for migration and legacy fallback semantics.
func ResolveUserConfigDirIn(baseConfigDir string, migrate bool) string {
	return ResolveUserConfigDirWithFallback(baseConfigDir, "", migrate)
}

// ResolveUserConfigDir resolves the ghish user configuration directory for
// homeDir, honoring XDG_CONFIG_HOME while preserving any pre-existing legacy
// directory under homeDir/.config when XDG_CONFIG_HOME points elsewhere.
func ResolveUserConfigDir(homeDir string, migrate bool) string {
	if homeDir == "" {
		homeDir, _ = os.UserHomeDir()
	}
	base := BaseConfigDir(homeDir)
	homeConfigDir := ""
	if homeDir != "" && filepath.IsAbs(homeDir) {
		homeConfigDir = filepath.Join(homeDir, ".config")
	}
	return ResolveUserConfigDirWithFallback(base, homeConfigDir, migrate)
}

// ResolveUserConfigDirWithFallback resolves the ghish user configuration
// directory inside baseConfigDir (e.g. ~/.config/ghish). Returns "" if
// baseConfigDir is empty or relative.
//
// When migrate is true on non-Windows platforms and canonical ghish/ does not
// exist while a legacy directory exists in baseConfigDir (or fallbackLegacyBase
// when XDG_CONFIG_HOME redirected away from ~/.config), it atomically renames
// the legacy directory to canonical ghish/ and creates a compatibility symlink
// at the legacy location so older binaries and hooks share the same files and
// flock inodes. On Windows (where unprivileged symlinks are unavailable), an
// existing legacy directory is kept in place rather than renamed away.
func ResolveUserConfigDirWithFallback(baseConfigDir, fallbackLegacyBase string, migrate bool) string {
	if baseConfigDir == "" || !filepath.IsAbs(baseConfigDir) {
		return ""
	}
	baseConfigDir = filepath.Clean(baseConfigDir)
	canonical := filepath.Join(baseConfigDir, canonicalConfigDirName)

	if _, err := os.Lstat(canonical); err == nil {
		return canonical
	}

	type legacyCandidate struct {
		path          string
		symlinkTarget string
	}
	// TODO: b/586589869 - Remove legacy pw_ghish directory migration/fallback after rollout.
	candidates := []legacyCandidate{
		{
			path:          filepath.Join(baseConfigDir, legacyConfigDirName),
			symlinkTarget: canonicalConfigDirName,
		},
	}
	if fallbackLegacyBase != "" && filepath.IsAbs(fallbackLegacyBase) {
		cleanFallback := filepath.Clean(fallbackLegacyBase)
		if cleanFallback != baseConfigDir {
			candidates = append(candidates, legacyCandidate{
				path:          filepath.Join(cleanFallback, legacyConfigDirName),
				symlinkTarget: canonical,
			})
		}
	}

	for _, cand := range candidates {
		legacyInfo, legacyErr := os.Lstat(cand.path)
		if legacyErr != nil || !legacyInfo.IsDir() {
			continue
		}
		// On Windows, skip renaming legacy away because unprivileged os.Symlink
		// is unavailable and renaming without a symlink breaks existing hooks
		// and splits worktrees.json across old and new binaries.
		if !migrate || runtime.GOOS == "windows" {
			return cand.path
		}
		if filepath.Dir(cand.path) != baseConfigDir {
			if err := os.MkdirAll(baseConfigDir, 0o755); err != nil {
				// OK: fallback keeps existing user config accessible if target parent cannot be created.
				slog.Debug("failed to create canonical config parent; continuing with legacy path", "dir", baseConfigDir, "err", err)
				return cand.path
			}
		}
		if err := os.Rename(cand.path, canonical); err != nil {
			if _, statErr := os.Lstat(canonical); statErr == nil {
				return canonical
			}
			// OK: fallback keeps existing user config accessible if rename fails (e.g. EXDEV or read-only FS).
			slog.Debug("failed to migrate legacy config directory; continuing with legacy path", "from", cand.path, "to", canonical, "err", err)
			return cand.path
		}
		if err := os.Symlink(cand.symlinkTarget, cand.path); err != nil {
			// OK: best-effort compatibility symlink; canonical directory is already in place.
			slog.Debug("failed to create legacy config compatibility symlink", "link", cand.path, "target", cand.symlinkTarget, "err", err)
		}
		return canonical
	}
	return canonical
}

var agentEnvVars = []string{
	"AI_AGENT",
	"GERRIT_AI",
	"ANTIGRAVITY_AGENT",
	"GEMINI_CLI",
	"CLAUDECODE",
	"CURSOR_AGENT",
	"CODEX_SANDBOX",
}

// IsAgentEnv returns true if any standard coding-agent environment variable is
// non-empty and not "0".
func IsAgentEnv() bool {
	for _, k := range agentEnvVars {
		val := strings.TrimSpace(os.Getenv(k))
		if val != "" && val != "0" {
			return true
		}
	}
	return false
}

// IsTTY returns true only if v is an *os.File whose Stat() has
// os.ModeCharDevice != 0.
func IsTTY(v any) bool {
	f, ok := v.(*os.File)
	if !ok || f == nil {
		return false
	}
	fi, err := f.Stat()
	if err != nil {
		return false
	}
	return (fi.Mode() & os.ModeCharDevice) != 0
}

// ColorEnabled returns false if NO_COLOR is non-empty, TERM is "dumb",
// GHISH_COLOR is "never", or IsAgentEnv() is true; returns true if GHISH_COLOR
// is "always"; otherwise returns IsTTY(w).
func ColorEnabled(w io.Writer) bool {
	if os.Getenv("NO_COLOR") != "" ||
		os.Getenv("TERM") == "dumb" ||
		os.Getenv("GHISH_COLOR") == "never" ||
		IsAgentEnv() {
		return false
	}
	if os.Getenv("GHISH_COLOR") == "always" {
		return true
	}
	return IsTTY(w)
}

// agentFlag is the value of the --agent flag, which marks content ghish posts
// (Gerrit comments today; Buganizer comments can reuse it) as written by a
// coding agent. Gerrit receives it as the is_ai comment field.
type agentFlag string

const (
	agentAuto  agentFlag = "auto" // Marked when IsAgentEnv reports a coding agent.
	agentTrue  agentFlag = "true"
	agentFalse agentFlag = "false"
)

const agentFlagUsage = "Mark as written by a coding agent (shown as AI-generated): auto, true, or false. " +
	"auto marks it when run by a coding agent; --agent alone means true (ghish-only)"

func (f *agentFlag) String() string { return string(*f) }
func (f *agentFlag) Type() string   { return "auto|true|false" }

// Set accepts "auto" or any value strconv.ParseBool accepts.
func (f *agentFlag) Set(s string) error {
	if s == string(agentAuto) {
		*f = agentAuto
		return nil
	}
	b, err := strconv.ParseBool(s)
	if err != nil {
		return fmt.Errorf("must be auto, true, or false: %w", err)
	}
	*f = agentFalse
	if b {
		*f = agentTrue
	}
	return nil
}

// isAI returns the AI marker to send (Gerrit's is_ai), or nil to leave it
// unset. Leaving it unset keeps an existing Gerrit draft's value; Gerrit only
// clears it on an explicit false.
func (f agentFlag) isAI() *bool {
	t, fl := true, false
	switch f {
	case agentTrue:
		return &t
	case agentFalse:
		return &fl
	case agentAuto:
		if IsAgentEnv() {
			return &t
		}
		return nil
	}
	panic(fmt.Sprintf("invalid agentFlag %q", string(f)))
}

// marked reports whether the content should be marked as written by a coding
// agent.
func (f agentFlag) marked() bool {
	isAI := f.isAI()
	return isAI != nil && *isAI
}
