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

import { LitElement, html, PropertyValues, TemplateResult } from 'lit';
import {
  customElement,
  property,
  query,
  queryAll,
  state,
} from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import { styles } from './log-list.styles';
import { LogEntry, Level, TableColumn } from '../../shared/interfaces';
import { virtualize } from '@lit-labs/virtualizer/virtualize.js';
import '@lit-labs/virtualizer';
import { debounce } from '../../utils/debounce';
import { throttle } from '../../utils/throttle';

/**
 * A sub-component of the log view which takes filtered logs and renders them in
 * a virtualized HTML table.
 *
 * @element log-list
 */
@customElement('log-list')
export class LogList extends LitElement {
  static styles = styles;

  /** The `id` of the parent view containing this log list. */
  @property()
  viewId = '';

  /** An array of log entries to be displayed. */
  @property({ type: Array })
  logs: LogEntry[] = [];

  /** A string representing the value contained in the search field. */
  @property({ type: String })
  searchText = '';

  /** Whether line wrapping in table cells should be used. */
  @property({ type: Boolean })
  lineWrap = true;

  @state()
  columnData: TableColumn[] = [];

  /** Indicates whether the table content is overflowing to the right. */
  @state()
  private _isOverflowingToRight = false;

  /**
   * Indicates whether to automatically scroll the table container to the bottom
   * when new log entries are added.
   */
  @state()
  private _autoscrollIsEnabled = true;

  /** A number representing the scroll percentage in the horizontal direction. */
  @state()
  private _scrollPercentageLeft = 0;

  @query('.table-container') private _tableContainer!: HTMLDivElement;
  @query('table') private _table!: HTMLTableElement;
  @query('tbody') private _tableBody!: HTMLTableSectionElement;
  @queryAll('tr') private _tableRows!: HTMLTableRowElement[];

  /** The zoom level based on pixel ratio of the window  */
  private _zoomLevel: number = Math.round(window.devicePixelRatio * 100);

  /** Indicates whether to enable autosizing of incoming log entries. */
  private _autosizeLocked = false;

  /** The number of times the `logs` array has been updated. */
  private logUpdateCount = 0;

  /** The last known vertical scroll position of the table container. */
  private lastScrollTop = 0;

  /** The maximum number of log entries to render in the list. */
  private readonly MAX_ENTRIES = 100_000;

  /** The maximum number of log updates until autosize is disabled. */
  private readonly AUTOSIZE_LIMIT: number = 8;

  /** The minimum width (in px) for table columns. */
  private readonly MIN_COL_WIDTH: number = 52;

  /** The minimum width (in px) for table columns. */
  private readonly LAST_COL_MIN_WIDTH: number = 250;

  /** The delay (in ms) for debouncing column resizing */
  private readonly RESIZE_DEBOUNCE_DELAY = 10;

  /**
   * Data used for column resizing including the column index, the starting
   * mouse position (X-coordinate), and the initial width of the column.
   */
  private columnResizeData: {
    columnIndex: number;
    startX: number;
    startWidth: number;
  } | null = null;

  firstUpdated() {
    this._tableContainer.addEventListener('scroll', this.handleTableScroll);
    this._tableBody.addEventListener('rangeChanged', this.onRangeChanged);

    const newRowObserver = new MutationObserver(this.onTableRowAdded);
    newRowObserver.observe(this._table, {
      childList: true,
      subtree: true,
    });
  }

  updated(changedProperties: PropertyValues) {
    super.updated(changedProperties);

    if (
      changedProperties.has('offsetWidth') ||
      changedProperties.has('scrollWidth')
    ) {
      this.updateHorizontalOverflowState();
    }

    if (changedProperties.has('logs')) {
      this.logUpdateCount++;
      this.handleTableScroll();
    }

    if (changedProperties.has('columnData')) {
      this.updateColumnWidths(this.generateGridTemplateColumns());
      this.updateHorizontalOverflowState();
      this.requestUpdate();
    }
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this._tableContainer.removeEventListener('scroll', this.handleTableScroll);
    this._tableBody.removeEventListener('rangeChanged', this.onRangeChanged);
  }

  private onTableRowAdded = () => {
    if (!this._autosizeLocked) {
      this.autosizeColumns();
    }

    // Disable auto-sizing once a certain number of updates to the logs array have been made
    if (this.logUpdateCount >= this.AUTOSIZE_LIMIT) {
      this._autosizeLocked = true;
    }
  };

  /** Called when the Lit virtualizer updates its range of entries. */
  private onRangeChanged = () => {
    if (this._autoscrollIsEnabled) {
      this.scrollTableToBottom();
    }
  };

