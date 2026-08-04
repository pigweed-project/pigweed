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

import { LitElement, PropertyValues, TemplateResult, html } from 'lit';
import { customElement, property, queryAll, state } from 'lit/decorators.js';
import {
  LogEntry,
  LogSourceEvent,
  SourceData,
  TableColumn,
} from '../shared/interfaces';
import {
  LocalStateStorage,
  LogViewerState,
  StateService,
} from '../shared/state';
import { ViewNode, NodeType } from '../shared/view-node';
import { styles } from './log-viewer.styles';
import { themeDark } from '../themes/dark';
import { themeLight } from '../themes/light';
import { LogView } from './log-view/log-view';
import { LogSource } from '../log-source';
import { LogStore } from '../log-store';
import CloseViewEvent from '../events/close-view';
import SplitViewEvent from '../events/split-view';
import InputChangeEvent from '../events/input-change';
import WrapToggleEvent from '../events/wrap-toggle';
import ColumnToggleEvent from '../events/column-toggle';
import ResizeColumnEvent from '../events/resize-column';

type ColorScheme = 'dark' | 'light';

/**
 * The root component which renders one or more log views for displaying
 * structured log entries.
 *
 * @element log-viewer
 */
@customElement('log-viewer')
export class LogViewer extends LitElement {
  static styles = [styles, themeDark, themeLight];

  logStore: LogStore;

  /** An array of log entries to be displayed. */
  @property({ type: Array })
  logs: LogEntry[] = [];

  @property({ type: Array })
  logSources: LogSource[] | LogSource = [];

  @property({ type: String, reflect: true })
  colorScheme?: ColorScheme;

  /**
   * Flag to determine whether Shoelace components should be used by
   * `LogViewer` and its subcomponents.
   */
  @property({ type: Boolean })
  useShoelaceFeatures = true;

  @state()
  _rootNode: ViewNode;

  /** An array that stores the preferred column order of columns  */
  @state()
  private _columnOrder: string[] = ['log_source', 'time', 'timestamp'];

  @queryAll('log-view') logViews!: LogView[];

  /** A map containing data from present log sources */
  private _sources: Map<string, SourceData> = new Map();

  private _sourcesArray: LogSource[] = [];

  private _lastUpdateTimeoutId: NodeJS.Timeout | undefined;

  private _stateService: StateService = new StateService(
    new LocalStateStorage(),
  );

  /**
   * Create a log-viewer
   * @param logSources - Collection of sources from where logs originate
   * @param options - Optional parameters to change default settings
   * @param options.columnOrder - defines column order between level and
   *   message undefined fields are added between defined order and message.
   * @param options.state - handles state between sessions, defaults to localStorage
   */
  constructor(
    logSources: LogSource[] | LogSource,
    options?: {
      columnOrder?: string[] | undefined;
      logStore?: LogStore | undefined;
      state?: LogViewerState | undefined;
    },
  ) {
    super();

    this.logSources = logSources;
    this.logStore = options?.logStore ?? new LogStore();
    this.logStore.setColumnOrder(this._columnOrder);

    const savedState = options?.state ?? this._stateService.loadState();
    this._rootNode =
      savedState?.rootNode || new ViewNode({ type: NodeType.View });
    if (options?.columnOrder) {
      this._columnOrder = [...new Set(options?.columnOrder)];
    }
    this.loadShoelaceComponents();
  }

  logEntryListener = (event: LogSourceEvent) => {
    if (event.type === 'log-entry') {
      const logEntry = event.data;
      this.logStore.addLogEntry(logEntry);
      this.logs = this.logStore.getLogs();

      if (this._lastUpdateTimeoutId) {
        clearTimeout(this._lastUpdateTimeoutId);
      }

      // Call requestUpdate at most once every 100 milliseconds.
      this._lastUpdateTimeoutId = setTimeout(() => {
        this.logs = [...this.logStore.getLogs()];
      }, 100);
    }
  };

