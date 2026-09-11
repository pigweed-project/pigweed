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
 * Custom element for the Pigweed header bar.
 */
class PwHeader extends HTMLElement {
  connectedCallback() {
    this.rewriteUrls();
    this.setupPagefind();
    this.setupSearch();
  }

  /**
   * Sets up search functionality:
   * On small viewports, the mobile search button triggers the Pagefind search
   * modal.
   */
  setupSearch() {
    const mobileBtn = this.querySelector('#pw-search-mobile');
    const desktopTrigger = this.querySelector('#pw-search-desktop');

    if (mobileBtn && desktopTrigger) {
      mobileBtn.addEventListener('click', () => {
        const targetBtn =
          desktopTrigger.querySelector('button, .pf-trigger-btn') ||
          desktopTrigger;
        targetBtn.dispatchEvent(
          new MouseEvent('click', { bubbles: true, cancelable: true }),
        );
      });
    }
  }

  /**
   * Dynamically loads Pagefind assets using the relative site root path so that
   * staging sites, subsites, and local servers load search resources without
   * 404s.
   */
  setupPagefind() {
    const root = getSiteRootPath();
    const baseUrl = new URL(root, window.location.href).href;
    const bundleUrl = new URL(`${root}search/`, window.location.href).href;

    const config = document.querySelector('pagefind-config');
    if (config) {
      config.setAttribute('base-url', baseUrl);
      config.setAttribute('bundle-path', bundleUrl);
    }

    if (!document.querySelector('link[data-pagefind-css]')) {
      const link = document.createElement('link');
      link.rel = 'stylesheet';
      link.href = `${root}search/pagefind-component-ui.css`;
      link.setAttribute('data-pagefind-css', '');
      document.head.appendChild(link);
    }

    if (!document.querySelector('script[data-pagefind-js]')) {
      const script = document.createElement('script');
      script.type = 'module';
      script.src = `${root}search/pagefind-component-ui.js`;
      script.setAttribute('data-pagefind-js', '');
      document.head.appendChild(script);
    }

    // Dynamically inject document path below search result titles.
    const modal = document.querySelector('pagefind-modal');
    if (modal) {
      const processResults = (rootNode) => {
        if (!rootNode) return;
        const links = rootNode.querySelectorAll(
          'a[href]:not([data-path-added])',
        );
        links.forEach((link) => {
          link.setAttribute('data-path-added', 'true');
          const href = link.getAttribute('href');
          if (!href) return;
          const title = link.querySelector(
            '.pagefind-ui__result-title, .pagefind-ui__sub-result-title, ' +
              '[class*="title"]',
          );
          const docPath = formatDocPath(href);
          if (!docPath) return;

          const pathDiv = document.createElement('div');
          pathDiv.className = 'pagefind-ui__result-path';
          pathDiv.textContent = docPath;
          if (title) {
            title.insertAdjacentElement('afterend', pathDiv);
          } else {
            link.appendChild(pathDiv);
          }
        });
      };

      const observeTarget = (target) => {
        if (!target) return;
        processResults(target);
        const observer = new MutationObserver(() => processResults(target));
        observer.observe(target, { childList: true, subtree: true });
      };

      observeTarget(modal);
      if (modal.shadowRoot) {
        observeTarget(modal.shadowRoot);
      }
    }
  }

  /**
   * Rewrites absolute production URLs (https://pigweed.dev/...) to relative
   * staging/local paths based on the current page's data-content_root,
   * rustdoc metadata, or URL pathname depth.
   */
  rewriteUrls() {
    const root = getSiteRootPath();
    const links = document.querySelectorAll('a[href^="https://pigweed.dev/"]');
    const pattern = 'https://pigweed.dev/';
    links.forEach((link) => {
      const href = link.getAttribute('href');
      if (!href || !href.startsWith(pattern)) return;
      let target = href.replace(pattern, root);
      if (target === root || target.endsWith('/')) {
        target += 'index.html';
      }
      link.setAttribute('href', target);
      link.href = target;
    });
  }
}

/**
 * Resolves a search result href into a clean document relative path.
 */
function formatDocPath(href) {
  if (!href) return '';
  try {
    const rootPathname = new URL(getSiteRootPath(), window.location.href)
      .pathname;
    const targetUrl = new URL(href, window.location.href);
    let docPath = targetUrl.pathname;

    if (docPath.startsWith(rootPathname)) {
      docPath = docPath.slice(rootPathname.length);
    } else {
      docPath = docPath.replace(/^\//, '');
    }

    return docPath || 'index.html';
  } catch (e) {
    return href.split('#')[0].split('?')[0].replace(/^\//, '');
  }
}

/**
 * Resolves the relative root path (e.g. "./", "../", "../../") based on
 * data-content_root, rustdoc metadata, or URL pathname depth.
 */
function getSiteRootPath() {
  const html = document.documentElement;
  const metaRustdoc = document.querySelector('meta[name="rustdoc-vars"]');
  const metaDoxygen = document.querySelector('meta[name="doxygen-site-root"]');
  let root = null;

  if (html?.dataset?.content_root != null) {
    root = html.dataset.content_root;
  } else if (metaRustdoc?.dataset?.rootPath != null) {
    root = `../${metaRustdoc.dataset.rootPath}`;
  } else if (metaDoxygen?.getAttribute('content') != null) {
    root = metaDoxygen.getAttribute('content');
  } else if (
    document.querySelector('#side-nav, #nav-path') ||
    window.location.pathname.includes('/api/cc/')
  ) {
    // Doxygen C++ API reference pages reside in api/cc/ (2 levels below site
    // root)
    root = '../../';
  } else {
    const currentPath = window.location.pathname
      .replace(/^\//, '')
      .replace(/\/$/, '');
    if (!currentPath) {
      root = './';
    } else {
      const depth = currentPath.split('/').length - 1;
      root = depth > 0 ? '../'.repeat(depth) : './';
    }
  }

  if (!root.endsWith('/')) {
    root += '/';
  }
  return root;
}

customElements.define('pw-header', PwHeader);
