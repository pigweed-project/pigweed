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

import * as assert from 'assert';
import { getClangdArgs } from './vscCommands';

test('getClangdArgs returns correct arguments', () => {
  const targetDir = '/path/to/target';
  const cores = 4;
  const args = getClangdArgs(targetDir, cores);

  assert.deepStrictEqual(args, [
    `--compile-commands-dir=${targetDir}`,
    '--query-driver=/*',
    '--header-insertion=never',
    '--background-index',
    '-j=1', // Math.max(1, Math.round(4 / 4)) = 1
  ]);
});

test('getClangdArgs handles different core counts', () => {
  const targetDir = '/path/to/target';

  // Test with 8 cores -> -j=2
  const args8 = getClangdArgs(targetDir, 8);
  assert.ok(args8.includes('-j=2'));

  // Test with 1 core -> -j=1
  const args1 = getClangdArgs(targetDir, 1);
  assert.ok(args1.includes('-j=1'));
});

test('getClangdArgs uses /* for query-driver glob', () => {
  const targetDir = '/path/to/target';
  const cores = 4;
  const args = getClangdArgs(targetDir, cores);
  assert.ok(args.includes('--query-driver=/*'));
});
