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
 * @fileoverview Functions and client-side utilities required to make the
 * pigweed.dev documentation site function correctly during local development
 * preview (e.g. localhost, local static files) and staging site preview
 * (e.g. Cloud Storage try-buckets).
 */

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
window.getSiteRootPath = getSiteRootPath;

/**
 * Returns true if the current page is hosted on the production domain.
 */
function isProductionDomain(hostname = window.location.hostname) {
  return hostname === 'pigweed.dev' || hostname.endsWith('.pigweed.dev');
}
window.isProductionDomain = isProductionDomain;

/**
 * Rewrites absolute production URLs (https://pigweed.dev/...) to relative
 * staging/local paths based on the current page's root path.
 */
function rewriteUrls() {
  // Do not rewrite URLs on the production domain.
  if (window.isProductionDomain()) {
    return;
  }

  const root = getSiteRootPath();
  const links = document.querySelectorAll('a[href^="https://pigweed.dev"]');
  links.forEach((link) => {
    const href = link.getAttribute('href');
    if (!href || !href.startsWith('https://pigweed.dev')) return;

    try {
      const url = new URL(href);
      let relPath = url.pathname.replace(/^\//, '');
      if (!relPath || relPath.endsWith('/')) {
        relPath += 'index.html';
      }
      const target = `${root}${relPath}${url.search}${url.hash}`;
      link.setAttribute('href', target);
      link.href = target;
    } catch (e) {
      const pattern = /^https:\/\/pigweed\.dev(\/)?/;
      let target = href.replace(pattern, root);
      if (target === root || target === '') {
        target = `${root}index.html`;
      } else if (target.endsWith('/')) {
        target = `${target}index.html`;
      } else if (target.startsWith('#') || target.startsWith('?')) {
        target = `${root}index.html${target}`;
      }
      link.setAttribute('href', target);
      link.href = target;
    }
  });
}

// We fire rewriteUrls twice:
// 1. Fire immediately to rewrite elements already parsed and present in the DOM
//    (such as the site header and breadcrumbs bar). This eliminates the race condition
//    where a user clicks a top-level header or breadcrumb link before a large page
//    has finished downloading and parsing, which would otherwise navigate away to
//    the production site.
// 2. Fire again on DOMContentLoaded (or if document is already complete) to catch
//    all elements parsed later in the document, such as section navigation in the
//    Sphinx sidebar, links within article bodies, and docstrings.
rewriteUrls();
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', rewriteUrls);
}
