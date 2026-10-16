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

// This file verifies that pw_ghish's Sphinx documentation (`pw_ghish/*.rst`)
// stays synchronized with the Go implementation, checking that every
// `.ghish.toml` struct tag in `tomlProjectConfig` is documented in
// `project_setup.rst` and that every non-hidden Cobra command/subcommand is
// documented in its corresponding `.rst` reference page.

package pw_ghish

import (
	"os"
	"reflect"
	"slices"
	"strings"
	"testing"
)

// TestDocSync_ProjectConfigTOMLKeysDocumented reflects over tomlProjectConfig
// in project_config.go and verifies that every non-alias `.ghish.toml` section
// and key is documented in pw_ghish/project_setup.rst.
func TestDocSync_ProjectConfigTOMLKeysDocumented(t *testing.T) {
	setupRSTBytes, err := os.ReadFile("project_setup.rst")
	if err != nil {
		t.Fatalf("os.ReadFile(project_setup.rst): %v", err)
	}
	setupRST := string(setupRSTBytes)

	// Legacy compatibility aliases that map onto a primary documented field:
	aliasSections := []string{"bugs"}
	aliasKeys := map[string]string{
		"issue.default_component_id": "default_component",
		"issue.require_trailer":      "trailer_format",
	}

	rootType := reflect.TypeOf(tomlProjectConfig{})
	for i := range rootType.NumField() {
		secField := rootType.Field(i)
		secTag, _, _ := strings.Cut(secField.Tag.Get("toml"), ",")
		if secTag == "" || slices.Contains(aliasSections, secTag) {
			continue
		}

		if !strings.Contains(setupRST, "["+secTag+"]") && !strings.Contains(setupRST, "["+secTag+".") {
			t.Errorf("project_setup.rst is missing documentation for .ghish.toml section [%s]", secTag)
		}

		secType := secField.Type
		if secType.Kind() != reflect.Struct {
			continue
		}
		for j := range secType.NumField() {
			f := secType.Field(j)
			keyTag, _, _ := strings.Cut(f.Tag.Get("toml"), ",")
			if keyTag == "" {
				continue
			}
			fullKey := secTag + "." + keyTag
			if _, isAlias := aliasKeys[fullKey]; isAlias {
				continue
			}
			if !strings.Contains(setupRST, keyTag) {
				t.Errorf("project_setup.rst is missing documentation for .ghish.toml key %q (in [%s])", keyTag, secTag)
			}
		}
	}
}

// TestDocSync_SubcommandsDocumentedInRST walks RootCmd and verifies that every
// non-hidden command family and subcommand is documented in its corresponding
// pw_ghish/*.rst reference file.
func TestDocSync_SubcommandsDocumentedInRST(t *testing.T) {
	setupRootCmdHelp()
	setupRootAliases()

	familyRSTFile := map[string]string{
		"pr":    "pr.rst",
		"run":   "run.rst",
		"issue": "issue.rst",
		"auth":  "auth.rst",
		"hook":  "hook.rst",
		"sense": "sense.rst",
	}

	rstContents := make(map[string]string)
	for _, filename := range familyRSTFile {
		data, err := os.ReadFile(filename)
		if err != nil {
			t.Fatalf("os.ReadFile(%q): %v", filename, err)
		}
		rstContents[filename] = string(data)
	}

	for _, top := range RootCmd.Commands() {
		if top.Hidden || top.Name() == "help" || top.Name() == "completion" {
			continue
		}
		rstFile, ok := familyRSTFile[top.Name()]
		if !ok {
			// Root-level aliases (view, diff, checks, status, push, agent) are
			// documented in subcommands.rst.
			subRSTBytes, err := os.ReadFile("subcommands.rst")
			if err != nil {
				t.Fatalf("os.ReadFile(subcommands.rst): %v", err)
			}
			if !strings.Contains(string(subRSTBytes), top.Name()) {
				t.Errorf("root command/alias %q is not documented in subcommands.rst", top.Name())
			}
			continue
		}

		content := rstContents[rstFile]
		for _, sub := range top.Commands() {
			// `pr sense` is an alias for the top-level `gh sense` command, which
			// has its own dedicated `sense.rst` reference page (verified via the
			// `"sense": "sense.rst"` entry in familyRSTFile above).
			if sub.Hidden || sub.Name() == "help" || sub.Name() == "sense" {
				continue
			}
			expectedPhrase := top.Name() + " " + sub.Name()
			if !strings.Contains(content, expectedPhrase) {
				t.Errorf("subcommand %q is not documented in %s", expectedPhrase, rstFile)
			}
		}
	}
}
