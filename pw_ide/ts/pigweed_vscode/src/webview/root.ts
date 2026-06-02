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

import { html, css, LitElement } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { decodeBazelName } from './bazelUtils';

interface vscode {
  postMessage(message: { type: string; data?: any }): void;
}
// declare const vscode: vscode;
declare function acquireVsCodeApi(): vscode;
type ExtensionData = {
  recommended: { id: string; installed: boolean; name: string }[];
  unwanted: { id: string; installed: boolean; name: string }[];
};

type CipdReport = {
  clangdPath?: string;
  bazelPath?: string;
  targetSelected?: string;
  isCompileCommandsGenerated?: boolean;
  compileCommandsPath?: string;
  lastBuildPlatformCount?: number;
  activeFileCount?: number;
  availableTargets?: { name: string; displayName?: string }[];
  preconfiguredTargets?: { label: string; displayName?: string }[];
};

const vscode = acquireVsCodeApi();

@customElement('app-root')
export class Root extends LitElement {
  static styles = css`
    :host {
      display: block;
      border: solid 1px gray;
      padding: 16px;
      max-width: 800px;
    }
    .target-list {
      list-style-type: none;
      padding: 0;
      margin: 8px 0;
    }
    .target-list li {
      margin-bottom: 4px;
    }
  `;

  @state() extensionData: ExtensionData = { unwanted: [], recommended: [] };
  @state() cipdReport: CipdReport = {};
  @state() selectedPreconfiguredTarget = '';

  createRenderRoot() {
    return this;
  }

  connectedCallback() {
    super.connectedCallback();

    // Initialize selectedPreconfiguredTarget if available
    if (
      this.cipdReport.preconfiguredTargets &&
      this.cipdReport.preconfiguredTargets.length > 0
    ) {
      this.selectedPreconfiguredTarget =
        this.cipdReport.preconfiguredTargets[0].label;
    }
  }

  private _handlePreconfiguredTargetChange(event: Event) {
    const selectElement = event.target as HTMLSelectElement;
    this.selectedPreconfiguredTarget = selectElement.value;
  }

  private _runPreconfiguredTarget(e?: MouseEvent) {
    e?.preventDefault();
    if (this.selectedPreconfiguredTarget) {
      vscode.postMessage({
        type: 'runPreconfiguredTarget',
        data: this.selectedPreconfiguredTarget,
      });
    }
  }

  private _selectTarget(e: Event) {
    const select = e.target as HTMLSelectElement;
    vscode.postMessage({ type: 'selectTarget', data: select.value });
  }

  private get _isCodeIntelligenceHealthy(): boolean {
    return !!(
      this.cipdReport.clangdPath &&
      this.cipdReport.bazelPath &&
      this.cipdReport.targetSelected &&
      this.cipdReport.isCompileCommandsGenerated
    );
  }

  private _getTargetDisplayName(name?: string): string {
    if (!name) return 'None';
    const target = this.cipdReport.availableTargets?.find(
      (t) => t.name === name,
    );
    return (
      target?.displayName || name.replace(/____/g, '//').replace(/__/g, ':')
    );
  }

  private get _selectedTargetDisplayName(): string {
    return this._getTargetDisplayName(this.cipdReport.targetSelected);
  }

  private _openDebugDetails(e: MouseEvent) {
    e.preventDefault();
    const mainDetails = this.renderRoot.querySelector(
      '#code-intelligence-details',
    ) as HTMLDetailsElement;
    const debugDetails = this.renderRoot.querySelector(
      '#debug-code-intelligence-details',
    ) as HTMLDetailsElement;

    if (mainDetails) {
      mainDetails.open = true;
    }
    if (debugDetails) {
      debugDetails.open = true;
    }
  }