  /** Scrolls to the bottom of the table container. */
  private scrollTableToBottom() {
    const container = this._tableContainer;

    // TODO: b/298097109 - Refactor `setTimeout` usage
    setTimeout(() => {
      container.scrollTop = container.scrollHeight;
    }, 0); // Complete any rendering tasks before scrolling
  }

  private onJumpToBottomButtonClick() {
    this._autoscrollIsEnabled = true;
    this.scrollTableToBottom();
  }

  /**
   * Calculates the maximum column widths for the table and updates the table
   * rows.
   */
  private autosizeColumns = (rows = this._tableRows) => {
    // Iterate through each row to find the maximum width in each column
    const visibleColumnData = this.columnData.filter(
      (column) => column.isVisible,
    );

    rows.forEach((row) => {
      const cells = Array.from(row.children).filter(
        (cell) => !cell.hasAttribute('hidden'),
      ) as HTMLTableCellElement[];

      cells.forEach((cell, columnIndex) => {
        if (visibleColumnData[columnIndex].fieldName == 'level') return;

        const textLength = cell.textContent?.trim().length || 0;

        if (!this._autosizeLocked) {
          // Update the preferred width if it's smaller than the new one
          if (visibleColumnData[columnIndex]) {
            visibleColumnData[columnIndex].characterLength = Math.max(
              visibleColumnData[columnIndex].characterLength,
              textLength,
            );
          } else {
            // Initialize if the column data for this index does not exist
            visibleColumnData[columnIndex] = {
              fieldName: '',
              characterLength: textLength,
              manualWidth: null,
              isVisible: true,
            };
          }
        }
      });
    });

    this.updateColumnWidths(this.generateGridTemplateColumns());
    const resizeColumn = new CustomEvent('resize-column', {
      bubbles: true,
      composed: true,
      detail: {
        viewId: this.viewId,
        columnData: this.columnData,
      },
    });

    this.dispatchEvent(resizeColumn);
  };

  private generateGridTemplateColumns(
    newWidth?: number,
    resizingIndex?: number,
  ): string {
    let gridTemplateColumns = '';

    let lastVisibleCol = -1;
    for (let i = this.columnData.length - 1; i >= 0; i--) {
      if (this.columnData[i].isVisible) {
        lastVisibleCol = i;
        break;
      }
    }

    const calculateColumnWidth = (col: TableColumn, i: number) => {
      const chWidth = col.characterLength;
      const padding = 24 + 1; // +1 pixel to avoid ellipsis jitter when highlighting text

      if (i === resizingIndex) {
        if (i === lastVisibleCol) {
          return `minmax(${newWidth}px, 1fr)`;
        }
        return `${newWidth}px`;
      }
      if (col.manualWidth !== null) {
        if (i === lastVisibleCol) {
          return `minmax(${col.manualWidth}px, 1fr)`;
        }
        return `${col.manualWidth}px`;
      }
      if (i === 0) {
        return `calc(var(--sys-log-viewer-table-cell-icon-size) + 1rem)`;
      }
      if (i === lastVisibleCol) {
        return `minmax(${this.LAST_COL_MIN_WIDTH}px, 1fr)`;
      }
      return `clamp(${this.MIN_COL_WIDTH}px, ${chWidth}ch + ${padding}px, 80ch)`;
    };

    this.columnData.forEach((column, i) => {
      if (column.isVisible) {
        const columnValue = calculateColumnWidth(column, i);
        gridTemplateColumns += columnValue + ' ';
      }
    });

    return gridTemplateColumns.trim();
  }

  private updateColumnWidths(gridTemplateColumns: string) {
    this.style.setProperty('--column-widths', gridTemplateColumns);
  }

