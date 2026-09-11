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

window.pw = {};

// IMPORTANT: This script should ONLY contain JavaScript logic that ONLY
// applies to the Sphinx subsite. Scripts that apply globally across Sphinx,
// Rustdoc, and Doxygen should go into //docs/common/header.js
// instead.

window.addEventListener('DOMContentLoaded', () => {
  // Manually control when Mermaid diagrams render to prevent scrolling issues.
  // Context: https://pigweed.dev/docs/style_guide.html#site-nav-scrolling
  if (window.mermaid) {
    // https://mermaid.js.org/config/usage.html#using-mermaid-run
    window.mermaid.run();
  }
  // Relative path to the root directory of the site. Sphinx's
  // HTML builder auto-inserts this metadata on every page.
  window.pw.root = document.documentElement.dataset.content_root;
});
