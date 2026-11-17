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

//! CSV token database parser.

use pw_status::{Error, Result};

use crate::detokenize::database::Database;

/// Parser state for a single CSV row.
#[derive(Debug, Default)]
struct CsvRowState {
    /// Completed fields parsed so far in the current row.
    row: Vec<String>,
    /// Characters accumulated for the field currently being parsed.
    current_field: String,
    /// Whether the parser is currently inside a double-quoted field.
    in_quotes: bool,
    /// Whether a closing quote was just encountered in the current field.
    quote_closed: bool,
    /// Whether the current line has encountered any non-newline characters.
    has_content: bool,
}

impl CsvRowState {
    fn finish_field(&mut self) {
        self.row.push(core::mem::take(&mut self.current_field));
        self.quote_closed = false;
    }

    fn finish_row(&mut self, results: &mut Vec<Vec<String>>) {
        self.row.push(core::mem::take(&mut self.current_field));
        if self.has_content || self.row.len() > 1 || !self.row[0].is_empty() {
            results.push(core::mem::take(&mut self.row));
        } else {
            self.row.clear();
        }
        self.quote_closed = false;
        self.has_content = false;
    }

    fn discard_corrupt_line(&mut self, chars: &mut core::iter::Peekable<core::str::Chars>) {
        for ch in chars.by_ref() {
            if ch == '\n' {
                break;
            }
        }
        self.row.clear();
        self.current_field.clear();
        self.in_quotes = false;
        self.quote_closed = false;
        self.has_content = false;
    }
}

/// Parses a CSV string into a vector of rows, where each row is a vector of strings.
///
/// This parser handles quotes (including newlines inside quotes), empty lines,
/// and error recovery by skipping lines with errors.
#[must_use]
pub fn parse_csv(csv: &str) -> Vec<Vec<String>> {
    let mut results = Vec::new();
    let mut state = CsvRowState::default();
    let mut chars = csv.chars().peekable();

    while let Some(ch) = chars.next() {
        match ch {
            '\r' => continue,
            '\n' if state.in_quotes => state.current_field.push(ch),
            '\n' => state.finish_row(&mut results),
            '"' if state.in_quotes => {
                state.has_content = true;
                if chars.next_if_eq(&'"').is_some() {
                    state.current_field.push('"');
                } else {
                    state.in_quotes = false;
                    state.quote_closed = true;
                }
            }
            '"' => {
                state.has_content = true;
                if !state.current_field.is_empty() || state.quote_closed {
                    state.discard_corrupt_line(&mut chars);
                    continue;
                }
                state.in_quotes = true;
            }
            ',' if state.in_quotes => {
                state.has_content = true;
                state.current_field.push(ch);
            }
            ',' => {
                state.has_content = true;
                state.finish_field();
            }
            _ => {
                state.has_content = true;
                if state.quote_closed {
                    state.discard_corrupt_line(&mut chars);
                    continue;
                }
                state.current_field.push(ch);
            }
        }
    }

    // Handle last line if no trailing newline and quotes are balanced.
    if !state.in_quotes
        && (state.has_content || state.row.len() > 1 || !state.current_field.is_empty())
    {
        state.finish_row(&mut results);
    }

    results
}

/// Parses a CSV token database string into a [`Database`].
///
/// # Errors
///
/// Returns [`Error::InvalidArgument`] if any row contains an invalid hex token
/// or malformed ISO date string.
pub fn parse_csv_database(csv: &str) -> Result<Database> {
    parse_csv_database_internal(csv, None)
}

/// Parses a CSV token database string into a [`Database`], pre-parsing nested token
/// specifications using `prefix`.
///
/// # Errors
///
/// Returns [`Error::InvalidArgument`] if any row contains an invalid hex token
/// or malformed ISO date string.
pub fn parse_csv_database_preparsed(csv: &str, prefix: char) -> Result<Database> {
    parse_csv_database_internal(csv, Some(prefix))
}

fn parse_csv_database_internal(csv: &str, prefix: Option<char>) -> Result<Database> {
    let mut database = Database::new();

    for row in parse_csv(csv) {
        if row.len() != 4 {
            continue;
        }

        let token_str = row[0].trim();
        let date_str = row[1].trim();
        let domain = &row[2];
        let format_string = &row[3];

        let Ok(token) = u32::from_str_radix(token_str, 16) else {
            return Err(Error::InvalidArgument);
        };

        if !is_valid_date(date_str) {
            return Err(Error::InvalidArgument);
        }

        database.add_entry_with_prefix(
            domain,
            token,
            format_string.to_string(),
            date_str.to_string(),
            prefix,
        );
    }

    Ok(database)
}

