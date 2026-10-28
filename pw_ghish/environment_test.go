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

import "testing"

func TestAgentFlag(t *testing.T) {
	tests := []struct {
		in        string
		wantAgent any // marker under an agent: true, false, or nil (unset)
		wantHuman any
	}{
		{in: "auto", wantAgent: true, wantHuman: nil},
		{in: "true", wantAgent: true, wantHuman: true},
		{in: "1", wantAgent: true, wantHuman: true},
		{in: "false", wantAgent: false, wantHuman: false},
	}
	deref := func(b *bool) any {
		if b == nil {
			return nil
		}
		return *b
	}
	for _, tc := range tests {
		var f agentFlag
		if err := f.Set(tc.in); err != nil {
			t.Fatalf("Set(%q): %v", tc.in, err)
		}
		t.Run(tc.in+" under agent", func(t *testing.T) {
			t.Setenv("ANTIGRAVITY_AGENT", "1")
			if got := deref(f.isAI()); got != tc.wantAgent {
				t.Errorf("got %v, want %v", got, tc.wantAgent)
			}
		})
		t.Run(tc.in+" under human", func(t *testing.T) {
			if got := deref(f.isAI()); got != tc.wantHuman {
				t.Errorf("got %v, want %v", got, tc.wantHuman)
			}
		})
	}
	var f agentFlag
	if err := f.Set("banana"); err == nil {
		t.Error("Set(banana) succeeded, want error")
	}
}
