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

package main

import (
	"os"

	"pigweed.dev/pw_ghish"
	_ "pigweed.dev/pw_ghish/worktree"
)

func main() {
	// Errors carry their own exit code so that commands can honor established
	// CLI contracts (notably `gh pr checks`, which exits 8 when checks are
	// still pending). Anything without an explicit code exits 1.
	if err := pw_ghish.Execute(); err != nil {
		os.Exit(pw_ghish.ExitCodeFor(err))
	}
}
