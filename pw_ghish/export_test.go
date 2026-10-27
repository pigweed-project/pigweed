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

// This file exposes package internals to the external pw_ghish_test package
// (command_kind_test.go, kind_contract_test.go), which can import
// pigweed.dev/pw_ghish/worktree (an import cycle for internal tests) and
// therefore walk the full command tree.

package pw_ghish

import (
	"reflect"

	"github.com/spf13/cobra"
)

// SetupRootAliasesForTest attaches the root-level aliases (checks, view, diff,
// status) that Execute adds at runtime.
var SetupRootAliasesForTest = setupRootAliases

// IsUnknownSubcommandRejectorForTest reports whether c's RunE was injected by
// rejectUnknownSubcommands (which Execute applies to every group) rather than
// declared by the command itself.
func IsUnknownSubcommandRejectorForTest(c *cobra.Command) bool {
	if c.RunE == nil {
		return false
	}
	root := &cobra.Command{Use: "root"}
	group := &cobra.Command{Use: "group"}
	group.AddCommand(&cobra.Command{Use: "leaf", RunE: func(*cobra.Command, []string) error { return nil }})
	root.AddCommand(group)
	rejectUnknownSubcommands(root)
	return reflect.ValueOf(c.RunE).Pointer() == reflect.ValueOf(group.RunE).Pointer()
}

// Design-language contract helpers from design_contract_test.go, for the
// kind smoke harness (kind_contract_test.go).
var (
	AssertMutationReceiptForTest   = assertMutationReceipt
	AssertVerdictForTest           = assertVerdict
	AssertNextHintForTest          = assertNextHint
	AssertNoDecorativeNoiseForTest = assertNoDecorativeNoise
	AssertByteBudgetForTest        = assertByteBudget
)

// PerfBudgetPinnedArgsForTest returns the arguments of every command
// TestPerfBudget_Commands pins exactly (perfBudgetPins).
func PerfBudgetPinnedArgsForTest() [][]string {
	args := make([][]string, 0, len(perfBudgetPins))
	for _, b := range perfBudgetPins {
		args = append(args, b.args)
	}
	return args
}
