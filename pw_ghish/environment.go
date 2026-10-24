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
	"io"
	"os"
	"strings"
)

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

// IsTTY returns true only if w is an *os.File whose Stat() has
// os.ModeCharDevice != 0.
func IsTTY(w io.Writer) bool {
	if w == nil {
		return false
	}
	f, ok := w.(*os.File)
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
