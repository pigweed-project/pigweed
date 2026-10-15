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
	"fmt"
	"strings"
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

func TestBatchChangeIDQueries(t *testing.T) {
	if got := BatchChangeIDQueries(nil, 30); len(got) != 0 {
		t.Fatalf("expected nil/empty slice for empty input, got %v", got)
	}
	if got := BatchChangeIDQueries([]string{"  ", ""}, 30); len(got) != 0 {
		t.Fatalf("expected empty slice for whitespace input, got %v", got)
	}

	// 205 Change-IDs with duplicates -> 200 unique -> 7 batches of at most 30 terms each
	var ids []string
	for i := 1; i <= 200; i++ {
		ids = append(ids, fmt.Sprintf("I%040d", i))
	}
	// Add duplicates and whitespace
	ids = append(ids, "  I0000000000000000000000000000000000000001  ", "", "I0000000000000000000000000000000000000200")

	batches := BatchChangeIDQueries(ids, 30)
	if len(batches) != 7 {
		t.Fatalf("expected 7 batches for 200 unique Change-IDs (batchSize=30), got %d", len(batches))
	}
	for i := 0; i < 6; i++ {
		terms := strings.Split(batches[i], " OR ")
		if len(terms) != 30 {
			t.Errorf("batch %d: expected 30 terms, got %d", i, len(terms))
		}
	}
	lastTerms := strings.Split(batches[6], " OR ")
	if len(lastTerms) != 20 {
		t.Errorf("last batch: expected 20 terms, got %d", len(lastTerms))
	}
	if lastTerms[0] != "change:I0000000000000000000000000000000000000181" {
		t.Errorf("unexpected first term in last batch: %q", lastTerms[0])
	}
}