  private _renderCodeIntelligenceStatus() {
    const header = html`<h3>Pigweed C++ Code Intelligence</h3>
      <p class="description">
        Provides C++ code intelligence features like 'Go to Definition' and
        hover help, with accurate results tailored to your selected build
        platform. Learn how
        <a
          href="#"
          @click=${(e: MouseEvent) => {
            e.preventDefault();
            vscode.postMessage({ type: 'openDocs' });
          }}
          >Pigweed's C++ code intelligence works</a
        >.
      </p>`;

    const isPreconfigured =
      this.cipdReport.preconfiguredTargets &&
      this.cipdReport.preconfiguredTargets.length > 0;

    // Loading state
    if (Object.keys(this.cipdReport).length === 0) {
      return html` <div class="code-intelligence-status-card">
        ${header}
        <div class="status-line status-info">
          <span>ℹ️</span>
          <span>Loading...</span>
        </div>
      </div>`;
    }

    const activeFileCount = this.cipdReport.activeFileCount || 0;
    const activeFileText = `${activeFileCount} file${
      activeFileCount === 1 ? '' : 's'
    } on the current platform`;

    // Healthy state
    if (this._isCodeIntelligenceHealthy) {
      return html` <div class="code-intelligence-status-card">
        ${header}
        <div class="status-line status-success">
          <span>✅</span>
          <span
            >${isPreconfigured
              ? html`<span style="color: gray"
                  >Code intelligence is <b>preconfigured</b> in
                  <code>BUILD.bazel</code></span
                >`
              : 'Code intelligence is configured and working'}
            (<a href="#" @click=${this._openDebugDetails}>see details</a
            >).</span
          >
        </div>
        <ol class="status-steps">
          <li>
            <b>Generate compile commands</b>
            <div class="step-detail">
              ${isPreconfigured
                ? html`
                    Select a target to generate compile commands:
                    <div class="target-selection-row">
                      <div class="vscode-select">
                        <select
                          @change=${this._handlePreconfiguredTargetChange}
                        >
                          ${this.cipdReport.preconfiguredTargets?.map(
                            (target) => html`
                              <option
                                value=${target.label}
                                ?selected=${target.label ===
                                this.selectedPreconfiguredTarget}
                              >
                                ${target.displayName || target.label}
                              </option>
                            `,
                          )}
                        </select>
                      </div>
                      <div
                        class="vscode-button"
                        role="button"
                        tabindex="0"
                        @click=${this._runPreconfiguredTarget}
                        @keydown=${(e: KeyboardEvent) => {
                          if (e.key === 'Enter' || e.key === ' ') {
                            this._runPreconfiguredTarget();
                          }
                        }}
                      >
                        Generate
                      </div>
                    </div>
                  `
                : html`
                    <p style="margin-top:0;">
                      No preconfigured targets found. Please configure a
                      <code>pw_compile_commands_generator</code> target in your
                      <code>BUILD.bazel</code> to enable C++ code intelligence.
                    </p>
                    <p>
                      See the
                      <a
                        href="https://pigweed.dev/pw_ide/guide/"
                        target="_blank"
                        >Pigweed IDE Guide</a
                      >
                      for more details.
                    </p>
                  `}
            </div>
          </li>
          ${this.cipdReport.availableTargets &&
          this.cipdReport.availableTargets.length > 1
            ? html`
                <li>
                  <b>Platform</b>
                  <div class="step-detail">
                    <div class="vscode-select">
                      <select @change=${this._selectTarget}>
                        ${this.cipdReport.availableTargets.map(
                          (target: {
                            name: string;
                            displayName?: string;
                          }) => html`
                            <option
                              value=${target.name}
                              ?selected=${target.name ===
                              this.cipdReport.targetSelected}
                            >
                              ${target.displayName
                                ? `${target.displayName} (${decodeBazelName(
                                    target.name,
                                  )})`
                                : decodeBazelName(target.name)}
                            </option>
                          `,
                        )}
                      </select>
                    </div>
                  </div>
                </li>
              `
            : ''}
          <li>
            <b>Enjoy code intelligence</b>
            <div class="step-detail">${activeFileText}</div>
          </li>
        </ol>
      </div>`;
    }

    // Broken state
    if (!this.cipdReport.bazelPath || !this.cipdReport.clangdPath) {
      return html` <div class="code-intelligence-status-card">
        ${header}
        <div class="status-line status-error">
          <span>❌</span>
          <span
            >Code intelligence is not working (<a
              href="#"
              @click=${this._openDebugDetails}
              >see details</a
            >).</span
          >
        </div>
      </div>`;
    }

    // First run / In-progress state
    let currentStepIndex = 0;
    if (
      !this.cipdReport.availableTargets ||
      this.cipdReport.availableTargets.length === 0
    ) {
      currentStepIndex = 0;
    } else if (!this.cipdReport.targetSelected) {
      currentStepIndex = 1;
    } else {
      currentStepIndex = 2; // All steps before "Enjoy" are done.
    }

    let platformStepDetail;
    if (
      this.cipdReport.availableTargets &&
      this.cipdReport.availableTargets.length > 1
    ) {
      platformStepDetail = html`
        <div class="vscode-select">
          <select @change=${this._selectTarget}>
            ${!this.cipdReport.targetSelected
              ? html`<option value="" disabled selected>
                  Select a platform
                </option>`
              : ''}
            ${this.cipdReport.availableTargets.map(
              (target: { name: string; displayName?: string }) => html`
                <option
                  value=${target.name}
                  ?selected=${target.name === this.cipdReport.targetSelected}
                >
                  ${target.displayName
                    ? `${target.displayName} (${decodeBazelName(target.name)})`
                    : decodeBazelName(target.name)}
                </option>
              `,
            )}
          </select>
        </div>
      `;
    } else {
      platformStepDetail = this.cipdReport.targetSelected
        ? `Selected: ${this._selectedTargetDisplayName}`
        : currentStepIndex === 1
          ? 'Select a platform from the build'
          : 'Selected: No platforms detected';
    }

    const steps = [
      {
        title: 'Generate compile commands',
        detail: html`
          <div class="step-detail">
            ${isPreconfigured
              ? html`
                  Select a target to generate compile commands:
                  <div class="target-selection-row">
                    <div class="vscode-select">
                      <select @change=${this._handlePreconfiguredTargetChange}>
                        ${this.cipdReport.preconfiguredTargets?.map(
                          (target) => html`
                            <option
                              value=${target.label}
                              ?selected=${target.label ===
                              this.selectedPreconfiguredTarget}
                            >
                              ${target.displayName || target.label}
                            </option>
                          `,
                        )}
                      </select>
                    </div>
                    <div
                      class="vscode-button"
                      role="button"
                      tabindex="0"
                      @click=${this._runPreconfiguredTarget}
                      @keydown=${(e: KeyboardEvent) => {
                        if (e.key === 'Enter' || e.key === ' ') {
                          this._runPreconfiguredTarget();
                        }
                      }}
                    >
                      Generate
                    </div>
                  </div>
                `
              : html`
                  <p style="margin-top:0;">
                    No preconfigured targets found. Please configure a
                    <code>pw_compile_commands_generator</code> target in
                    <code>BUILD.bazel</code> to enable C++ code intelligence.
                  </p>
                  <p>
                    See the
                    <a href="https://pigweed.dev/pw_ide/guide/" target="_blank"
                      >Pigweed IDE Guide</a
                    >
                    for more details.
                  </p>
                `}
          </div>
        `,
      },
      ...(this.cipdReport.availableTargets &&
      this.cipdReport.availableTargets.length > 1
        ? [
            {
              title: `Platform`,
              detail: platformStepDetail,
            },
          ]
        : []),
      {
        title: 'Enjoy code intelligence',
        detail: 'Not enabled yet',
      },
    ];

    return html` <div class="code-intelligence-status-card">
      ${header}
      <div class="status-line status-info">
        <span>ℹ️</span>
        <span
          >Compile commands are <b>preconfigured</b> in
          <code>BUILD.bazel</code>
          (<a href="#" @click=${this._openDebugDetails}>see details</a>).</span
        >
      </div>
      <ol class="status-steps">
        ${steps.map((step, index) => {
          let detailContent;
          if (typeof step.detail === 'string') {
            const detailParts = step.detail.split(/<\/?code>/);
            detailContent =
              detailParts.length === 3
                ? html`${detailParts[0]}<code>${detailParts[1]}</code>${detailParts[2]}`
                : step.detail;
          } else {
            detailContent = step.detail;
          }

          return html`
            <li class=${index > currentStepIndex ? 'step-dimmed' : ''}>
              <b>${step.title}</b>
              <div class="step-detail">${detailContent}</div>
            </li>
          `;
        })}
      </ol>
    </div>`;
  }

