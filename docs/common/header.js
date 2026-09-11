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
    this.setupPagefind?.();
    this.setupSearch?.();
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
