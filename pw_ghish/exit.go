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
)

// Process exit codes. These intentionally mirror the GitHub CLI so that
// scripts and agents that already know the `gh` contract keep working.
const (
	// ExitCodeOK indicates the command succeeded.
	ExitCodeOK = 0

	// ExitCodeFailure is the exit code used for any error that does not
	// request a more specific one.
	ExitCodeFailure = 1

	// ExitCodePending indicates that CI checks have not finished yet. `gh pr
	// checks` uses exit code 8 for this so that callers can distinguish "not
	// done yet" (retry later) from "failed" (stop and investigate). Returning
	// 0 here would make `gh pr checks && gh pr merge` merge a change whose CI
	// is still running.
	ExitCodePending = 8
)

// ExitCodeError is an error that carries the process exit code the CLI should
// terminate with. Commands return it from RunE instead of calling os.Exit, so
// that deferred cleanup still runs and the behavior stays unit-testable.
//
// Use ExitCodeFor to map any error onto an exit code.
type ExitCodeError struct {
	// Code is the process exit code. It must be non-zero: an ExitCodeError
	// always represents a failure of some kind, even when (as with
	// ExitCodePending) the underlying condition is merely "not finished".
	Code int
	// Err is the underlying error. It is never nil for values built by
	// NewExitCodeError.
	Err error
}

// Error implements the error interface.
func (e *ExitCodeError) Error() string {
	if e == nil || e.Err == nil {
		return "unknown error"
	}
	return e.Err.Error()
}

// Unwrap allows errors.Is and errors.As to see through to the wrapped error.
func (e *ExitCodeError) Unwrap() error {
	if e == nil {
		return nil
	}
	return e.Err
}

// NewExitCodeError returns an error that terminates the process with the given
// exit code. The message is formatted with fmt.Errorf semantics, so it may
// contain a %w verb to wrap an underlying error.
//
// Passing ExitCodeOK is a programmer error: an error must never map to a
// successful exit. It is normalized to ExitCodeFailure so that a mistake here
// fails loudly rather than silently reporting success.
func NewExitCodeError(code int, format string, args ...any) error {
	if code == ExitCodeOK {
		code = ExitCodeFailure
	}
	return &ExitCodeError{Code: code, Err: fmt.Errorf(format, args...)}
}

// ExitCodeFor returns the process exit code that corresponds to err.
//
// A nil error maps to ExitCodeOK. An error carrying an *ExitCodeError anywhere
// in its unwrap chain maps to that code. Everything else maps to
// ExitCodeFailure, so an unannotated error can never be mistaken for success.
func ExitCodeFor(err error) int {
	if err == nil {
		return ExitCodeOK
	}
	var codeErr *ExitCodeError
	if errors.As(err, &codeErr) && codeErr.Code != ExitCodeOK {
		return codeErr.Code
	}
	return ExitCodeFailure
}
