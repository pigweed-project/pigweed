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

import (
	"fmt"
	"sort"
	"strings"

	"github.com/andygrunwald/go-gerrit"
)

// Note: Consider special handling/filtering for robot comments (e.g. from automated linters/builders) in the future.

// UnresolvedComment represents a single unresolved comment thread preview.
type UnresolvedComment struct {
	File          string `json:"file"`
	Line          int    `json:"line"`
	PatchSet      int    `json:"patchset"`
	Author        string `json:"author"`
	Message       string `json:"message"`
	HasDraftReply bool   `json:"has_draft_reply"`
}

// CommentsSummary contains summarized thread metrics and previews for a change.
type CommentsSummary struct {
	TotalThreads      int                 `json:"total_threads"`
	ResolvedThreads   int                 `json:"resolved_threads"`
	UnresolvedThreads int                 `json:"unresolved_threads"`
	DraftsCount       int                 `json:"drafts_count"`
	Unresolved        []UnresolvedComment `json:"unresolved"`
	FormattedText     string              `json:"formatted_text"`
}

// AnalyzeComments processes published comments and drafts into a CommentsSummary with hysteresis-based formatting.
func AnalyzeComments(published map[string][]gerrit.CommentInfo, drafts map[string][]gerrit.CommentInfo) CommentsSummary {
	draftsCount := 0
	draftReplies := make(map[string]bool)
	if drafts != nil {
		for _, fileDrafts := range drafts {
			draftsCount += len(fileDrafts)
			for _, d := range fileDrafts {
				if d.InReplyTo != "" {
					draftReplies[d.InReplyTo] = true
				}
			}
		}
	}

	var paths []string
	if published != nil {
		for p := range published {
			paths = append(paths, p)
		}
	}
	sort.Strings(paths)

	var allThreadsTotal int
	var resolvedCount int
	var unresolvedList []UnresolvedComment

	for _, path := range paths {
		fileComments := published[path]
		if len(fileComments) == 0 {
			continue
		}

		threads := BuildCommentThreads(path, fileComments, draftReplies)
		for _, th := range threads {
			allThreadsTotal++
			if !th.Unresolved {
				resolvedCount++
				continue
			}

			fileDisplay := path
			if path == "/PATCHSET_LEVEL" || path == "" {
				fileDisplay = "Change comment"
			}

			author := FormatAccount(th.Latest.Author)
			if author == "" {
				author = "Unknown"
			}

			ps := th.Latest.PatchSet
			if ps == 0 {
				ps = th.Root.Comment.PatchSet
			}

			unresolvedList = append(unresolvedList, UnresolvedComment{
				File:          fileDisplay,
				Line:          th.Line,
				PatchSet:      ps,
				Author:        author,
				Message:       strings.TrimSpace(th.Latest.Message),
				HasDraftReply: th.HasDraftReply,
			})
		}
	}

	formattedText := formatCommentsSummary(allThreadsTotal, len(unresolvedList), draftsCount, unresolvedList)

	return CommentsSummary{
		TotalThreads:      allThreadsTotal,
		ResolvedThreads:   resolvedCount,
		UnresolvedThreads: len(unresolvedList),
		DraftsCount:       draftsCount,
		Unresolved:        unresolvedList,
		FormattedText:     formattedText,
	}
}

func formatCommentSnippet(msg string, maxLen int) string {
	msg = strings.ReplaceAll(msg, "\r\n", " ")
	msg = strings.ReplaceAll(msg, "\n", " ")
	msg = strings.ReplaceAll(msg, "\t", " ")
	fields := strings.Fields(msg)
	clean := strings.Join(fields, " ")
	if len(clean) > maxLen {
		return clean[:maxLen-3] + "..."
	}
	return clean
}

func formatCommentsSummary(totalThreads, unresolvedCount, draftsCount int, unresolvedList []UnresolvedComment) string {
	if unresolvedCount == 0 {
		if totalThreads == 0 {
			if draftsCount == 0 {
				return "    None"
			}
			if draftsCount == 1 {
				return "    None (1 unpublished draft)"
			}
			return fmt.Sprintf("    None (%d unpublished drafts)", draftsCount)
		}

		threadWord := "threads"
		if totalThreads == 1 {
			threadWord = "thread"
		}
		if draftsCount == 0 {
			return fmt.Sprintf("    All resolved (%d %s)", totalThreads, threadWord)
		}
		if draftsCount == 1 {
			return fmt.Sprintf("    All resolved (%d %s, 1 unpublished draft)", totalThreads, threadWord)
		}
		return fmt.Sprintf("    All resolved (%d %s, %d unpublished drafts)", totalThreads, threadWord, draftsCount)
	}

	threadWord := "threads"
	if unresolvedCount == 1 {
		threadWord = "thread"
	}

	var b strings.Builder
	b.WriteString("\n")

	if unresolvedCount <= 2 {
		var details []string
		if totalThreads > unresolvedCount {
			details = append(details, fmt.Sprintf("out of %d threads", totalThreads))
		}
		if draftsCount > 0 {
			if draftsCount == 1 {
				details = append(details, "1 unpublished draft")
			} else {
				details = append(details, fmt.Sprintf("%d unpublished drafts", draftsCount))
			}
		}
		detailsStr := ""
		if len(details) > 0 {
			detailsStr = fmt.Sprintf(" (%s)", strings.Join(details, ", "))
		}
		fmt.Fprintf(&b, "      ⚠ %d unresolved %s%s:", unresolvedCount, threadWord, detailsStr)
	} else {
		var details []string
		details = append(details, "use 'gh pr view --comments' to view all")
		if draftsCount > 0 {
			if draftsCount == 1 {
				details = append(details, "1 unpublished draft")
			} else {
				details = append(details, fmt.Sprintf("%d unpublished drafts", draftsCount))
			}
		}
		fmt.Fprintf(&b, "      ⚠ %d unresolved threads (%s):", unresolvedCount, strings.Join(details, "; "))
	}

	maxDisplay := 2
	count := len(unresolvedList)
	if count > maxDisplay {
		count = maxDisplay
	}

	for i := 0; i < count; i++ {
		item := unresolvedList[i]
		loc := item.File
		if loc != "Change comment" && item.Line > 0 {
			loc = fmt.Sprintf("%s:%d", item.File, item.Line)
		}

		psTag := ""
		if item.PatchSet > 0 {
			psTag = fmt.Sprintf(" [PS%d]", item.PatchSet)
		}

		authorTag := ""
		if item.Author != "" && item.Author != "Unknown" {
			authorTag = fmt.Sprintf(" by %s", item.Author)
		}

		draftReplyTag := ""
		if item.HasDraftReply {
			draftReplyTag = " (has unpublished draft reply)"
		}

		snippet := formatCommentSnippet(item.Message, 80)
		fmt.Fprintf(&b, "\n        • %s%s%s%s:\n          \"%s\"", loc, psTag, authorTag, draftReplyTag, snippet)
	}

	if len(unresolvedList) > maxDisplay {
		remaining := len(unresolvedList) - maxDisplay
		remWord := "threads"
		if remaining == 1 {
			remWord = "thread"
		}
		fmt.Fprintf(&b, "\n        ... and %d more unresolved %s", remaining, remWord)
	}

	return b.String()
}