  connectedCallback() {
    super.connectedCallback();
    this.addEventListener('close-view', this.handleCloseView);

    this._sourcesArray = Array.isArray(this.logSources)
      ? this.logSources
      : [this.logSources];
    this._sourcesArray.forEach((logSource: LogSource) => {
      logSource.addEventListener('log-entry', this.logEntryListener);
    });

    // If color scheme isn't set manually, retrieve it from localStorage
    if (!this.colorScheme) {
      const storedScheme = localStorage.getItem(
        'colorScheme',
      ) as ColorScheme | null;
      if (storedScheme) {
        this.colorScheme = storedScheme;
      }
    }
  }

  firstUpdated() {
    this.delSevFromState(this._rootNode);
  }

  updated(changedProperties: PropertyValues) {
    super.updated(changedProperties);

    if (changedProperties.has('colorScheme') && this.colorScheme) {
      // Only store in localStorage if color scheme is 'dark' or 'light'
      if (this.colorScheme === 'light' || this.colorScheme === 'dark') {
        localStorage.setItem('colorScheme', this.colorScheme);
      } else {
        localStorage.removeItem('colorScheme');
      }
    }

    if (changedProperties.has('logs')) {
      this.logs.forEach((logEntry) => {
        if (logEntry.sourceData && !this._sources.has(logEntry.sourceData.id)) {
          this._sources.set(logEntry.sourceData.id, logEntry.sourceData);
        }
      });
    }
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this.removeEventListener('close-view', this.handleCloseView);

    this._sourcesArray.forEach((logSource: LogSource) => {
      logSource.removeEventListener('log-entry', this.logEntryListener);
    });

    // Save state before disconnecting
    this._stateService.saveState({ rootNode: this._rootNode });
  }

  /**
   * Conditionally loads Shoelace components
   */
  async loadShoelaceComponents() {
    if (this.useShoelaceFeatures) {
      await import('@shoelace-style/shoelace/dist/components/split-panel/split-panel.js');
    }
  }

  private splitLogView(event: SplitViewEvent) {
    const { parentId, orientation, columnData, searchText, viewTitle } =
      event.detail;

    // Find parent node, handle errors if not found
    const parentNode = this.findNodeById(this._rootNode, parentId);
    if (!parentNode) {
      console.error('Parent node not found for split:', parentId);
      return;
    }

    // Create `ViewNode`s with inherited or provided data
    const newView = new ViewNode({
      type: NodeType.View,
      logViewId: crypto.randomUUID(),
      columnData: JSON.parse(
        JSON.stringify(columnData || parentNode.logViewState?.columnData),
      ),
      searchText: searchText || parentNode.logViewState?.searchText,
      viewTitle: viewTitle || parentNode.logViewState?.viewTitle,
    });

    // Both views receive the same values for `searchText` and `columnData`
    const originalView = new ViewNode({
      type: NodeType.View,
      logViewId: crypto.randomUUID(),
      columnData: JSON.parse(JSON.stringify(newView.logViewState?.columnData)),
      searchText: newView.logViewState?.searchText,
    });

    parentNode.type = NodeType.Split;
    parentNode.orientation = orientation;
    parentNode.children = [originalView, newView];

    this._stateService.saveState({ rootNode: this._rootNode });

    this.requestUpdate();
  }

  private findNodeById(node: ViewNode, id: string): ViewNode | undefined {
    if (node.logViewId === id) {
      return node;
    }

    // Recursively search through children `ViewNode`s for a match
    for (const child of node.children) {
      const found = this.findNodeById(child, id);
      if (found) {
        return found;
      }
    }
    return undefined;
  }

  /**
   * Removes a log view when its Close button is clicked.
   *
   * @param event The event object dispatched by the log view controls.
   */
  private handleCloseView(event: CloseViewEvent) {
    const viewId = event.detail.viewId;

    const removeViewNode = (node: ViewNode, id: string): boolean => {
      let nodeIsFound = false;

      node.children.forEach((child, index) => {
        if (nodeIsFound) return;

        if (child.logViewId === id) {
          node.children.splice(index, 1); // Remove the targeted view
          if (node.children.length === 1) {
            // Flatten the node if only one child remains
            const remainingChild = node.children[0];
            Object.assign(node, remainingChild);
          }
          nodeIsFound = true;
        } else {
          nodeIsFound = removeViewNode(child, id);
        }
      });
      return nodeIsFound;
    };

    if (removeViewNode(this._rootNode, viewId)) {
      this._stateService.saveState({ rootNode: this._rootNode });
    }

    this.requestUpdate();
  }

