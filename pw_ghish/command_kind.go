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

import "github.com/spf13/cobra"

// KindAnnotation is the Cobra annotation key every runnable leaf command must
// set to declare which UI contract it follows (see developing.rst, "New
// command checklist"). Its value must be one of the Kind* constants.
const KindAnnotation = "ghish:kind"

// Command kinds. Each kind maps to an output contract from the UI design
// language in developing.rst.
const (
	// KindMutation commands change Gerrit, Buganizer, LUCI, git, or local
	// state and must end with a structured PrintReceipt/FormatReceipt receipt
	// carrying a Next: hint. Commands that both mutate and print (pr checkout,
	// issue develop, wt use) are mutations.
	KindMutation = "mutation"
	// KindView commands render a single object in detail (pr view, run view,
	// sense, auth status) and must lead with a Verdict:/Next: answer and fold
	// long sections past their budgets.
	KindView = "view"
	// KindList commands render a collection (pr list, issue status, wt list,
	// pr checks) and must fold past their budgets with a Verdict:/Next:
	// footer.
	KindList = "list"
	// KindStream commands are long-running, streaming, or passthrough
	// (run watch) and are exempt from single-shot output budgets.
	KindStream = "stream"
	// KindMeta commands are plumbing invoked by tools rather than people
	// (git and agent hooks, version, completion) and emit machine-oriented
	// output.
	KindMeta = "meta"
)

// BudgetExemptAnnotation marks a command as exempt from the default
// performance/context budget for its kind. Its value must be a human-readable
// reason explaining why the exemption is necessary.
const BudgetExemptAnnotation = "ghish:budget-exempt"

// OutputAnnotation declares how a command's stdout is meant to be consumed.
// Its only value is OutputRaw; commands without it print human/agent-facing
// output that follows their kind's Verdict:/Next: contract.
const OutputAnnotation = "ghish:output"

// OutputRaw marks a command whose stdout is a raw artifact meant for other
// tools (e.g. `pr diff` prints a unified patch for `git apply`), so it
// carries no Verdict:/Next: lines and no single-shot byte budget. The
// decorative-noise rule and performance budgets still apply.
const OutputRaw = "raw"

// ValidCommandKinds lists every accepted KindAnnotation value.
var ValidCommandKinds = []string{KindMutation, KindView, KindList, KindStream, KindMeta}

// CommandKind returns the value of c's KindAnnotation, or "" if c does not
// declare a kind.
func CommandKind(c *cobra.Command) string {
	return c.Annotations[KindAnnotation]
}
