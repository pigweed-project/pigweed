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

import { LitElement, html, PropertyValues } from 'lit';
import { customElement, query, state } from 'lit/decorators.js';
import { EvalOutput, ReplKernel } from '../../repl-kernel';
import './code-editor';
import { CodeEditor } from './code-editor';
import { styles } from './repl.styles';
import { themeDark } from '../../themes/dark';
import { themeLight } from '../../themes/light';

@customElement('repl-component')
export class Repl extends LitElement {
  static styles = [themeDark, themeLight, styles];
  private codeEditor: CodeEditor;
  private evalResults: EvalOutput[] = [];
  private readonly LOCALSTORAGE_KEY = 'replHistory';

  // prettier-ignore
  private welcomeMsg = html`Welcome to the Pigweed Web Console!

  Input keyboard shortcuts:
    Press <kbd>Enter</kbd> to run
    Press <kbd>Shift</kbd> + <kbd>Enter</kbd> to create multi-line commands
    Press <kbd>Up</kbd> or <kbd>Down</kbd> to navigate command history

  Example Python commands:
    device.rpcs.pw.rpc.EchoService.Echo(msg='hello!')
    LOG.warning('Message appears in Host Logs window.')
    DEVICE_LOG.warning('Message appears in Device Logs window.')`;

  @query('#output') _output!: HTMLElement;

  @state()
  replTitle: string;

  constructor(
    private replKernel: ReplKernel,
    title = 'REPL',
  ) {
    super();
    this.replTitle = title;

    this.evalResults.push({ result: this.welcomeMsg });
    this.codeEditor = new CodeEditor(
      '',
      this.replKernel.autocomplete.bind(this.replKernel),
      async (code: string) => {
        const output = await this.replKernel.eval(code);
        this.evalResults.push({ stdin: code, ...output });
        this.requestUpdate();
      },
    );
  }

  firstUpdated() {
    // Append the code-editor instance to the shadow DOM
    this.shadowRoot!.querySelector('#editor')!.appendChild(this.codeEditor);
  }

  protected updated(_changedProperties: PropertyValues): void {
    if (this._output.scrollHeight > this._output.clientHeight) {
      this._output.scrollTop = this._output.scrollHeight;
    }
  }

  /** Remove all results from output */
  private clearOutput() {
    this.evalResults = [];
    this.requestUpdate();
  }

  render() {
    return html`
      <div id="repl">
        <div class="header">
          ${this.replTitle}
          <span class="actions-container">
            <md-icon-button
              @click=${this.clearOutput}
              title="Clear Repl output"
            >
              <md-icon>&#xe16c;</md-icon>
            </md-icon-button>
            <md-icon-button
              href="https://pigweed.dev/pw_web/repl.html#python-shell"
              title="Go to the python shell documentation page"
            >
              <md-icon>&#xe8fd;</md-icon>
            </md-icon-button>
          </span>
        </div>
        <ul id="output">
          ${this.evalResults.map(
            (result) => html`
              <li>
                ${
                  !result.stderr &&
                  !result.stdout &&
                  !result.result &&
                  !result.stdin
                    ? html`<span class="stdout"><i>No output</i></span>`
                    : ''
                }
                <span class="stdin">${result.stdin}</span>
                <span class="stderr">${result.stderr}</span>
                <span class="stdout">${result.stdout}</span>
                <span class="stdout">${result.result}</span>
              </li>
            `,
          )}
        </ul>
        <div id="editor"></div>
      </div>
    `;
  }
}
