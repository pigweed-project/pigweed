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
    this.setupPagefind();
    this.setupSearch();
    this.setupNavPopovers();
    this.setupMobileMenu();
    // We wait for DOMContentLoaded (or run immediately if already loaded) to
    // rewrite URLs across the entire page. Because <pw-header> is connected at
    // the very top of <body>, running rewriteUrls() immediately would miss
    // elements parsed later in the document, such as Sphinx sidebar navigation
    // links within #pst-primary-sidebar (e.g. C/C++ API reference links).
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', () => {
        this.rewriteUrls();
        this.setupTocFab();
      });
    } else {
      this.rewriteUrls();
      this.setupTocFab();
    }
  }

  /**
   * Sets up the mobile hamburger menu button to toggle the active subsite's
   * navigation menu (Sphinx, Rustdoc, or Doxygen).
   */
  setupMobileMenu() {
    const menuBtn = this.querySelector('#pw-header-menu');
    if (!menuBtn) return;

    const icon = menuBtn.querySelector('.material-symbols-outlined');

    const updateMenuIcon = (isOpen) => {
      menuBtn.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
      if (icon) {
        icon.textContent = isOpen ? 'menu_open' : 'menu';
      }
    };

    const scrollToActiveItem = () => {
      requestAnimationFrame(() => {
        // 1. Rustdoc subsite
        const rustdocActive = document.querySelector(
          'nav.sidebar .current, nav.sidebar [aria-current="page"], nav.sidebar #rustdoc-toc .current',
        );
        const rustdocSidebar = document.querySelector('nav.sidebar');
        if (rustdocActive && rustdocSidebar) {
          const sRect = rustdocSidebar.getBoundingClientRect();
          const aRect = rustdocActive.getBoundingClientRect();
          if (sRect.height > 0) {
            rustdocSidebar.scrollTop = Math.max(
              0,
              aRect.top -
                sRect.top +
                rustdocSidebar.scrollTop -
                sRect.height / 2 +
                aRect.height / 2,
            );
          }
          return;
        }

        // 2. Doxygen subsite
        const doxygenActive = document.querySelector(
          '#side-nav .selected, #side-nav .item.selected, #side-nav a.selected',
        );
        const doxygenSideNav = document.querySelector('#side-nav');
        if (doxygenActive && doxygenSideNav) {
          const sRect = doxygenSideNav.getBoundingClientRect();
          const aRect = doxygenActive.getBoundingClientRect();
          if (sRect.height > 0) {
            doxygenSideNav.scrollTop = Math.max(
              0,
              aRect.top -
                sRect.top +
                doxygenSideNav.scrollTop -
                sRect.height / 2 +
                aRect.height / 2,
            );
          }
          return;
        }

        // 3. Sphinx subsite
        const sphinxSidebar = document.querySelector('.bd-sidebar-primary');
        let sphinxActive = document.querySelector(
          '.bd-sidebar-primary .pw-site-nav [aria-current="page"], .bd-sidebar-primary .pw-site-nav a.current, .bd-sidebar-primary .pw-site-nav a.active',
        );
        if (!sphinxActive || sphinxActive.getClientRects().length === 0) {
          sphinxActive = document.querySelector(
            '.bd-sidebar-primary [aria-current="page"], .bd-sidebar-primary a.current, .bd-sidebar-primary a.active, .bd-sidebar-primary .active, .bd-sidebar-primary .current',
          );
        }
        if (sphinxActive && sphinxSidebar) {
          const sRect = sphinxSidebar.getBoundingClientRect();
          const aRect = sphinxActive.getBoundingClientRect();
          if (sRect.height > 0 && aRect.height > 0) {
            sphinxSidebar.scrollTop = Math.max(
              0,
              aRect.top -
                sRect.top +
                sphinxSidebar.scrollTop -
                sRect.height / 2 +
                aRect.height / 2,
            );
          }
        }
      });
    };

    menuBtn.addEventListener('click', () => {
      // Close TOC drawer if open
      document.body.classList.remove('pw-sphinx-toc-open');
      const tocFab = document.querySelector('#pw-toc-fab');
      if (tocFab) tocFab.setAttribute('aria-expanded', 'false');

      // 1. Rustdoc subsite
      const rustdocSidebar = document.querySelector(
        'nav.sidebar, .rustdoc .sidebar',
      );
      if (rustdocSidebar) {
        const isShown = rustdocSidebar.classList.contains('shown');
        if (isShown) {
          rustdocSidebar.classList.remove('shown');
          updateMenuIcon(false);
        } else {
          rustdocSidebar.classList.add('shown');
          updateMenuIcon(true);
          scrollToActiveItem();
        }
        return;
      }

      // 2. Doxygen subsite
      const doxygenSideNav = document.querySelector('#side-nav');
      if (doxygenSideNav) {
        const isOpen = document.body.classList.toggle('pw-doxygen-nav-open');
        updateMenuIcon(isOpen);
        if (isOpen) {
          scrollToActiveItem();
        }
        return;
      }

      // 3. Sphinx subsite (PyData Sphinx Theme)
      const primarySidebar = document.querySelector(
        '#pst-primary-sidebar, .bd-sidebar-primary',
      );
      if (primarySidebar) {
        const isOpen = document.body.classList.toggle('pw-sphinx-nav-open');
        updateMenuIcon(isOpen);
        if (isOpen) {
          scrollToActiveItem();
        }
        return;
      }
    });

    const closeAllDrawers = () => {
      document.body.classList.remove('pw-sphinx-nav-open');
      document.body.classList.remove('pw-sphinx-toc-open');
      document.body.classList.remove('pw-doxygen-nav-open');
      const rustdocSidebar = document.querySelector(
        'nav.sidebar, .rustdoc .sidebar',
      );
      if (rustdocSidebar) {
        rustdocSidebar.classList.remove('shown');
      }
      updateMenuIcon(false);
      const tocFab = document.querySelector('#pw-toc-fab');
      if (tocFab) tocFab.setAttribute('aria-expanded', 'false');
    };

    // Close menu when clicking on the backdrop outside the drawer
    const backdrop = document.querySelector('#pw-nav-backdrop');
    if (backdrop) {
      backdrop.addEventListener('click', closeAllDrawers);
    }

    // Close mobile drawer when a link in Rustdoc sidebar is clicked
    document.addEventListener('click', (e) => {
      const link = e.target.closest('nav.sidebar a, .rustdoc .sidebar a');
      if (link) {
        closeAllDrawers();
      }
    });

    // Close on Escape key press
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        closeAllDrawers();
      }
    });

    // Reset menu state on viewport resize back to desktop
    const mediaQuery = window.matchMedia('(min-width: 840px)');
    mediaQuery.addEventListener('change', (e) => {
      if (e.matches) {
        closeAllDrawers();
      }
    });
  }

  /**
   * Sets up the floating action button (FAB) for page table of contents (TOC)
   * on Sphinx pages on mobile viewports.
   */
  setupTocFab() {
    const fab = document.querySelector('#pw-toc-fab');
    if (!fab) return;

    const updateTocState = () => {
      const secondarySidebar = document.querySelector(
        '#pst-secondary-sidebar, .bd-sidebar-secondary',
      );
      if (!secondarySidebar) {
        fab.classList.remove('has-toc');
        return;
      }

      const tocEntries = secondarySidebar.querySelectorAll(
        '.toc-entry, nav#pst-page-toc-nav li, nav.page-toc li, nav.page-toc a',
      );
      if (tocEntries.length > 0) {
        fab.classList.add('has-toc');
      } else {
        fab.classList.remove('has-toc');
      }
    };

    updateTocState();
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', updateTocState);
    }
    window.addEventListener('load', updateTocState);

    fab.addEventListener('click', (e) => {
      e.stopPropagation();
      // Close primary navigation if open
      document.body.classList.remove('pw-sphinx-nav-open');
      const menuBtn = document.querySelector('#pw-header-menu');
      if (menuBtn) {
        menuBtn.setAttribute('aria-expanded', 'false');
        const icon = menuBtn.querySelector('.material-symbols-outlined');
        if (icon) icon.textContent = 'menu';
      }

      const isOpen = document.body.classList.toggle('pw-sphinx-toc-open');
      fab.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
    });

    // Close TOC drawer when a TOC link is clicked
    document.addEventListener('click', (e) => {
      const link = e.target.closest(
        '#pst-secondary-sidebar a, .bd-sidebar-secondary a',
      );
      if (link) {
        document.body.classList.remove('pw-sphinx-toc-open');
        fab.setAttribute('aria-expanded', 'false');
      }
    });
  }

  /**
   * Progressive accessibility enhancement: sets aria-expanded on dropdown items.
   */
  setupNavPopovers() {
    const navItems = this.querySelectorAll('.pw-nav-item');
    navItems.forEach((item) => {
      const link = item.querySelector('.pw-nav-link.has-children');
      if (!link) return;

      link.setAttribute('aria-expanded', 'false');
      link.setAttribute('aria-haspopup', 'true');

      item.addEventListener('mouseenter', () => {
        link.setAttribute('aria-expanded', 'true');
      });
      item.addEventListener('mouseleave', () => {
        link.setAttribute('aria-expanded', 'false');
      });
      item.addEventListener('focusin', () => {
        link.setAttribute('aria-expanded', 'true');
      });
      item.addEventListener('focusout', (e) => {
        if (!item.contains(e.relatedTarget)) {
          link.setAttribute('aria-expanded', 'false');
        }
      });
    });
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
    const links = document.querySelectorAll('a[href^="https://pigweed.dev"]');
    const pattern = /^https:\/\/pigweed\.dev(\/)?/;
    links.forEach((link) => {
      const href = link.getAttribute('href');
      if (!href || !href.startsWith('https://pigweed.dev')) return;
      let target = href.replace(pattern, root);
      if (target === root || target.endsWith('/') || target === '') {
        target = `${root}index.html`;
      } else if (target.startsWith('#') || target.startsWith('?')) {
        target = `${root}index.html${target}`;
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

/**
 * Custom element for managing theme preferences (light/dark mode) within the
 * header. Root-level theme variables that transcend the header live in
 * theme.css.
 */
class PwTheme extends HTMLElement {
  connectedCallback() {
    this.buttons = this.querySelectorAll('.pw-theme-btn');
    if (!this.buttons.length) return;

    // Hide the 'ayu' theme option in Rustdoc settings.
    const style = document.createElement('style');
    style.textContent = 'label[for="theme-ayu"] { display: none !important; }';
    document.head.appendChild(style);

    const savedTheme =
      localStorage.getItem('theme') ||
      localStorage.getItem('mode') ||
      document.documentElement.getAttribute('data-theme') ||
      document.documentElement.getAttribute('data-mode') ||
      (document.documentElement.classList.contains('dark-mode')
        ? 'dark'
        : '') ||
      (document.documentElement.classList.contains('light-mode')
        ? 'light'
        : '') ||
      'dark';

    this.setTheme(savedTheme);

    this.buttons.forEach((btn) => {
      btn.addEventListener('click', () => {
        const theme = btn.getAttribute('data-theme-val');
        if (theme) {
          this.setTheme(theme);
        }
      });
    });

    // Re-assert the active Pigweed theme on Back/Forward Cache (bfcache) restoration.
    // Rustdoc's storage script listens to 'pageshow' and runs updateTheme() asynchronously
    // via setTimeout(..., 0), which can otherwise revert or switch data-theme unexpectedly.
    window.addEventListener('pageshow', (event) => {
      if (event.persisted) {
        const currentTheme =
          localStorage.getItem('theme') ||
          (window.matchMedia('(prefers-color-scheme: dark)').matches
            ? 'dark'
            : 'light');
        this.setTheme(currentTheme);
        setTimeout(() => this.setTheme(currentTheme), 0);
      }
    });
  }

  setTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    document.documentElement.setAttribute('data-mode', theme);
    document.documentElement.classList.remove('light-mode', 'dark-mode');
    document.documentElement.classList.add(`${theme}-mode`);

    this.buttons.forEach((btn) => {
      const isSelected = btn.getAttribute('data-theme-val') === theme;
      if (isSelected) {
        btn.classList.add('active');
        btn.setAttribute('aria-checked', 'true');
      } else {
        btn.classList.remove('active');
        btn.setAttribute('aria-checked', 'false');
      }
    });

    try {
      localStorage.setItem('theme', theme);
      localStorage.setItem('mode', theme);
      // Synchronize Rustdoc's theme keys to prevent Rustdoc's storage script
      // from falling back to or activating default themes on page load or
      // Back/Forward Cache (bfcache) restorations.
      localStorage.setItem('rustdoc-theme', theme);
      localStorage.setItem('rustdoc-use-system-theme', 'false');
      localStorage.setItem('rustdoc-preferred-dark-theme', 'dark');
      localStorage.setItem('rustdoc-preferred-light-theme', 'light');
    } catch (e) {
      // localStorage might be disabled or unavailable in some contexts
    }
  }
}

customElements.define('pw-theme', PwTheme);

/**
 * Custom element for Pigweed breadcrumbs.
 */
class PwBreadcrumbs extends HTMLElement {}

customElements.define('pw-breadcrumbs', PwBreadcrumbs);
