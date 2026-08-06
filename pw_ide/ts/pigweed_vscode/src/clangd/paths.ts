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

import * as fs from 'fs';
import * as path from 'path';

import { glob } from 'glob';

import { loadUnprocessedMapping } from './unprocessedMapping';
import { settings, workingDir } from '../settings/vscode';

export const CDB_FILE_NAME = 'compile_commands.json' as const;

export const LAST_BAZEL_COMMAND_FILE_NAME = 'pwLastBazelCommand.txt' as const;

export const CDB_FILE_DIR = '.compile_commands';

export const CDB_FILE_DIRS = [
  CDB_FILE_DIR,
  '.pw_ide', // The legacy pw_ide directory
];

// Need this indirection to prevent `workingDir` being called before init.
const CDB_DIRS = () =>
  CDB_FILE_DIRS.map((dir) => path.join(workingDir.get(), dir));

export class Target {
  private _name: string;
  private _dir: string;
  private _displayName?: string;
  private _lastGeneratedAt?: string;
  private _hasCpp: boolean;
  private _hasRust: boolean;

  constructor(
    name: string,
    dir?: string,
    displayName?: string,
    lastGeneratedAt?: string,
    hasCpp = false,
    hasRust = false,
  ) {
    this._name = name;
    this._dir = dir ?? path.join(CDB_FILE_DIRS[0], name);
    this._displayName = displayName;
    this._lastGeneratedAt = lastGeneratedAt;
    this._hasCpp = hasCpp;
    this._hasRust = hasRust;
  }

  get name() {
    return this._name;
  }

  /** Absolute path to the directory containing the compile commmands file. */
  get dir() {
    return this._dir;
  }

  /** Absolute path to the compile commands file. */
  get path() {
    return path.join(this._dir, CDB_FILE_NAME);
  }

  get displayName() {
    if (this._displayName) {
      return this._displayName;
    }

    if (path.dirname(this._dir) === CDB_FILE_DIRS[0]) {
      return this._name;
    }

    // If the target is not in the canonical directory, append the directory
    // name to distinguish it from targets with the same name in the canonical
    // directory.
    const dirName = path.basename(path.dirname(this._dir));
    return `${this._name} (${dirName})`;
  }

  get lastGeneratedAt() {
    return this._lastGeneratedAt;
  }

  get hasCpp() {
    return this._hasCpp;
  }

  get hasRust() {
    return this._hasRust;
  }
}

export async function availableTargets(): Promise<Target[]> {
  // Get targets from the standard compile commands directories.
  const directoryTargets = (
    await Promise.all(
      CDB_DIRS().map(async (cwd) => {
        let subdirs: string[] = [];
        try {
          subdirs = await fs.promises.readdir(cwd);
        } catch (e) {
          return [];
        }

        const targets = await Promise.all(
          subdirs.map(async (subdir) => {
            const dir = path.join(cwd, subdir);
            const stat = await fs.promises.stat(dir).catch(() => null);
            if (!stat || !stat.isDirectory()) return null;

            const hasCpp = fs.existsSync(
              path.join(dir, 'compile_commands.json'),
            );
            const hasRust = fs.existsSync(path.join(dir, 'rust-project.json'));
            if (!hasCpp && !hasRust) return null;

            let displayName: string | undefined;
            try {
              const displayNamePath = path.join(dir, 'display_name.txt');
              displayName = await fs.promises.readFile(
                displayNamePath,
                'utf-8',
              );
              displayName = displayName.trim();
            } catch (e) {
              // ignore
            }

            let lastGeneratedAt: string | undefined;
            try {
              let newestMtime = 0;
              const cppStat = await fs.promises
                .stat(path.join(dir, 'compile_commands.json'))
                .catch(() => null);
              const rustStat = await fs.promises
                .stat(path.join(dir, 'rust-project.json'))
                .catch(() => null);
              if (cppStat) newestMtime = Math.max(newestMtime, cppStat.mtimeMs);
              if (rustStat)
                newestMtime = Math.max(newestMtime, rustStat.mtimeMs);
              if (newestMtime > 0) {
                lastGeneratedAt = new Date(newestMtime).toISOString();
              }
            } catch (e) {
              // ignore
            }

            return new Target(
              subdir,
              dir,
              displayName,
              lastGeneratedAt,
              hasCpp,
              hasRust,
            );
          }),
        );

        return targets.filter((t): t is Target => t !== null);
      }),
    )
  )
    .flat()
    .filter((target) => target.name.trim() !== '.');

  // Get targets from the unprocessed mapping file.
  const unprocessedTargets = Object.entries(await loadUnprocessedMapping()).map(
    ([name, filePath]) => {
      const dir = path.dirname(path.join(workingDir.get(), filePath));
      return new Target(name, dir);
    },
  );

  // Combine all targets and sort them alphabetically for UI simplicity.
  return [...directoryTargets, ...unprocessedTargets].sort((a, b) =>
    a.name.localeCompare(b.name),
  );
}

export function getTarget(): Target | undefined {
  const targetName = settings.codeAnalysisTarget();
  if (targetName === undefined) return undefined;
  return new Target(targetName, settings.codeAnalysisTargetDir());
}

export async function baseSetTarget(target: Target): Promise<void> {
  await settings.codeAnalysisTarget(target.name);
  await settings.codeAnalysisTargetDir(target.dir);
}
