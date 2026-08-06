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
import { filterPreconfiguredTargets } from './targetFilter';

const testSuite = typeof suite === 'function' ? suite : describe;

testSuite('filterPreconfiguredTargets', () => {
  test('filters C++ and Rust targets correctly', () => {
    const targets = [
      { label: '//:cpp_only', hasCpp: true, hasRust: false },
      { label: '//:rust_only', hasCpp: false, hasRust: true },
      { label: '//:both', hasCpp: true, hasRust: true },
      { label: '//:neither', hasCpp: false, hasRust: false },
    ];

    const cppTargets = filterPreconfiguredTargets(targets, 'cpp');
    assert.deepStrictEqual(cppTargets, [
      { label: '//:cpp_only', hasCpp: true, hasRust: false },
      { label: '//:both', hasCpp: true, hasRust: true },
    ]);

    const rustTargets = filterPreconfiguredTargets(targets, 'rust');
    assert.deepStrictEqual(rustTargets, [
      { label: '//:rust_only', hasCpp: false, hasRust: true },
      { label: '//:both', hasCpp: true, hasRust: true },
    ]);
  });

  test('handles undefined targets gracefully', () => {
    assert.deepStrictEqual(filterPreconfiguredTargets(undefined, 'cpp'), []);
    assert.deepStrictEqual(filterPreconfiguredTargets(undefined, 'rust'), []);
  });
});
