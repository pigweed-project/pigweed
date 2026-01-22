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
  isBazelInterceptorEnabled?: boolean;
  bazelCompileCommandsManualBuildCommand?: string;
  bazelCompileCommandsLastBuildCommand?: string;

  lastBuildPlatformCount?: number;
  activeFileCount?: number;
  availableTargets?: { name: string; path: string }[];
  preconfiguredTargets?: string[];
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
  @state() manualBazelTarget = '';

  @state() selectedPreconfiguredTarget = '';

  createRenderRoot() {
    return this;
  }

  connectedCallback() {
    super.connectedCallback();
    // Initialize manualBazelTarget when component connects
    this.manualBazelTarget =
      this.cipdReport.bazelCompileCommandsManualBuildCommand || '';

    // Initialize selectedPreconfiguredTarget if available
    if (
      this.cipdReport.preconfiguredTargets &&
      this.cipdReport.preconfiguredTargets.length > 0
    ) {
      this.selectedPreconfiguredTarget =
        this.cipdReport.preconfiguredTargets[0];
    }
  }

  private _toggleBazelInterceptor(enabled: boolean) {
    vscode.postMessage({
      type: enabled
        ? 'enableBazelBuildInterceptor'
        : 'disableBazelBuildInterceptor', // Send appropriate message
    });
  }

  // Handles input changes for the manual bazel command
  private _handleManualBazelInputChange(event: Event) {
    const inputElement = event.target as HTMLInputElement;
    this.manualBazelTarget = inputElement.value;
  }

  // Called when the Refresh button is clicked
  private _refreshCompileCommandsManually(e?: MouseEvent) {
    e?.stopPropagation();
    vscode.postMessage({
      type: 'refreshCompileCommandsManually',
      data: this.manualBazelTarget.trim(),
    });
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

    const platformCount = this.cipdReport.lastBuildPlatformCount || 0;
    const platformText = `(last build had ${platformCount} platform${
      platformCount === 1 ? '' : 's'
    })`;

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
            <b
              >${isPreconfigured
                ? 'Generate compile commands'
                : 'Run a build'}</b
            >
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
                                value=${target}
                                ?selected=${target ===
                                this.selectedPreconfiguredTarget}
                              >
                                ${target}
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
                : html`Last built:
                    <code
                      >bazel
                      ${this.cipdReport
                        .bazelCompileCommandsLastBuildCommand}</code
                    >`}
            </div>
          </li>
          <li>
            <b>Select a platform ${platformText}</b>
            <div class="step-detail">
              ${this.cipdReport.availableTargets &&
              this.cipdReport.availableTargets.length > 0
                ? html`
                    <div class="vscode-select">
                      <select @change=${this._selectTarget}>
                        ${this.cipdReport.availableTargets.map(
                          (target: { name: string }) => html`
                            <option
                              value=${target.name}
                              ?selected=${target.name ===
                              this.cipdReport.targetSelected}
                            >
                              ${target.name}
                            </option>
                          `,
                        )}
                      </select>
                    </div>
                  `
                : `Selected: ${this.cipdReport.targetSelected}`}
            </div>
          </li>
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
    if (!this.cipdReport.bazelCompileCommandsLastBuildCommand) {
      currentStepIndex = 0;
    } else if (!this.cipdReport.targetSelected) {
      currentStepIndex = 1;
    } else {
      currentStepIndex = 2; // All steps before "Enjoy" are done.
    }

    let platformStepDetail;
    if (
      this.cipdReport.availableTargets &&
      this.cipdReport.availableTargets.length > 0
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
              (target: { name: string }) => html`
                <option
                  value=${target.name}
                  ?selected=${target.name === this.cipdReport.targetSelected}
                >
                  ${target.name}
                </option>
              `,
            )}
          </select>
        </div>
      `;
    } else {
      platformStepDetail = this.cipdReport.targetSelected
        ? `Selected: ${this.cipdReport.targetSelected}`
        : currentStepIndex === 1
          ? 'Select a platform from the build'
          : 'Selected: No platforms detected';
    }

    const steps = [
      {
        title: isPreconfigured ? 'Generate compile commands' : 'Run a build',
        detail: isPreconfigured
          ? html`
              <div class="step-detail">
                Select a target to generate compile commands:
                <div class="target-selection-row">
                  <div class="vscode-select">
                    <select @change=${this._handlePreconfiguredTargetChange}>
                      ${this.cipdReport.preconfiguredTargets?.map(
                        (target) => html`
                          <option
                            value=${target}
                            ?selected=${target ===
                            this.selectedPreconfiguredTarget}
                          >
                            ${target}
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
              </div>
            `
          : this.cipdReport.bazelCompileCommandsLastBuildCommand
            ? `Last built: <code>bazel ${this.cipdReport.bazelCompileCommandsLastBuildCommand}</code>`
            : 'Build a bazel target in your project',
      },
      {
        title: `Select a platform ${platformText}`,
        detail: platformStepDetail,
      },
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
          >${isPreconfigured
            ? html`Compile commands are <b>preconfigured</b> in
                <code>BUILD.bazel</code>`
            : this.cipdReport.isBazelInterceptorEnabled
              ? html`Run <code>bazel </code> on a target to configure code
                  intelligence`
              : 'Refresh manually below to configure code intelligence'}
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
    const currentManualTarget =
      this.manualBazelTarget ??
      (this.cipdReport.bazelCompileCommandsManualBuildCommand || '');

    return html`
      ${this.isExtensionDisabled
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
        : ''}
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
              <div class="row">
                <div>
                  <b>Compile commands generated using</b><br />
                  <sub>
                    ${this.cipdReport.bazelCompileCommandsLastBuildCommand
                      ? `bazel ${this.cipdReport.bazelCompileCommandsLastBuildCommand}`
                      : 'NA'}
                  </sub>
                </div>
                <div></div>
              </div>
              <div class="toggle-button-group">
                <button
                  class="toggle-button ${this.cipdReport
                    .isBazelInterceptorEnabled
                    ? 'active'
                    : ''}"
                  @click="${() => {
                    this._toggleBazelInterceptor(true); // Enable
                  }}"
                >
                  <b>Last <code>bazel build</code> command</b>
                  <br />
                  <span
                    >Intercepts Bazel commands to update code intelligence
                    automatically. Only files built by the command will have
                    active intelligence.</span
                  >
                </button>
                <div
                  role="button"
                  tabindex="0"
                  @keydown=${(e: KeyboardEvent) => {
                    if (e.key === 'Enter') this._toggleBazelInterceptor(true);
                  }}
                  class="toggle-button ${!this.cipdReport
                    .isBazelInterceptorEnabled
                    ? 'active'
                    : ''}"
                  @click="${() => {
                    this._toggleBazelInterceptor(false); // Disable
                  }}"
                >
                  <b>Manual with a fixed command</b>
                  <br />
                  <span
                    >In this mode, code intelligence is updated only for the
                    below command, and must be done manually.</span
                  >
                  <br />
                  <div class="input-button-row">
                    <span class="prefix">bazel build </span>
                    <input
                      type="text"
                      class="vscode-input"
                      @click=${(e: MouseEvent) => e.stopPropagation()}
                      .value=${currentManualTarget}
                      @input=${this._handleManualBazelInputChange}
                      placeholder="//..."
                      aria-label="Manual bazel build target"
                      ?disabled=${this.cipdReport.isBazelInterceptorEnabled}
                    />
                    <div
                      class="vscode-button input-button"
                      role="button"
                      href="#"
                      @click=${this._refreshCompileCommandsManually}
                      ?disabled=${this.cipdReport.isBazelInterceptorEnabled}
                      @keydown=${(e: KeyboardEvent) => {
                        if (e.key === ' ' || e.key === 'Enter')
                          this._refreshCompileCommandsManually();
                      }}
                    >
                      Refresh
                    </div>
                  </div>
                </div>
              </div>
              <div class="row">
                <div></div>
              </div>

              ${this._isCodeIntelligenceHealthy
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
                  </div>`}

              <details
                id="debug-code-intelligence-details"
                class="vscode-collapsible"
                ?open=${!this._isCodeIntelligenceHealthy}
              >
                <summary>
                  <i class="codicon codicon-chevron-right icon-arrow"></i>
                  <b class="title"> Debug Code Intelligence </b>
                </summary>
                <div>
                  ${this.cipdReport.clangdPath
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
                    : ''}
                  <div class="row">
                    <div>
                      <b>Clangd is available</b><br />
                      <sub>${this.cipdReport.clangdPath || 'N/A'}</sub>
                    </div>
                    <div>
                      ${this.cipdReport.clangdPath
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
                          `}
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
                      ${this.cipdReport.isCompileCommandsGenerated
                        ? '✅'
                        : '❌'}
                    </div>
                  </div>
                </div>
              </details>
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
              ${this.extensionData.recommended.length === 0 &&
              html` <p><i>No recommended extensions found.</i></p> `}
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
              ${this.extensionData.unwanted.length === 0 &&
              html` <p><i>No unwanted extensions found.</i></p> `}
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
          this.manualBazelTarget =
            this.cipdReport.bazelCompileCommandsManualBuildCommand || '';

          if (
            !this.selectedPreconfiguredTarget &&
            this.cipdReport.preconfiguredTargets &&
            this.cipdReport.preconfiguredTargets.length > 0
          ) {
            this.selectedPreconfiguredTarget =
              this.cipdReport.preconfiguredTargets[0];
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
