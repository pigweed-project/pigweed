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

/**
 * Custom element for the Pigweed Sphinx site navigation sidebar (<pw-nav>).
 */
class PwNav extends HTMLElement {
  connectedCallback() {
    this.addEventListener('click', (e) => {
      if (e.target.closest('a, summary')) {
        return;
      }
      const item = e.target.closest('.pw-nav-tree-item.has-children');
      if (!item) {
        return;
      }
      const sublist = item.querySelector(':scope > details > .pw-nav-sublist');
      if (sublist && sublist.contains(e.target)) {
        return;
      }
      const details = item.querySelector(':scope > details');
      if (details) {
        details.open = !details.open;
      }
    });

    if (document.readyState === 'loading') {
      document.addEventListener(
        'DOMContentLoaded',
        () => {
          this.scrollToActiveItem();
        },
        { once: true },
      );
    } else {
      this.scrollToActiveItem();
    }
  }

  /**
   * Scrolls the navigation container vertically so the active page link is
   * centered within the visible sidebar viewport.
   */
  scrollToActiveItem() {
    requestAnimationFrame(() => {
      const activeLink = this.querySelector(
        '[aria-current="page"], a.current, a.active',
      );
      if (!activeLink) return;

      const navRect = this.getBoundingClientRect();
      const activeRect = activeLink.getBoundingClientRect();
      if (navRect.height > 0 && activeRect.height > 0) {
        this.scrollTop = Math.max(
          0,
          activeRect.top -
            navRect.top +
            this.scrollTop -
            navRect.height / 2 +
            activeRect.height / 2,
        );
      }
    });
  }
}

customElements.define('pw-nav', PwNav);
