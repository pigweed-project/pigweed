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

// These tests live in the external pw_ghish_test package so they can import
// pigweed.dev/pw_ghish/worktree, whose init() attaches the `wt` subtree to
// RootCmd, and therefore walk the same command tree users get from ./gh.

package pw_ghish_test

import (
	"slices"
	"strings"
	"testing"

	"github.com/spf13/cobra"
	"pigweed.dev/pw_ghish"
	_ "pigweed.dev/pw_ghish/worktree"
)

const newCommandChecklistHint = `see pw_ghish/developing.rst "New command checklist"`

// isCobraBuiltin reports whether c is (or lives under) Cobra's auto-generated
// `help` or `completion` command.
func isCobraBuiltin(root, c *cobra.Command) bool {
	for cur := c; cur != nil && cur != root; cur = cur.Parent() {
		if cur.Parent() == root && (cur.Name() == "help" || cur.Name() == "completion") {
			return true
		}
	}
	return false
}

// walkCommands visits every command below root (excluding root itself and
// Cobra builtins) in depth-first order.
func walkCommands(root *cobra.Command, visit func(*cobra.Command)) {
	var walk func(*cobra.Command)
	walk = func(c *cobra.Command) {
		for _, child := range c.Commands() {
			if isCobraBuiltin(root, child) {
				continue
			}
			visit(child)
			walk(child)
		}
	}
	walk(root)
}

// runtimeRoot returns RootCmd with every family attached the way Execute
// attaches them (worktree via its init(), root aliases explicitly).
func runtimeRoot() *cobra.Command {
	pw_ghish.SetupRootAliasesForTest()
	return pw_ghish.RootCmd
}

// declaresOwnRun reports whether c runs its own action, as opposed to being a
// non-runnable group or a group whose RunE only rejects unknown subcommands.
func declaresOwnRun(c *cobra.Command) bool {
	return c.Runnable() && !pw_ghish.IsUnknownSubcommandRejectorForTest(c)
}

func TestCommandKind(t *testing.T) {
	c := &cobra.Command{Use: "x"}
	if got := pw_ghish.CommandKind(c); got != "" {
		t.Errorf("CommandKind(no annotations) = %q, want \"\"", got)
	}
	c.Annotations = map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindList}
	if got := pw_ghish.CommandKind(c); got != pw_ghish.KindList {
		t.Errorf("CommandKind = %q, want %q", got, pw_ghish.KindList)
	}
}

// TestInvariants_EveryRunnableCommandDeclaresKind walks the live RootCmd tree
// and requires every runnable command (leaves, and groups with their own RunE
// such as `auth`) to declare a valid ghish:kind, so new commands cannot opt
// out of the UI design language (AGENTS.md invariant #15). Groups whose only
// RunE is the unknown-subcommand rejector that Execute installs are skipped.
func TestInvariants_EveryRunnableCommandDeclaresKind(t *testing.T) {
	root := runtimeRoot()
	runnable := 0
	walkCommands(root, func(c *cobra.Command) {
		if !declaresOwnRun(c) {
			return
		}
		runnable++
		kind := pw_ghish.CommandKind(c)
		t.Logf("%-28s %s", c.CommandPath(), kind)
		if kind == "" {
			t.Errorf("%q has no %q annotation; add Annotations: map[string]string{KindAnnotation: Kind<...>} with one of %v (%s)",
				c.CommandPath(), pw_ghish.KindAnnotation, pw_ghish.ValidCommandKinds, newCommandChecklistHint)
			return
		}
		if !slices.Contains(pw_ghish.ValidCommandKinds, kind) {
			t.Errorf("%q declares %s=%q; must be one of %v (%s)",
				c.CommandPath(), pw_ghish.KindAnnotation, kind, pw_ghish.ValidCommandKinds, newCommandChecklistHint)
		}
	})
	if runnable == 0 {
		t.Fatal("found no runnable commands under RootCmd; the tree walk is broken")
	}
}

// TestInvariants_BudgetExemptionsHaveReasons requires every
// ghish:budget-exempt annotation to carry a human-readable reason.
func TestInvariants_BudgetExemptionsHaveReasons(t *testing.T) {
	const minReasonLen = 10
	walkCommands(runtimeRoot(), func(c *cobra.Command) {
		reason, ok := c.Annotations[pw_ghish.BudgetExemptAnnotation]
		if !ok {
			return
		}
		if len(strings.TrimSpace(reason)) < minReasonLen {
			t.Errorf("%q: %s=%q must explain why the command is exempt (at least %d characters; %s)",
				c.CommandPath(), pw_ghish.BudgetExemptAnnotation, reason, minReasonLen, newCommandChecklistHint)
		}
	})
}
