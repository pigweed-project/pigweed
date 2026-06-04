// Copyright 2025 The Pigweed Authors
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
import { EventEmitter } from 'events';
import { getPreconfiguredTargets, _resetCache } from './bazelQuery';
import * as sinon from 'sinon';
import * as fs from 'fs';

suite('getPreconfiguredTargets', () => {
  const mockBazelBinary = '/path/to/bazel';
  const mockCwd = '/path/to/cwd';

  function createMockSpawn(
    stdoutData: string,
    stderrData: string,
    exitCode: number,
    error?: Error,
  ) {
    return (_command: string, _args: string[], _options: any) => {
      const child: any = new EventEmitter();
      child.stdout = new EventEmitter();
      child.stderr = new EventEmitter();
      child.kill = () => {
        /* empty */
      };

      setTimeout(() => {
        if (error) {
          child.emit('error', error);
        } else {
          if (stdoutData) child.stdout.emit('data', stdoutData);
          if (stderrData) child.stderr.emit('data', stderrData);
          child.emit('close', exitCode);
        }
      }, 0);

      return child;
    };
  }

  test('returns targets when they exist', async () => {
    const stdout =
      JSON.stringify({
        type: 'RULE',
        rule: {
          name: '//:update_compile_commands',
          location: '/BUILD.bazel:100:1',
          attribute: [
            {
              name: 'display_name',
              type: 'STRING',
              stringValue: 'All Platforms',
            },
          ],
        },
      }) +
      '\n' +
      JSON.stringify({
        type: 'RULE',
        rule: {
          name: '//:other_target',
          location: '/BUILD.bazel:200:1',
          attribute: [],
        },
      }) +
      '\n';
    const spawnFn = createMockSpawn(stdout, '', 0);

    const result = await getPreconfiguredTargets(
      mockCwd,
      spawnFn as any,
      mockBazelBinary,
    );

    assert.deepStrictEqual(result, [
      { label: '//:update_compile_commands', displayName: 'All Platforms' },
      { label: '//:other_target', displayName: undefined },
    ]);
  });

  test('returns empty array when no target found (empty output)', async () => {
    const stdout = '';
    const spawnFn = createMockSpawn(stdout, '', 0);

    const result = await getPreconfiguredTargets(
      mockCwd,
      spawnFn as any,
      mockBazelBinary,
    );

    assert.deepStrictEqual(result, []);
  });

  test('returns empty array when bazel query fails (exit code 1)', async () => {
    const spawnFn = createMockSpawn('', 'Target not found', 1);

    const result = await getPreconfiguredTargets(
      mockCwd,
      spawnFn as any,
      mockBazelBinary,
    );

    assert.deepStrictEqual(result, []);
  });

  test('returns empty array when spawn errors', async () => {
    const spawnFn = createMockSpawn('', '', 0, new Error('Spawn failed'));

    const result = await getPreconfiguredTargets(
      mockCwd,
      spawnFn as any,
      mockBazelBinary,
    );

    assert.deepStrictEqual(result, []);
  });

  test('returns empty array when bazel binary is undefined', async () => {
    const spawnFn = createMockSpawn('', '', 0);

    const result = await getPreconfiguredTargets(
      mockCwd,
      spawnFn as any,
      undefined,
    );

    assert.deepStrictEqual(result, []);
  });

  suite('caching', () => {
    let statStub: sinon.SinonStub;

    setup(() => {
      _resetCache();
      statStub = sinon.stub(fs.promises, 'stat');
    });

    teardown(() => {
      statStub.restore();
      _resetCache();
    });

    test('returns cached targets when file mtime matches', async () => {
      const stdout =
        JSON.stringify({
          type: 'RULE',
          rule: { name: '//:target1', location: '/BUILD.bazel:100:1' },
        }) + '\n';

      const spawnFn = createMockSpawn(stdout, '', 0);

      statStub.resolves({ mtimeMs: 1000 } as any);

      // First call - should call spawn
      const result1 = await getPreconfiguredTargets(
        mockCwd,
        spawnFn as any,
        mockBazelBinary,
      );
      assert.deepStrictEqual(result1, [
        { label: '//:target1', displayName: undefined },
      ]);

      // Second call - should NOT call spawn, should return cached result
      let called = false;
      const spySpawn = (_command: string, _args: string[], _options: any) => {
        called = true;
        const child: any = new EventEmitter();
        child.stdout = new EventEmitter();
        child.stderr = new EventEmitter();
        setTimeout(() => {
          child.stdout.emit('data', stdout);
          child.emit('close', 0);
        }, 0);
        return child;
      };

      const result2 = await getPreconfiguredTargets(
        mockCwd,
        spySpawn as any,
        mockBazelBinary,
      );
      assert.deepStrictEqual(result2, [
        { label: '//:target1', displayName: undefined },
      ]);
      assert.strictEqual(
        called,
        false,
        'Spawn should not have been called for cached result',
      );
    });

    test('invalidates cache when file mtime changes', async () => {
      const stdout1 =
        JSON.stringify({
          type: 'RULE',
          rule: { name: '//:target1', location: '/BUILD.bazel:100:1' },
        }) + '\n';

      const stdout2 =
        JSON.stringify({
          type: 'RULE',
          rule: { name: '//:target2', location: '/BUILD.bazel:200:1' },
        }) + '\n';

      statStub.resolves({ mtimeMs: 1000 } as any);

      const spawnFn1 = createMockSpawn(stdout1, '', 0);
      const result1 = await getPreconfiguredTargets(
        mockCwd,
        spawnFn1 as any,
        mockBazelBinary,
      );
      assert.deepStrictEqual(result1, [
        { label: '//:target1', displayName: undefined },
      ]);

      // Change mtime
      statStub.resolves({ mtimeMs: 2000 } as any);

      const spawnFn2 = createMockSpawn(stdout2, '', 0);
      const result2 = await getPreconfiguredTargets(
        mockCwd,
        spawnFn2 as any,
        mockBazelBinary,
      );
      assert.deepStrictEqual(result2, [
        { label: '//:target2', displayName: undefined },
      ]);
    });
  });
});
