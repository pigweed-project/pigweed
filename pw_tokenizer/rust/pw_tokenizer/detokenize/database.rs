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

use std::collections::HashMap;

/// An entry in the token database.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TokenizedStringEntry {
    /// The format string of this entry.
    pub format_string: String,
    /// The date when this entry was removed, or empty if it is still active.
    pub date_removed: String,
}

/// A token database containing a mapping of tokens to entries.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Database(HashMap<String, HashMap<u32, Vec<TokenizedStringEntry>>>);

impl Database {
    /// Constructs an empty token database.
    #[must_use]
    pub fn new() -> Self {
        Self(HashMap::new())
    }

    /// Adds a tokenized string entry to the database.
    pub fn add_entry(
        &mut self,
        domain: &str,
        token: u32,
        format_string: String,
        date_removed: String,
    ) {
        let entries = self
            .0
            .entry(domain.to_string())
            .or_default()
            .entry(token)
            .or_default();

        for entry in entries.iter_mut() {
            if entry.format_string == format_string {
                if date_removed > entry.date_removed {
                    entry.date_removed = date_removed;
                }
                return;
            }
        }
        entries.push(TokenizedStringEntry {
            format_string,
            date_removed,
        });
    }

    /// Looks up database entries for a given token and domain.
    #[must_use]
    pub fn lookup(&self, token: u32, domain: &str) -> &[TokenizedStringEntry] {
        let canonical_domain = crate::detokenize::detokenizer::canonicalize_domain(domain);
        self.0
            .get(&canonical_domain)
            .and_then(|domain_map| domain_map.get(&token))
            .map(|entries| entries.as_slice())
            .unwrap_or(&[])
    }
}