  @state() isExtensionDisabled = false;

  render() {
    return html`
      ${
        this.isExtensionDisabled
          ? html`
              <div class="disabled-overlay">
                <p>
                  This doesn't look like a Pigweed or a Bazel project.<br />
                  Extension is disabled.
                </p>
                <div
                  class="vscode-button"
                  role="button"
                  @click="${() => {
                    vscode.postMessage({ type: 'fileBug' });
                  }}"
                  @keydown="${(e: KeyboardEvent) => {
                    if (e.key === 'Enter' || e.key === ' ') {
                      vscode.postMessage({ type: 'fileBug' });
                    }
                  }}"
                  tabindex="0"
                >
                  File Bug
                </div>
              </div>
            `
          : ''
      }
      <div class="${this.isExtensionDisabled ? 'blur-content' : ''}">
        ${this._renderCodeIntelligenceStatus()}
        <details id="code-intelligence-details" class="vscode-collapsible">
          <summary>
            <i class="codicon codicon-chevron-right icon-arrow"></i>
            <b class="title"> Code Intelligence </b>
          </summary>
          <div>
            <span>Settings for code navigation and intelligence.</span>
            <div class="container">
              <div class="row"></div>
              </div>

              ${
                this._isCodeIntelligenceHealthy
                  ? html`
                      <div class="row">
                        <div><b>Everything appears to be working</b><br /></div>
                        <div>✅</div>
                      </div>
                    `
                  : html` <div class="row">
                      <div>
                        <b>Still not working?</b><br />
                        <span>See below on what might be wrong.</span>
                      </div>
                    </div>`
              }

              <h3 style="margin-top: 20px; text-transform: uppercase; font-size: 11px; opacity: 0.8;">Debug Information</h3>
              <div id="debug-code-intelligence-details">
                ${
                  this.cipdReport.clangdPath
                    ? html`
                        <div class="row">
                          <div>Restart clangd language server</div>
                          <div>
                            <button
                              class="vscode-button"
                              @click="${() => {
                                vscode.postMessage({
                                  type: 'restartClangd',
                                });
                              }}"
                            >
                              Restart
                            </button>
                          </div>
                        </div>
                      `
                    : ''
                }
                <div class="row">
                  <div>
                    <b>Clangd is available</b><br />
                    <sub>${this.cipdReport.clangdPath || 'N/A'}</sub>
                  </div>
                  <div>
                    ${
                      this.cipdReport.clangdPath
                        ? '✅'
                        : html`
                            <button
                              class="vscode-button"
                              @click="${() => {
                                vscode.postMessage({
                                  type: 'retryClangdPath',
                                });
                              }}"
                            >
                              Repair
                            </button>
                          `
                    }
                  </div>
                </div>
                <div class="row">
                  <div>
                    <b>Bazel is available</b><br />
                    <sub>${this.cipdReport.bazelPath || 'N/A'}</sub>
                  </div>
                  <div>${this.cipdReport.bazelPath ? '✅' : '❌'}</div>
                </div>
                <div class="row">
                  <div>
                    <b>Target is selected</b><br />
                    <sub>${this.cipdReport.targetSelected || 'None'}</sub>
                  </div>
                  <div>${this.cipdReport.targetSelected ? '✅' : '❌'}</div>
                </div>
                <div class="row">
                  <div>
                    <b>compile_commands.json exists</b><br />
                    <sub>${this.cipdReport.compileCommandsPath || 'N/A'}</sub>
                  </div>
                  <div>
                    ${this.cipdReport.isCompileCommandsGenerated ? '✅' : '❌'}
                  </div>
                </div>
              </div>
            </div>
          </div>
        </details>
        <details class="vscode-collapsible">
          <summary>
            <i class="codicon codicon-chevron-right icon-arrow"></i>
            <b class="title"> Recommended Extensions</b>
          </summary>
          <div>
            <div class="container">
              ${
                this.extensionData.recommended.length === 0 &&
                html` <p><i>No recommended extensions found.</i></p> `
              }
              ${this.extensionData.recommended.map(
                (ext) =>
                  html`<div class="row">
                    <div>${ext.name || ext.id}</div>
                    <div>
                      ${!ext.installed
                        ? html`
                            <button
                              class="vscode-button"
                              @click="${() => {
                                vscode.postMessage({
                                  type: 'openExtension',
                                  data: ext.id,
                                });
                              }}"
                            >
                              Install
                            </button>
                          `
                        : html`<i>Installed</i>`}
                    </div>
                  </div>`,
              )}
            </div>
            <b>Unwanted Extensions</b><br />
            <div class="container">
              ${
                this.extensionData.unwanted.length === 0 &&
                html` <p><i>No unwanted extensions found.</i></p> `
              }
              ${this.extensionData.unwanted.map(
                (ext) =>
                  html`<div class="row">
                    <div>${ext.name || ext.id}</div>
                    <div>
                      ${ext.installed
                        ? html`
                            <button
                              class="vscode-button"
                              @click="${() => {
                                vscode.postMessage({
                                  type: 'openExtension',
                                  data: ext.id,
                                });
                              }}"
                            >
                              Remove
                            </button>
                          `
                        : html`<i>Not Installed</i>`}
                    </div>
                  </div>`,
              )}
            </div>
          </div>
        </details>
        <details class="vscode-collapsible">
          <summary>
            <i class="codicon codicon-chevron-right icon-arrow"></i>
            <b class="title"> Help and Feedback </b>
          </summary>
          <div class="container">
            <div
              class="row link-row"
              @click="${() => {
                vscode.postMessage({ type: 'dumpLogs' });
              }}"
              @keydown=${(e: KeyboardEvent) => {
                if (e.key === ' ' || e.key === 'Enter') {
                  vscode.postMessage({ type: 'dumpLogs' });
                }
              }}
              tabindex="0"
            >
              <span>
                <i class="codicon codicon-notebook"></i> Dump Extension Logs
              </span>
            </div>
            <div
              class="row link-row"
              @click="${() => {
                vscode.postMessage({ type: 'openDocs' });
              }}"
              @keydown=${(e: KeyboardEvent) => {
                if (e.key === ' ' || e.key === 'Enter') {
                  vscode.postMessage({ type: 'openDocs' });
                }
              }}
              tabindex="0"
            >
              <span
                ><i class="codicon codicon-book"></i> View Documentation</span
              >
            </div>
            <div
              class="row link-row"
              @click="${() => {
                vscode.postMessage({ type: 'fileBug' });
              }}"
              @keydown=${(e: KeyboardEvent) => {
                if (e.key === ' ' || e.key === 'Enter') {
                  vscode.postMessage({ type: 'fileBug' });
                }
              }}
              tabindex="0"
            >
              <span>
                <i class="codicon codicon-bug"></i> Report a Bug / Request a
                Feature
              </span>
            </div>
          </div>
        </details>
      </div>
    `;
  }

  async firstUpdated() {
    window.addEventListener(
      'message',
      (e: MessageEvent<{ type: string; data: any }>) => {
        const message = e.data;
        const { type } = message;
        if (type === 'extensionData') {
          this.extensionData = message.data;
        } else if (type === 'extensionDisabled') {
          this.isExtensionDisabled = message.data;
          this.requestUpdate();
        } else if (type === 'cipdReport') {
          this.cipdReport = message.data;

          if (
            !this.selectedPreconfiguredTarget &&
            this.cipdReport.preconfiguredTargets &&
            this.cipdReport.preconfiguredTargets.length > 0
          ) {
            this.selectedPreconfiguredTarget =
              this.cipdReport.preconfiguredTargets[0].label;
          }

          this.requestUpdate();
        }
      },
      false,
    );

    vscode.postMessage({ type: 'getExtensionData' });
    vscode.postMessage({ type: 'getCipdReport' });
  }
}
