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
	"testing"

	"pigweed.dev/pw_ghish"
)

func TestWTCommandRegisteredOnRoot(t *testing.T) {
	wtCmd, _, err := pw_ghish.RootCmd.Find([]string{"wt"})
	if err != nil || wtCmd == nil {
		t.Fatalf("expected 'wt' command registered on RootCmd, got err: %v", err)
	}
	if wtCmd.Name() != "wt" {
		t.Errorf("expected command name 'wt', got %q", wtCmd.Name())
	}

	subcommands := []string{"init", "use", "park", "next", "list", "close", "gc"}
	for _, sub := range subcommands {
		subCmd, _, err := pw_ghish.RootCmd.Find([]string{"wt", sub})
		if err != nil || subCmd == nil || subCmd.Name() != sub {
			t.Errorf("expected 'wt %s' subcommand registered, got err: %v", sub, err)
		}
	}
}
