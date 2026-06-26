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

#![cfg(feature = "std")]

use std::collections::HashMap;

use pw_status::Result;

use super::TokenizedStringEntry;

/// Parses a CSV string into a vector of rows, where each row is a vector of strings.
///
/// This parser handles quotes (including newlines inside quotes), empty lines,
/// and error recovery by skipping lines with errors.
pub fn parse_csv(csv: &str) -> Vec<Vec<String>> {
    let mut results = Vec::new();
    let mut row = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;
    let mut quote_closed = false;
    let mut chars = csv.chars().peekable();
    let mut row_has_content = false;

    while let Some(c) = chars.next() {
        match c {
            '\r' => continue,
            '\n' => {
                if in_quotes {
                    current.push(c);
                } else {
                    row.push(current);
                    if row_has_content || row.len() > 1 || !row[0].is_empty() {
                        results.push(row);
                    }
                    row = Vec::new();
                    current = String::new();
                    quote_closed = false;
                    row_has_content = false;
                }
            }
            '"' => {
                row_has_content = true;
                if in_quotes {
                    if let Some(&'"') = chars.peek() {
                        chars.next(); // consume second quote
                        current.push('"');
                    } else {
                        in_quotes = false;
                        quote_closed = true;
                    }
                } else {
                    if !current.is_empty() || quote_closed {
                        // Error case, skip to newline.
                        skip_to_newline(&mut chars);
                        row = Vec::new();
                        current = String::new();
                        quote_closed = false;
                        row_has_content = false;
                        continue;
                    }
                    in_quotes = true;
                }
            }
            ',' => {
                row_has_content = true;
                if in_quotes {
                    current.push(c);
                } else {
                    row.push(current);
                    current = String::new();
                    quote_closed = false;
                }
            }
            _ => {
                row_has_content = true;
                if quote_closed {
                    // Error case, skip to newline.
                    skip_to_newline(&mut chars);
                    row = Vec::new();
                    current = String::new();
                    quote_closed = false;
                    row_has_content = false;
                    continue;
                }
                current.push(c);
            }
        }
    }
    // Handle last line if no trailing newline
    if in_quotes {
        // Error, unclosed quote at EOF. Skip row.
    } else if row_has_content || row.len() > 1 || !current.is_empty() {
        row.push(current);
        results.push(row);
    }
    results
}

fn skip_to_newline(chars: &mut core::iter::Peekable<core::str::Chars>) {
    for c in chars.by_ref() {
        if c == '\n' {
            break;
        }
    }
}

pub fn parse_csv_database(
    csv: &str,
) -> Result<HashMap<String, HashMap<u32, Vec<TokenizedStringEntry>>>> {
    let mut database: HashMap<String, HashMap<u32, Vec<TokenizedStringEntry>>> = HashMap::new();

    let parsed_csv = parse_csv(csv);

    for row in parsed_csv {
        if row.len() != 4 {
            continue;
        }

        let token_str = row[0].trim();
        let date_str = row[1].trim();
        let domain = super::canonicalize_domain(&row[2]);
        let format_string = &row[3];

        let token = match u32::from_str_radix(token_str, 16) {
            Ok(t) => t,
            Err(_) => return Err(pw_status::Error::InvalidArgument),
        };

        if !super::is_valid_date(date_str) {
            return Err(pw_status::Error::InvalidArgument);
        }

        super::add_entry(
            &mut database,
            &domain,
            token,
            format_string.to_string(),
            date_str.to_string(),
        );
    }

    Ok(database)
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
}