fn is_valid_date(date_str: &str) -> bool {
    const ISO_DATE_LEN: usize = 10;
    const ISO_DATE_YEAR_SEP_IDX: usize = 4;
    const ISO_DATE_MONTH_SEP_IDX: usize = 7;

    if date_str.is_empty() {
        return true;
    }
    if date_str.len() != ISO_DATE_LEN {
        return false;
    }
    date_str.chars().enumerate().all(|(idx, ch)| match idx {
        ISO_DATE_YEAR_SEP_IDX | ISO_DATE_MONTH_SEP_IDX => ch == '-',
        _ => ch.is_ascii_digit(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_read_file() {
        let csv = "abc,def,ghi\n\
                   \"\",\"\"\"\",\n\
                   123,\"\",\"4\"";

        let result = parse_csv(csv);
        assert_eq!(result.len(), 3);

        assert_eq!(result[0][0], "abc");
        assert_eq!(result[0][1], "def");
        assert_eq!(result[0][2], "ghi");
        assert_eq!(result[1][0], "");
        assert_eq!(result[1][1], "\"");
        assert_eq!(result[1][2], "");
        assert_eq!(result[2][0], "123");
        assert_eq!(result[2][1], "");
        assert_eq!(result[2][2], "4");
    }

    #[test]
    fn test_empty_lines() {
        let csv = "\n\n\r\n\r\n";
        let result = parse_csv(csv);
        assert_eq!(result.len(), 0);
    }

    #[test]
    fn test_empty_lines_with_text_interspersed() {
        let csv = "\n\n\r \n\r\n\r\n\r,\r\n";
        let result = parse_csv(csv);
        assert_eq!(result.len(), 2);
        assert_eq!(result[0].len(), 1);
        assert_eq!(result[0][0], " ");

        assert_eq!(result[1].len(), 2);
        assert_eq!(result[1][0], "");
        assert_eq!(result[1][1], "");
    }

    #[test]
    fn test_varying_columns() {
        let csv = "\n\
                   a\r\n\
                   b\r\n\
                   ,\r\n\
                   c,d,,\r\n\
                   \x20, ,\"\n\"\n\
                   e,fg,hijk,lmno ";

        let expected: Vec<Vec<String>> = vec![
            vec!["a".to_string()],
            vec!["b".to_string()],
            vec!["".to_string(), "".to_string()],
            vec![
                "c".to_string(),
                "d".to_string(),
                "".to_string(),
                "".to_string(),
            ],
            vec![" ".to_string(), " ".to_string(), "\n".to_string()],
            vec![
                "e".to_string(),
                "fg".to_string(),
                "hijk".to_string(),
                "lmno ".to_string(),
            ],
        ];

        let result = parse_csv(csv);
        assert_eq!(result.len(), 6);
        for i in 0..6 {
            assert_eq!(result[i], expected[i]);
        }
    }

    #[test]
    fn test_error_no_lines() {
        let result = parse_csv(r#"11,"abc"., 13 "#);
        assert_eq!(result.len(), 0);
    }

    #[test]
    fn test_error_skips_only_errors() {
        let csv = "a,b,c\n\
                   1,\"2\".,3\n\
                   d,e\r\n\
                   \"456\n\r\" 789\r\n\r\n\
                   f,g,h\n\
                   \"0";
        let expected: Vec<Vec<String>> = vec![
            vec!["a".to_string(), "b".to_string(), "c".to_string()],
            vec!["d".to_string(), "e".to_string()],
            vec!["f".to_string(), "g".to_string(), "h".to_string()],
        ];
        let result = parse_csv(csv);
        assert_eq!(result, expected);
    }

    #[test]
    fn test_from_csv_bad_date_and_bad_token() {
        assert_eq!(
            parse_csv_database("00000001,2026/01/01,,hello\n").unwrap_err(),
            Error::InvalidArgument
        );
        assert_eq!(
            parse_csv_database("not_hex,2026-01-01,,hello\n").unwrap_err(),
            Error::InvalidArgument
        );
    }

    #[test]
    fn test_from_csv_duplicate_entries_uses_newest_removal_date() {
        let csv = "00000001,2020-01-01,,same message\n\
                   00000001,2025-05-10,,same message\n\
                   00000001,2022-03-04,,same message\n";
        let db = parse_csv_database(csv).unwrap();
        let entries = db.lookup(1, "");
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].date_removed, "2025-05-10");
    }
}
