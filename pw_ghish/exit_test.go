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
	"errors"
	"fmt"
	"testing"
)

func TestExitCodeFor(t *testing.T) {
	sentinel := errors.New("boom")

	tests := []struct {
		name string
		err  error
		want int
	}{
		{
			name: "nil error is success",
			err:  nil,
			want: ExitCodeOK,
		},
		{
			name: "plain error defaults to failure",
			err:  errors.New("something went wrong"),
			want: ExitCodeFailure,
		},
		{
			name: "explicit pending code is preserved",
			err:  NewExitCodeError(ExitCodePending, "checks pending"),
			want: ExitCodePending,
		},
		{
			name: "explicit failure code is preserved",
			err:  NewExitCodeError(ExitCodeFailure, "checks failed"),
			want: ExitCodeFailure,
		},
		{
			name: "code survives being wrapped by a caller",
			err:  fmt.Errorf("running checks: %w", NewExitCodeError(ExitCodePending, "checks pending")),
			want: ExitCodePending,
		},
		{
			name: "code survives multiple layers of wrapping",
			err: fmt.Errorf("outer: %w",
				fmt.Errorf("inner: %w", NewExitCodeError(ExitCodePending, "checks pending"))),
			want: ExitCodePending,
		},
		{
			name: "arbitrary non-standard code is preserved",
			err:  NewExitCodeError(42, "custom"),
			want: 42,
		},
		{
			// An error must never report success. A caller that asks for
			// ExitCodeOK has made a mistake; normalizing to 1 makes that
			// mistake visible instead of silently swallowing the failure.
			name: "success code on an error is normalized to failure",
			err:  NewExitCodeError(ExitCodeOK, "this is still an error"),
			want: ExitCodeFailure,
		},
		{
			// Same guarantee, but for a struct literal that bypassed the
			// NewExitCodeError constructor.
			name: "hand-built zero code is normalized to failure",
			err:  &ExitCodeError{Code: ExitCodeOK, Err: sentinel},
			want: ExitCodeFailure,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := ExitCodeFor(tt.err); got != tt.want {
				t.Errorf("ExitCodeFor(%v) = %d, want %d", tt.err, got, tt.want)
			}
		})
	}
}

func TestExitCodeError_MessageAndUnwrap(t *testing.T) {
	t.Run("message is the wrapped error's message", func(t *testing.T) {
		err := NewExitCodeError(ExitCodePending, "%d checks pending", 3)
		if got, want := err.Error(), "3 checks pending"; got != want {
			t.Errorf("Error() = %q, want %q", got, want)
		}
	})

	t.Run("errors.Is sees through to the wrapped sentinel", func(t *testing.T) {
		sentinel := errors.New("underlying cause")
		err := NewExitCodeError(ExitCodeFailure, "could not query Buildbucket: %w", sentinel)
		if !errors.Is(err, sentinel) {
			t.Errorf("errors.Is(%v, sentinel) = false, want true", err)
		}
	})

	t.Run("errors.As extracts the exit code error", func(t *testing.T) {
		err := fmt.Errorf("wrapped: %w", NewExitCodeError(ExitCodePending, "pending"))
		var codeErr *ExitCodeError
		if !errors.As(err, &codeErr) {
			t.Fatalf("errors.As(%v) = false, want true", err)
		}
		if codeErr.Code != ExitCodePending {
			t.Errorf("Code = %d, want %d", codeErr.Code, ExitCodePending)
		}
	})

	t.Run("nil receiver does not panic", func(t *testing.T) {
		var codeErr *ExitCodeError
		if got := codeErr.Error(); got == "" {
			t.Error("Error() on nil receiver returned an empty string")
		}
		if got := codeErr.Unwrap(); got != nil {
			t.Errorf("Unwrap() on nil receiver = %v, want nil", got)
		}
	})
}

// TestExitCodesMatchGitHubCLI pins the numeric values. These are a public
// contract with shell scripts and agents that already know `gh`; changing them
// silently breaks every caller.
func TestExitCodesMatchGitHubCLI(t *testing.T) {
	if ExitCodeOK != 0 {
		t.Errorf("ExitCodeOK = %d, want 0", ExitCodeOK)
	}
	if ExitCodeFailure != 1 {
		t.Errorf("ExitCodeFailure = %d, want 1", ExitCodeFailure)
	}
	if ExitCodePending != 8 {
		t.Errorf("ExitCodePending = %d, want 8 (gh pr checks uses 8 for pending)", ExitCodePending)
	}
}
