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

/**
 * Hooks Doxygen's tree builders to sync disclosure state, shorten C++ labels,
 * preserve keyboard focus across lazy node insertion, and apply --pw-nav-width.
 */
function setupDoxygenNav() {
  // Matches barWidth = 6 in Doxygen's generated resize.js (#splitbar gutter).
  const SPLITBAR_WIDTH_PX = 6;
  const FALLBACK_SIDEBAR_WIDTH_PX = 272;

  let pendingKeyboardFocusToggle = null;

  const getDefaultSidebarWidthPx = () => {
    const rootStyle = window.getComputedStyle(document.documentElement);
    const raw = rootStyle.getPropertyValue('--pw-nav-width').trim();
    const value = Number.parseFloat(raw);
    if (!Number.isFinite(value) || value <= 0) {
      return FALLBACK_SIDEBAR_WIDTH_PX;
    }
    if (raw.endsWith('rem')) {
      const rootFontPx = Number.parseFloat(rootStyle.fontSize) || 16;
      return Math.round(value * rootFontPx);
    }
    return Math.round(value);
  };

  const stripNamespace = (text) =>
    text.startsWith('operator ')
      ? text
      : text.replace(/^(?:[a-z][a-z0-9_]*::)+/, '');

  const formatListLabels = (ul) => {
    if (!ul) return;
    const entries = [];
    const shortOriginals = new Map();
    for (const li of ul.children) {
      const link = li.querySelector(':scope > .item > .label > a');
      if (!link) continue;
      const full = link.dataset.fullLabel || link.textContent.trim();
      link.dataset.fullLabel = full;
      const short = stripNamespace(full);
      entries.push({ link, full, short });
      let set = shortOriginals.get(short);
      if (!set) {
        set = new Set();
        shortOriginals.set(short, set);
      }
      set.add(full);
    }
    for (const { link, full, short } of entries) {
      const display = shortOriginals.get(short).size > 1 ? full : short;
      if (link.dataset.formattedLabel === display) continue;
      link.dataset.formattedLabel = display;
      link.title = full;
      link.textContent = display;
    }
  };

  const syncNodeExpanded = (node) => {
    if (!node || !node.expandToggle || !node.plus_img) {
      return;
    }
    const isOpen = Boolean(node.expanded);
    node.plus_img.classList.toggle('pw-open', isOpen);
    node.expandToggle.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
  };

  const prepareChildNodes = (po) => {
    const ul =
      typeof po.getChildrenUL === 'function' ? po.getChildrenUL() : null;
    formatListLabels(ul);
    if (!Array.isArray(po.children)) {
      return;
    }
    for (const child of po.children) {
      if (!child || !child.expandToggle || !child.plus_img) {
        continue;
      }
      const toggleLink = child.expandToggle;
      const labelEl = child.labelSpan;
      // Place toggle after label in DOM so Tab order matches visual order.
      if (
        child.itemDiv &&
        labelEl &&
        toggleLink.previousElementSibling !== labelEl
      ) {
        child.itemDiv.appendChild(toggleLink);
      }
      const labelLink = labelEl && labelEl.querySelector(':scope > a');
      const labelText = labelLink
        ? labelLink.dataset.fullLabel || labelLink.textContent.trim()
        : '';
      toggleLink.setAttribute('role', 'button');
      toggleLink.setAttribute(
        'aria-label',
        labelText ? `${labelText} subpages` : 'Toggle section',
      );
      syncNodeExpanded(child);

      if (!toggleLink.dataset.pwBound) {
        toggleLink.dataset.pwBound = 'true';
        const origOnClick = toggleLink.onclick;
        toggleLink.onclick = function (e) {
          if (document.activeElement === toggleLink) {
            pendingKeyboardFocusToggle = toggleLink;
          }
          if (typeof origOnClick === 'function') {
            origOnClick.call(this, e);
          }
          syncNodeExpanded(child);
        };
        if (labelLink && labelLink.classList.contains('nolink')) {
          labelLink.onclick = toggleLink.onclick;
        }
        toggleLink.addEventListener('keydown', (e) => {
          if (e.key === ' ') {
            e.preventDefault();
            pendingKeyboardFocusToggle = toggleLink;
            toggleLink.click();
          }
        });
      }
    }
  };

  const hasSavedSidebarWidth = () => {
    try {
      if (window.localStorage.getItem('doxygen_width')) {
        return true;
      }
    } catch {
      // Ignore storage access errors.
    }
    return /(?:^|;\s*)doxygen_width=/.test(document.cookie || '');
  };

  if (typeof window.getNode === 'function') {
    const origGetNode = window.getNode;
    window.getNode = function (o, po) {
      // Doxygen's getNode() detaches and re-inserts po.li, dropping focus.
      const focusedInRow =
        po && po.li && po.li.contains(document.activeElement)
          ? document.activeElement
          : null;
      const asyncKeyboardFocus =
        !focusedInRow &&
        po &&
        po.expandToggle &&
        pendingKeyboardFocusToggle === po.expandToggle &&
        (document.activeElement === po.expandToggle ||
          document.activeElement === document.body)
          ? po.expandToggle
          : null;
      const focusTarget = focusedInRow || asyncKeyboardFocus;

      origGetNode.call(this, o, po);
      prepareChildNodes(po);

      if (focusTarget && document.contains(focusTarget)) {
        focusTarget.focus({ preventScroll: true });
        if (pendingKeyboardFocusToggle === focusTarget) {
          pendingKeyboardFocusToggle = null;
        }
      }
    };
  }

  if (typeof window.expandNode === 'function') {
    const origExpandNode = window.expandNode;
    window.expandNode = function (o, node, imm, showRoot) {
      origExpandNode.call(this, o, node, imm, showRoot);
      syncNodeExpanded(node);
    };
  }

  if (typeof window.showNode === 'function') {
    const origShowNode = window.showNode;
    window.showNode = function (o, node, index, hash) {
      origShowNode.call(this, o, node, index, hash);
      syncNodeExpanded(node);
    };
  }

  if (typeof window.initResizable === 'function') {
    const origInitResizable = window.initResizable;
    window.initResizable = function (...args) {
      origInitResizable.apply(this, args);
      if (!hasSavedSidebarWidth()) {
        const sideNav = document.getElementById('side-nav');
        const docContent = document.getElementById('doc-content');
        if (sideNav && docContent) {
          const widthPx = getDefaultSidebarWidthPx();
          sideNav.style.width = `${widthPx}px`;
          docContent.style.marginLeft = `${widthPx + SPLITBAR_WIDTH_PX}px`;
        }
      }
    };
  }
}

setupDoxygenNav();
