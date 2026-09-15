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

package pw_ghish

import "strings"

// BugLink is one bug referenced by a change, read from its commit message.
//
// This is the Gerrit answer to GitHub's `closingIssuesReferences`: it lets a
// caller that just wrote a bug link read it back and confirm it took. Closes
// distinguishes the two Gerrit trailers that GitHub does not: `Bug:` links a
// bug, `Fixed:` links it and closes it when the change is submitted.
//
// There is deliberately no URL field. A `b/` ID resolves against whichever
// tracker the host is wired to, and that is not derivable from the trailer.
// Emitting a guessed URL that points at the wrong tracker would be worse than
// emitting none, the same reasoning that makes gh-ish refuse to translate
// `Fixes #456` rather than guess at `b/456`.
type BugLink struct {
	ID     string `json:"id"`
	Closes bool   `json:"closes"`
}

// ExtractBugLinks returns the bugs referenced by a commit message, in the
// order they appear. The result is never nil, so that it serializes as `[]`
// rather than `null`: a caller asking "which bugs?" should get an empty list,
// not a missing one.
//
// Only trailers count, so a `Bug:` line inside a prose paragraph is left
// alone -- the same rule ExtractTrailers applies, and the same rule Gerrit
// applies when it decides what to link.
//
// A bug named by both a `Bug:` and a `Fixed:` trailer appears once, with
// Closes set: the change does close it, and reporting it twice would make a
// caller think two bugs are linked.
//
// A value that is not a bug reference at all -- `Bug: none`, or free text --
// is reported verbatim rather than dropped. "The author wrote none" and "the
// author wrote nothing" are different facts, and silently collapsing them
// would hide an explicit decision.
func ExtractBugLinks(commitMsg string) []BugLink {
	links := []BugLink{}
	index := make(map[string]int)

	add := func(id string, closes bool) {
		if id == "" {
			return
		}
		if i, ok := index[id]; ok {
			// Linked and fixed is fixed.
			links[i].Closes = links[i].Closes || closes
			return
		}
		index[id] = len(links)
		links = append(links, BugLink{ID: id, Closes: closes})
	}

	for _, trailer := range ExtractTrailers(commitMsg) {
		_, value, closes, ok := parseBugTrailer(trailer)
		if !ok {
			continue
		}
		ids := bugChainIDs(value)
		if len(ids) == 0 {
			// Not a list of bug references: `none`, or free text.
			add(NormalizeBugChain(value), closes)
			continue
		}
		for _, id := range ids {
			add(id, closes)
		}
	}

	return links
}

// FormatBugLinks renders bug links as a flat, comma-separated list of IDs, to
// match how pr view presents its other list-valued fields.
func FormatBugLinks(links []BugLink) string {
	ids := make([]string, 0, len(links))
	for _, l := range links {
		ids = append(ids, l.ID)
	}
	return strings.Join(ids, ", ")
}