  /**
   * Highlights text content within the table cell based on the current filter
   * value.
   *
   * @param {string} text - The table cell text to be processed.
   */
  private highlightMatchedText(text: string): TemplateResult[] {
    if (!this.searchText) {
      return [html`${text}`];
    }

    const searchPhrase = this.searchText?.replace(/(^"|')|("|'$)/g, '');
    const escapedsearchText = searchPhrase.replace(
      /[.*+?^${}()|[\]\\]/g,
      '\\$&',
    );
    const regex = new RegExp(`(${escapedsearchText})`, 'gi');
    const parts = text.split(regex);
    return parts.map((part) =>
      regex.test(part) ? html`<mark>${part}</mark>` : html`${part}`,
    );
  }

  /** Updates horizontal overflow state. */
  private updateHorizontalOverflowState() {
    const containerWidth = this.offsetWidth;
    const tableWidth = this._tableContainer.scrollWidth;

    this._isOverflowingToRight = tableWidth > containerWidth;
  }

  /**
   * Calculates scroll-related properties and updates the component's state when
   * the user scrolls the table.
   */
  private handleTableScroll = () => {
    const container = this._tableContainer;
    const currentScrollTop = container.scrollTop;
    const containerWidth = container.offsetWidth;
    const scrollLeft = container.scrollLeft;
    const scrollY =
      container.scrollHeight - currentScrollTop - container.clientHeight;
    const maxScrollLeft = container.scrollWidth - containerWidth;

    // Determine scroll direction and update the last known scroll position
    const isScrollingVertically = currentScrollTop !== this.lastScrollTop;
    const isScrollingUp = currentScrollTop < this.lastScrollTop;
    this.lastScrollTop = currentScrollTop;

    const logsAreCleared = this.logs.length == 0;
    const zoomChanged =
      this._zoomLevel !== Math.round(window.devicePixelRatio * 100);

    if (logsAreCleared) {
      this._autoscrollIsEnabled = true;
      return;
    }

    // Do not change autoscroll if zoom level on window changed
    if (zoomChanged) {
      this._zoomLevel = Math.round(window.devicePixelRatio * 100);
      return;
    }

    // Calculate horizontal scroll percentage
    if (!isScrollingVertically) {
      this._scrollPercentageLeft = scrollLeft / maxScrollLeft || 0;
      return;
    }

    // Scroll direction up, disable autoscroll
    if (isScrollingUp && Math.abs(scrollY) > 1) {
      this._autoscrollIsEnabled = false;
      return;
    }

    // Scroll direction down, enable autoscroll if near the bottom
    if (Math.abs(scrollY) <= 1) {
      this._autoscrollIsEnabled = true;
      return;
    }
  };

  /**
   * Handles column resizing.
   *
   * @param {MouseEvent} event - The mouse event triggered during column
   *   resizing.
   * @param {number} columnIndex - An index specifying the column being resized.
   */
  private handleColumnResizeStart(event: MouseEvent, columnIndex: number) {
    event.preventDefault();

    // Check if the corresponding index in columnData is not visible. If not,
    // check the columnIndex - 1th element until one isn't hidden.
    while (
      this.columnData[columnIndex] &&
      !this.columnData[columnIndex].isVisible
    ) {
      columnIndex--;
      if (columnIndex < 0) {
        // Exit the loop if we've checked all possible columns
        return;
      }
    }

    // If no visible columns are found, return early
    if (columnIndex < 0) return;

    const startX = event.clientX;
    const columnHeader = this._table.querySelector(
      `th:nth-child(${columnIndex + 1})`,
    ) as HTMLTableCellElement;

    if (!columnHeader) return;

    const startWidth = columnHeader.offsetWidth;

    this.columnResizeData = {
      columnIndex: columnIndex,
      startX,
      startWidth,
    };

    const handleColumnResize = throttle((event: MouseEvent) => {
      this.handleColumnResize(event);
    }, 16);

    const handleColumnResizeEnd = () => {
      this.columnResizeData = null;
      document.removeEventListener('mousemove', handleColumnResize);
      document.removeEventListener('mouseup', handleColumnResizeEnd);

      // Communicate column data changes back to parent Log View
      const resizeColumn = new CustomEvent('resize-column', {
        bubbles: true,
        composed: true,
        detail: {
          viewId: this.viewId,
          columnData: this.columnData,
        },
      });

      this.dispatchEvent(resizeColumn);
    };

    document.addEventListener('mousemove', handleColumnResize);
    document.addEventListener('mouseup', handleColumnResizeEnd);
  }

  /**
   * Adjusts the column width during a column resize.
   *
   * @param {MouseEvent} event - The mouse event object.
   */
  private handleColumnResize(event: MouseEvent) {
    if (!this.columnResizeData) return;

    const { columnIndex, startX, startWidth } = this.columnResizeData;
    const offsetX = event.clientX - startX;
    const newWidth =
      this.columnData.length - 1 === columnIndex
        ? Math.max(startWidth + offsetX, this.LAST_COL_MIN_WIDTH)
        : Math.max(startWidth + offsetX, this.MIN_COL_WIDTH);

    // Ensure the column index exists in columnData
    if (this.columnData[columnIndex]) {
      this.columnData[columnIndex].manualWidth = newWidth;
    }

    const generateGridTemplateColumns = debounce(() => {
      const gridTemplateColumns = this.generateGridTemplateColumns(
        newWidth,
        columnIndex,
      );
      this.updateColumnWidths(gridTemplateColumns);
    }, this.RESIZE_DEBOUNCE_DELAY);

    generateGridTemplateColumns();
  }

  render() {
    const logsDisplayed: LogEntry[] = this.logs.slice(0, this.MAX_ENTRIES);

    return html`
      <div
        class="table-container"
        role="log"
        @scroll="${this.handleTableScroll}"
      >
        <table>
          <thead>
            ${this.tableHeaderRow()}
          </thead>

          <tbody>
            ${virtualize({
              items: logsDisplayed,
              renderItem: (log) => html`${this.tableDataRow(log)}`,
            })}
          </tbody>
        </table>
        ${this.overflowIndicators()} ${this.jumpToBottomButton()}
      </div>
    `;
  }

  private tableHeaderRow() {
    return html`
      <tr>
        ${this.columnData.map((columnData, columnIndex) =>
          this.tableHeaderCell(
            columnData.fieldName,
            columnIndex,
            columnData.isVisible,
          ),
        )}
      </tr>
    `;
  }

  private tableHeaderCell(
    fieldKey: string,
    columnIndex: number,
    isVisible: boolean,
  ) {
    return html`
      <th title="${fieldKey}" ?hidden=${!isVisible}>
        ${fieldKey} ${this.resizeHandle(columnIndex)}
      </th>
    `;
  }

  private resizeHandle(columnIndex: number) {
    if (columnIndex === 0) {
      return html`
        <span class="resize-handle" style="pointer-events: none"></span>
      `;
    }

    return html`
      <span
        class="resize-handle"
        @mousedown="${(event: MouseEvent) =>
          this.handleColumnResizeStart(event, columnIndex)}"
      ></span>
    `;
  }

  private tableDataRow(log: LogEntry) {
    const classes = {
      'log-row': true,
      'log-row--nowrap': !this.lineWrap,
    };
    const logLevelClass = ('log-row--' +
      (log?.level || Level.INFO).toLowerCase()) as keyof typeof classes;
    classes[logLevelClass] = true;

    return html`
      <tr class="${classMap(classes)}">
        ${this.columnData.map((columnData, columnIndex) =>
          this.tableDataCell(
            log,
            columnData.fieldName,
            columnIndex,
            columnData.isVisible,
          ),
        )}
      </tr>
    `;
  }

  private tableDataCell(
    log: LogEntry,
    fieldKey: string,
    columnIndex: number,
    isVisible: boolean,
  ) {
    const field = log.fields.find((f) => f.key === fieldKey) || {
      key: fieldKey,
      value: '',
    };

    if (field.key == 'level') {
      const levelIcons = new Map<Level, string>([
        [Level.INFO, `\ue88e`],
        [Level.WARNING, '\uf083'],
        [Level.ERROR, '\ue888'],
        [Level.CRITICAL, '\uf5cf'],
        [Level.DEBUG, '\ue868'],
      ]);

      const levelValue: Level = field.value
        ? (field.value as Level)
        : log.level
          ? log.level
          : Level.INFO;
      const iconId = levelIcons.get(levelValue) || '';
      const toTitleCase = (input: string): string => {
        return input.replace(/\b\w+/g, (match) => {
          return match.charAt(0).toUpperCase() + match.slice(1).toLowerCase();
        });
      };

      return html`
        <td class="level-cell" ?hidden=${!isVisible}>
          <div class="cell-content">
            <md-icon
              class="cell-icon"
              title="${toTitleCase(field.value.toString())}"
            >
              ${iconId}
            </md-icon>
          </div>
          ${this.resizeHandle(columnIndex)}
        </td>
      `;
    }

    return html`
      <td ?hidden=${!isVisible}>
        <div class="cell-content">
          <span class="cell-text"
            >${
              field.value
                ? this.highlightMatchedText(field.value.toString())
                : ''
            }</span
          >
        </div>
        ${this.resizeHandle(columnIndex)}
      </td>
    `;
  }

  private overflowIndicators = () => html`
    <div
      class="bottom-indicator"
      data-visible="${this._autoscrollIsEnabled ? 'false' : 'true'}"
    ></div>

    <div
      class="overflow-indicator left-indicator"
      style="opacity: ${this._scrollPercentageLeft - 0.5}"
      ?hidden="${!this._isOverflowingToRight}"
    ></div>

    <div
      class="overflow-indicator right-indicator"
      style="opacity: ${1 - this._scrollPercentageLeft - 0.5}"
      ?hidden="${!this._isOverflowingToRight}"
    ></div>
  `;

  private jumpToBottomButton = () => html`
    <md-filled-button
      class="jump-to-bottom-btn"
      title="Jump to Bottom"
      @click="${this.onJumpToBottomButtonClick}"
      leading-icon
      data-visible="${this._autoscrollIsEnabled ? 'false' : 'true'}"
    >
      <md-icon slot="icon" aria-hidden="true">&#xe5db;</md-icon>
      Jump to Bottom
    </md-filled-button>
  `;
}