  private handleViewEvent(
    event:
      | InputChangeEvent
      | ColumnToggleEvent
      | ResizeColumnEvent
      | WrapToggleEvent,
  ) {
    const { viewId } = event.detail;
    const nodeToUpdate = this.findNodeById(this._rootNode, viewId);

    if (!nodeToUpdate) {
      return;
    }

    if (event.type === 'wrap-toggle') {
      const { isChecked } = (event as WrapToggleEvent).detail;
      if (nodeToUpdate.logViewState) {
        nodeToUpdate.logViewState.wordWrap = isChecked;
        this._stateService.saveState({ rootNode: this._rootNode });
      }
    }

    if (event.type === 'input-change') {
      const { inputValue } = (event as InputChangeEvent).detail;
      if (nodeToUpdate.logViewState) {
        nodeToUpdate.logViewState.searchText = inputValue;
        this._stateService.saveState({ rootNode: this._rootNode });
      }
    }

    if (event.type === 'resize-column' || event.type === 'column-toggle') {
      const { columnData } = (event as ResizeColumnEvent).detail;
      if (nodeToUpdate.logViewState) {
        nodeToUpdate.logViewState.columnData = columnData;
        this._stateService.saveState({ rootNode: this._rootNode });
      }
    }
  }

  /**
   * Handles case if switching from level -> severity -> level, state will be
   * restructured to remove severity and move up level if it exists.
   *
   * @param node The state node.
   */
  private delSevFromState(node: ViewNode) {
    if (node.logViewState?.columnData) {
      const fields = node.logViewState?.columnData.map(
        (field) => field.fieldName,
      );

      if (fields?.includes('level')) {
        const index = fields.indexOf('level');
        if (index !== 0) {
          const level = node.logViewState?.columnData[index] as TableColumn;
          node.logViewState?.columnData.splice(index, 1);
          node.logViewState?.columnData.unshift(level);
        }
      }

      if (fields?.includes('severity')) {
        const index = fields.indexOf('severity');
        node.logViewState?.columnData.splice(index, 1);
      }
    }

    if (node.type === 'split') {
      node.children.forEach((child) => this.delSevFromState(child));
    }
  }

  private renderNodes(node: ViewNode): TemplateResult {
    if (node.type === NodeType.View || !this.useShoelaceFeatures) {
      return html`<log-view
        id=${node.logViewId ?? ''}
        .logs=${this.logs}
        .sources=${this._sources}
        .isOneOfMany=${this._rootNode.children.length > 1}
        .columnOrder=${this._columnOrder}
        .searchText=${node.logViewState?.searchText ?? ''}
        .columnData=${node.logViewState?.columnData ?? []}
        .viewTitle=${node.logViewState?.viewTitle || ''}
        .lineWrap=${node.logViewState?.wordWrap ?? true}
        .useShoelaceFeatures=${this.useShoelaceFeatures}
        @split-view="${this.splitLogView}"
        @input-change="${this.handleViewEvent}"
        @wrap-toggle="${this.handleViewEvent}"
        @resize-column="${this.handleViewEvent}"
        @column-toggle="${this.handleViewEvent}"
      ></log-view>`;
    } else {
      const [startChild, endChild] = node.children;
      return html`<sl-split-panel ?vertical=${node.orientation === 'vertical'}>
        ${
          startChild
            ? html`<div slot="start">${this.renderNodes(startChild)}</div>`
            : ''
        }
        ${
          endChild
            ? html`<div slot="end">${this.renderNodes(endChild)}</div>`
            : ''
        }
      </sl-split-panel>`;
    }
  }

  render() {
    return html`${this.renderNodes(this._rootNode)}`;
  }
}

// Manually register Log View component due to conditional rendering
if (!customElements.get('log-view')) {
  customElements.define('log-view', LogView);
}

declare global {
  interface HTMLElementTagNameMap {
    'log-viewer': LogViewer;
  }
}
