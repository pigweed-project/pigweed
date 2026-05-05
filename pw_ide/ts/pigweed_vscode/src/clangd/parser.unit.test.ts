// Copyright 2024 The Pigweed Authors
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
import * as path from 'path';

import {
  CompilationDatabase,
  inferTarget,
  inferTargetPositions,
} from './parser';

test('handles invalid compile commands gracefully', () => {
  const db = new CompilationDatabase();
  const contents = JSON.stringify([
    {
      directory: '/project',
      file: 'test.cpp',
      // Invalid: missing both command and arguments
    },
    {
      directory: '/project',
      file: 'another.cpp',
      command: 'g++ -c another.cpp -o another.o',
      output: 'another.o',
    },
  ]);

  db.loadFromString(contents);

  const invalidCommand = db.db.find((cmd) => cmd.data.file === 'test.cpp');
  const validCommand = db.db.find((cmd) => cmd.data.file === 'another.cpp');

  assert.ok(invalidCommand);
  assert.ok(invalidCommand.error);
  assert.strictEqual(
    invalidCommand.error.message,
    'Compile command must have a command or arguments',
  );

  assert.ok(validCommand);
  assert.strictEqual(validCommand.error, undefined);
});

test('inferTargetPositions', () => {
  const testCases: [string, number[]][] = [
    ['?', [0]],
    ['*' + path.sep + '?', [1]],
    ['*' + path.sep + '*' + path.sep + '?', [2]],
    ['*' + path.sep + '?' + path.sep + '?', [1, 2]],
  ];

  for (const [glob, positions] of testCases) {
    assert.deepStrictEqual(positions, inferTargetPositions(glob));
  }
});

test('inferTarget', () => {
  const testCases: [string, string, string][] = [
    ['?', ['target', 'thing.o'].join(path.sep), 'target'],
    [
      '*' + path.sep + '?',
      ['variants', 'target', 'foo', 'bar', 'thing.o'].join(path.sep),
      'target',
    ],
    [
      '*' + path.sep + '*' + path.sep + '*' + path.sep + '*' + path.sep + '?',
      ['foo', 'bar', 'baz', 'hi', 'target', 'obj', 'thing.o'].join(path.sep),
      'target',
    ],
    [
      '*' + path.sep + '?' + path.sep + '?',
      ['variants', 'target', 'foo', 'bar', 'thing.o'].join(path.sep),
      'target_foo',
    ],
  ];

  for (const [glob, inputPath, name] of testCases) {
    assert.equal(name, inferTarget(glob, inputPath));
  }
});

test('does not filter out header files', () => {
  const db = new CompilationDatabase();
  const contents = JSON.stringify([
    {
      directory: '/project',
      file: 'test.cpp',
      command: 'g++ -c test.cpp -o test.o',
    },
    {
      directory: '/project',
      file: 'header.h',
      command: 'g++ -c header.h -o header.o',
    },
    {
      directory: '/project',
      file: 'another_header.hpp',
      command: 'g++ -c another_header.hpp -o another_header.o',
    },
  ]);

  db.loadFromString(contents);

  const cppFile = db.db.find((cmd) => cmd.data.file === 'test.cpp');
  const hFile = db.db.find((cmd) => cmd.data.file === 'header.h');
  const hppFile = db.db.find((cmd) => cmd.data.file === 'another_header.hpp');

  assert.ok(cppFile);
  assert.ok(hFile);
  assert.ok(hppFile);
});
