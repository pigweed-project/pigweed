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
import { calculateIsStale } from './report';

suite('report - calculateIsStale', () => {
  test('isStale is true when file contains old timestamp', () => {
    const now = Math.floor(Date.now() / 1000);
    const sixDaysAgo = now - 6 * 24 * 60 * 60;

    const result = calculateIsStale(sixDaysAgo.toString(), now);

    assert.strictEqual(result, true);
  });

  test('isStale is false when file contains recent timestamp', () => {
    const now = Math.floor(Date.now() / 1000);
    const fourDaysAgo = now - 4 * 24 * 60 * 60;

    const result = calculateIsStale(fourDaysAgo.toString(), now);

    assert.strictEqual(result, false);
  });

  test('returns false on invalid timestamp', () => {
    const now = Math.floor(Date.now() / 1000);

    const result = calculateIsStale('invalid', now);

    assert.strictEqual(result, false);
  });
});
