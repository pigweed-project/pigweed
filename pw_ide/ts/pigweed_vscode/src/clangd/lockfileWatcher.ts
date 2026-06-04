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

import { Disposable } from '../disposables';
import logger from '../logging';
import { WebviewProvider } from '../webviewProvider';
import { LockManager } from './lock';

export class LockfileWatcher extends Disposable {
  private timer: NodeJS.Timeout | undefined;
  private lockManager: LockManager;
  private lastExistsState = false;

  constructor(
    private webviewProvider: WebviewProvider,
    lockManager?: LockManager,
  ) {
    super();
    this.lockManager = lockManager || new LockManager();
    logger.info(
      `Initializing lockfile watcher for ${this.lockManager.lockfilePath}`,
    );

    // Initial check
    this.pollLockfile();

    // Poll every 1s
    this.timer = setInterval(() => this.pollLockfile(), 1000);
    this.disposables.push({ dispose: () => clearInterval(this.timer) });
  }

  private pollLockfile() {
    const exists = this.lockManager.isLocked();

    if (exists !== this.lastExistsState) {
      this.lastExistsState = exists;
      logger.info(`[LockfileWatcher] lockfile state changed: ${exists}`);
      this.webviewProvider.refresh();
    }
  }
}
