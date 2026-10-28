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
	"os"
	"strconv"
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
