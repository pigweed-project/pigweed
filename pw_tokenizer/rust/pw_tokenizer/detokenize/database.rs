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

//! In-memory token database storage and lookup.

use std::collections::HashMap;

use pw_format::FormatString;

/// An entry in the token database.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TokenizedStringEntry {
    /// The format string of this entry.
    pub format_string: String,
    /// The date when this entry was removed, or empty if it is still active.
    pub date_removed: String,
    /// Optional pre-parsed format string.
    pub parsed_format_string: Option<FormatString>,
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

    /// Adds a tokenized string entry to the database without pre-parsing the format string.
    pub fn add_entry(
        &mut self,
        domain: &str,
        token: u32,
        format_string: String,
        date_removed: String,
    ) {
        self.add_entry_with_prefix(domain, token, format_string, date_removed, None);
    }

    /// Adds a tokenized string entry to the database and pre-parses the format string with `prefix`.
    pub fn add_entry_preparsed(
        &mut self,
        domain: &str,
        token: u32,
        format_string: String,
        date_removed: String,
        prefix: char,
    ) {
        self.add_entry_with_prefix(domain, token, format_string, date_removed, Some(prefix));
    }

    pub(crate) fn add_entry_with_prefix(
        &mut self,
        domain: &str,
        token: u32,
        format_string: String,
        date_removed: String,
        prefix: Option<char>,
    ) {
        let canonical_domain = crate::detokenize::canonicalize_domain(domain);
        let entries = self
            .0
            .entry(canonical_domain)
            .or_default()
            .entry(token)
            .or_default();

        if let Some(existing_entry) = entries
            .iter_mut()
            .find(|entry| entry.format_string == format_string)
        {
            if date_removed > existing_entry.date_removed {
                existing_entry.date_removed = date_removed;
            }
            if let Some(prefix_char) =
                prefix.filter(|_| existing_entry.parsed_format_string.is_none())
            {
                existing_entry.parsed_format_string =
                    Some(FormatString::parse_printf_lossy_with_prefix(
                        &existing_entry.format_string,
                        prefix_char,
                    ));
            }
            return;
        }

        let parsed_format_string = prefix.map(|prefix_char| {
            FormatString::parse_printf_lossy_with_prefix(&format_string, prefix_char)
        });
        entries.push(TokenizedStringEntry {
            format_string,
            date_removed,
            parsed_format_string,
        });
    }

    /// Pre-parses all format strings currently in the database using the given `prefix`.
    pub fn preparse_format_strings(&mut self, prefix: char) {
        for domain_map in self.0.values_mut() {
            for entries in domain_map.values_mut() {
                for entry in entries.iter_mut() {
                    entry.parsed_format_string = Some(
                        FormatString::parse_printf_lossy_with_prefix(&entry.format_string, prefix),
                    );
                }
            }
        }
    }

    /// Consumes the database, pre-parses all format strings with `prefix`, and returns it.
    #[must_use]
    pub fn with_preparsed_format_strings(mut self, prefix: char) -> Self {
        self.preparse_format_strings(prefix);
        self
    }

    /// Returns true if any entry in the database has a pre-parsed format string.
    #[must_use]
    pub fn has_preparsed_entries(&self) -> bool {
        self.0.values().any(|domain_map| {
            domain_map.values().any(|entries| {
                entries
                    .iter()
                    .any(|entry| entry.parsed_format_string.is_some())
            })
        })
    }

    /// Returns true if the database contains no entries.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.0.is_empty() || self.0.values().all(HashMap::is_empty)
    }

    /// Returns the total number of entries in the database across all domains and tokens.
    #[must_use]
    pub fn len(&self) -> usize {
        self.0
            .values()
            .flat_map(|domain_map| domain_map.values())
            .map(Vec::len)
            .sum()
    }

    /// Looks up database entries for a given token and domain.
    #[must_use]
    pub fn lookup(&self, token: u32, domain: &str) -> &[TokenizedStringEntry] {
        let canonical_domain = crate::detokenize::canonicalize_domain(domain);
        self.0
            .get(&canonical_domain)
            .and_then(|domain_map| domain_map.get(&token))
            .map(|entries| entries.as_slice())
            .unwrap_or(&[])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_database_deduplicates_entries_and_preparses() {
        let mut db = Database::new();
        db.add_entry("", 0x10, "msg %d".to_string(), "2022-01-01".to_string());
        db.add_entry("", 0x10, "msg %d".to_string(), "2025-09-15".to_string());
        db.add_entry(
            "custom",
            0x20,
            "other $10#0000000001".to_string(),
            String::new(),
        );

        assert_eq!(db.len(), 2);
        assert_eq!(db.lookup(0x10, "")[0].date_removed, "2025-09-15");
        assert!(!db.has_preparsed_entries());

        db.preparse_format_strings('$');
        assert!(db.has_preparsed_entries());
        assert!(db.lookup(0x10, "")[0].parsed_format_string.is_some());
        assert!(db.lookup(0x20, "custom")[0].parsed_format_string.is_some());
    }
}
