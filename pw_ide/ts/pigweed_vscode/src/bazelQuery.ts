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

import { spawn } from 'child_process';
import * as path from 'path';
import { getReliableBazelExecutable } from './bazel';
import * as fs from 'fs';
import logger from './logging';

let cachedPreconfiguredTargets:
  | { label: string; displayName?: string }[]
  | undefined;
let cachedBuildBazelMtime: number | undefined;

export function _resetCache() {
  cachedPreconfiguredTargets = undefined;
  cachedBuildBazelMtime = undefined;
}

/**
 * Checks if the root BUILD.bazel file contains `pw_compile_commands_generator` targets.
 *
 * @param cwd The working directory to run the query in.
 * @returns Promise<{ label: string; displayName?: string }[]> A list of matching targets with display names.
 */
export async function getPreconfiguredTargets(
  cwd: string,
  spawnFn: typeof spawn = spawn,
  bazelBinary: string | undefined = getReliableBazelExecutable(),
): Promise<{ label: string; displayName?: string }[]> {
  if (!bazelBinary) return [];

  const buildBazelPath = path.join(cwd, 'BUILD.bazel');
  try {
    const stat = await fs.promises.stat(buildBazelPath);
    if (cachedPreconfiguredTargets && cachedBuildBazelMtime === stat.mtimeMs) {
      return cachedPreconfiguredTargets;
    }
    cachedBuildBazelMtime = stat.mtimeMs;
  } catch (e) {
    // ignore
  }

  return new Promise((resolve) => {
    const env = {
      ...process.env,
      PATH: `${path.dirname(bazelBinary)}:${process.env?.PATH || ''}`,
      BAZELISK_SKIP_WRAPPER: '1',
    };

    const query =
      'attr(tags, "pw_compile_commands_generator", attr(generator_function, "pw_compile_commands_generator", //:*))';

    logger.info(`Running ${bazelBinary} query '${query}'`);

    const child = spawnFn(
      bazelBinary,
      ['query', query, '--output=streamed_jsonproto'],
      {
        cwd,
        env,
      },
    );

    let stdout = '';

    child.stdout?.on('data', (data) => {
      stdout += data.toString();
    });

    child.on('close', (code) => {
      if (code !== 0) {
        resolve([]);
        return;
      }

      const targets: {
        label: string;
        displayName?: string;
        lineNumber?: number;
      }[] = [];

      const lines = stdout
        .trim()
        .split('\n')
        .filter((l) => l.trim().length > 0);
      for (const line of lines) {
        try {
          const obj = JSON.parse(line);
          if (obj.type === 'RULE' && obj.rule) {
            const label = obj.rule.name;
            const location = obj.rule.location;
            const locMatch = location?.match(/:(\d+):\d+$/);
            const lineNumber = locMatch ? parseInt(locMatch[1], 10) : undefined;

            const attr = obj.rule.attribute?.find(
              (a: any) => a.name === 'display_name',
            );
            const displayName = attr?.stringValue;

            targets.push({ label, displayName, lineNumber });
          }
        } catch (e) {
          // Ignore parse errors for individual lines
        }
      }

      targets.sort((a, b) => (a.lineNumber || 0) - (b.lineNumber || 0));

      cachedPreconfiguredTargets = targets.map(({ label, displayName }) => ({
        label,
        displayName,
      }));
      resolve(cachedPreconfiguredTargets);
    });

    child.on('error', () => {
      resolve([]);
    });
  });
}
