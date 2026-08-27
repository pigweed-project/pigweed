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

import { LogViewer as RootComponent } from './components/log-viewer';
import { LogViewerState } from './shared/state';
import { LogSource } from './log-source';
import { LogStore } from './log-store';

import '@material/web/button/filled-button.js';
import '@material/web/button/outlined-button.js';
import '@material/web/checkbox/checkbox.js';
import '@material/web/field/outlined-field.js';
import '@material/web/textfield/outlined-text-field.js';
import '@material/web/textfield/filled-text-field.js';
import '@material/web/icon/icon.js';
import '@material/web/iconbutton/icon-button.js';
import '@material/web/menu/menu.js';
import '@material/web/menu/sub-menu.js';
import '@material/web/menu/menu-item.js';

/**
 * Create an instance of log-viewer
 * @param logSources - Collection of sources from where logs originate
 * @param root - HTML component to append log-viewer to
 * @param options - Optional parameters to change default settings
 * @param options.columnOrder - Defines column order between level and
 *   message. Undefined fields are added between defined order and message.
 * @param options.logStore - Stores and handles management of all logs
 * @param options.state - Handles state between sessions, defaults to localStorage
 */
export function createLogViewer(
  logSources: LogSource[] | LogSource,
  root: HTMLElement,
  options?: {
    columnOrder?: string[] | undefined;
    logSources?: LogSource | LogSource[] | undefined;
    logStore?: LogStore | undefined;
    state?: LogViewerState | undefined;
  },
) {
  const logViewer = new RootComponent(logSources, options);
  root.appendChild(logViewer);

  // Method to destroy and unsubscribe
  return () => {
    if (logViewer.parentNode) {
      logViewer.parentNode.removeChild(logViewer);
    }
  };
}
