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

import * as vscode from 'vscode';
import { StatusBarItem } from 'vscode';

import { Disposable } from './disposables';

import {
  didChangeClangdConfig,
  didChangeRefreshStatus,
  didChangeTarget,
} from './events';

import { RefreshStatus } from './refreshManager';
import { settings } from './settings/vscode';
import { availableTargets } from './clangd/paths';
import { decodeBazelName } from './bazelUtils';

const DEFAULT_TARGET_TEXT = 'Select a Target';
const ICON_IDLE = '$(check)';
const ICON_FAULT = '$(warning)';
const ICON_INPROGRESS = '$(sync~spin)';

export class TargetStatusBarItem extends Disposable {
  private statusBarItem: StatusBarItem;
  private targetText = DEFAULT_TARGET_TEXT;
  private icon = ICON_IDLE;

  constructor() {
    super();

    this.statusBarItem = vscode.window.createStatusBarItem(
      vscode.StatusBarAlignment.Left,
      100,
    );

    // Seed the initial state, then make it visible
    this.updateTarget();
    this.updateRefreshStatus();
    this.statusBarItem.show();

    // Subscribe to relevant events
    didChangeTarget.event(this.updateTarget);
    didChangeRefreshStatus.event(this.updateRefreshStatus);

    // Dispose this when the extension is deactivated
    this.disposables.push(this.statusBarItem);
  }

  label = () => `${this.icon} ${this.targetText}`;

  updateProps = (
    props: Partial<{ command: string; icon: string; tooltip: string }> = {},
  ): void => {
    this.icon = props.icon ?? this.icon;
    this.statusBarItem.command = props.command ?? this.statusBarItem.command;
    this.statusBarItem.tooltip = props.tooltip ?? this.statusBarItem.tooltip;
    this.statusBarItem.text = this.label();
  };

  updateTarget = async (target?: string): Promise<void> => {
    const rawTarget =
      target ?? settings.codeAnalysisTarget() ?? DEFAULT_TARGET_TEXT;

    if (rawTarget === DEFAULT_TARGET_TEXT) {
      this.targetText = DEFAULT_TARGET_TEXT;
      this.updateProps();
      return;
    }

    const targets = await availableTargets();
    const match = targets.find((t) => t.name === rawTarget);

    const decodedName = decodeBazelName(rawTarget);

    if (match && match.displayName) {
      this.targetText = `${match.displayName} (${decodedName})`;
    } else {
      this.targetText = decodedName;
    }

    this.updateProps();
  };

  updateRefreshStatus = (status: RefreshStatus = 'idle'): void => {
    switch (status) {
      case 'idle':
        this.updateProps({
          command: 'pigweed.select-target',
          icon: ICON_IDLE,
          tooltip: 'Click to select a code analysis target',
        });
        break;
      case 'fault':
        this.updateProps({
          command: 'pigweed.select-target',
          icon: ICON_FAULT,
          tooltip: 'Click to select a code analysis target',
        });
        break;
      default:
        this.updateProps({
          command: 'pigweed.open-output-panel',
          icon: ICON_INPROGRESS,
          tooltip:
            'Refreshing compile commands. Click to open the output panel',
        });
        break;
    }
  };
}

export class InactiveVisibilityStatusBarItem extends Disposable {
  private statusBarItem: StatusBarItem;

  constructor() {
    super();

    this.statusBarItem = vscode.window.createStatusBarItem(
      vscode.StatusBarAlignment.Left,
      99,
    );

    // Seed the initial state, then make it visible
    this.update();
    this.statusBarItem.show();

    // Update state on clangd config change events
    didChangeClangdConfig.event(this.update);

    // Dispose this when the extension is deactivated
    this.disposables.push(this.statusBarItem);
  }

  update = (): void => {
    if (settings.disableInactiveFileCodeIntelligence()) {
      this.statusBarItem.text = '$(eye-closed)';

      this.statusBarItem.tooltip =
        'Code intelligence is disabled for files not in current ' +
        "target's build. Click to enable.";

      this.statusBarItem.command =
        'pigweed.enable-inactive-file-code-intelligence';
    } else {
      this.statusBarItem.text = '$(eye)';

      this.statusBarItem.tooltip =
        'Code intelligence is enabled for all files.' +
        "Click to disable for files not in current target's build.";

      this.statusBarItem.command =
        'pigweed.disable-inactive-file-code-intelligence';
    }
  };
}
