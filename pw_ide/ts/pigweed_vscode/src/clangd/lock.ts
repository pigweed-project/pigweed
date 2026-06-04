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

import * as fs from 'fs';
import * as path from 'path';
import { CDB_FILE_DIR } from './paths';
import { workingDir } from '../settings/vscode';
import logger from '../logging';

export class LockManager {
  constructor(
    private fsObj = fs,
    private checkPidAlive = (pid: number): boolean => {
      try {
        process.kill(pid, 0);
        return true;
      } catch (e: any) {
        return e.code !== 'ESRCH';
      }
    },
  ) {}

  get lockfilePath(): string {
    return path.join(workingDir.get(), CDB_FILE_DIR, 'pw.lock');
  }

  isLocked(): boolean {
    if (!this.fsObj.existsSync(this.lockfilePath)) {
      return false;
    }

    try {
      const pidStr = this.fsObj.readFileSync(this.lockfilePath, 'utf-8').trim();
      if (!pidStr) {
        // Empty file (potentially in transition/being written). Assume locked.
        return true;
      }

      const pid = parseInt(pidStr, 10);
      if (isNaN(pid)) {
        return true;
      }

      return this.checkPidAlive(pid);
    } catch (e) {
      // If reading fails, assume locked to be safe
      return true;
    }
  }
}
