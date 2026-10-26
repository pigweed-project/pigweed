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
	os.Exit(pw_ghish.Main(os.Args[1:], os.Stdout, os.Stderr, os.Exit))
}
