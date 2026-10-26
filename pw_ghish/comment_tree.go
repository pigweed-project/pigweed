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
	"cmp"
	"fmt"
	"maps"
	"slices"
	"strings"

	"github.com/andygrunwald/go-gerrit"
)

// CommentNode represents a single node within a threaded comment hierarchy.
type CommentNode struct {
	Comment  gerrit.CommentInfo
	IsDraft  bool
	Children []*CommentNode
}

// CommentThread represents a complete conversation thread on a specific file.
type CommentThread struct {
	Path          string
	Root          *CommentNode
	Comments      []gerrit.CommentInfo
	Latest        gerrit.CommentInfo
	Line          int
	Unresolved    bool
	HasDraftReply bool
}

// isNewerComment reports whether candidate is more recent than current based on
// Updated timestamp, falling back to PatchSet number and ID.
func isNewerComment(candidate, current *gerrit.CommentInfo) bool {
	if current == nil {
		return true
	}
	if (current.Updated == nil) != (candidate.Updated == nil) {
		return candidate.Updated != nil
	}
	if current.Updated != nil && candidate.Updated != nil && !candidate.Updated.Time.Equal(current.Updated.Time) {
		return candidate.Updated.Time.After(current.Updated.Time)
	}
	if candidate.PatchSet != current.PatchSet {
		return candidate.PatchSet > current.PatchSet
	}
	return false
}

// BuildCommentForest organizes a flat list of comments on a single file into a
// forest (slice of root trees), resolving InReplyTo parentage, detecting cycles,
// promoting orphaned replies to roots, and sorting threads by line and timestamp.
func BuildCommentForest(comments []gerrit.CommentInfo) []*CommentNode {
	return BuildCommentForestWithDrafts(comments, nil)
}

// BuildCommentForestWithDrafts organizes published comments and unpublished drafts
// on a single file into a unified threaded forest. Draft replies attach to their
// parent published or draft comment via InReplyTo, while standalone drafts become
// root nodes marked with IsDraft.
func BuildCommentForestWithDrafts(published, drafts []gerrit.CommentInfo) []*CommentNode {
	total := len(published) + len(drafts)
	if total == 0 {
		return nil
	}

	nodes := make(map[string]*CommentNode, total)
	ordered := make([]*CommentNode, 0, total)

	for _, c := range published {
		n := &CommentNode{Comment: c, IsDraft: false}
		nodes[c.ID] = n
		ordered = append(ordered, n)
	}
	for _, d := range drafts {
		n := &CommentNode{Comment: d, IsDraft: true}
		nodes[d.ID] = n
		ordered = append(ordered, n)
	}

	var roots []*CommentNode
	for _, node := range ordered {
		c := node.Comment

		// Disallow self-parenting
		if c.InReplyTo != "" && c.InReplyTo != c.ID {
			if parent, ok := nodes[c.InReplyTo]; ok {
				// Prevent circular ancestor loops
				if !createsCycle(node, parent, nodes) {
					parent.Children = append(parent.Children, node)
					continue
				}
			}
		}
		// Root comment or orphan whose parent is missing / cyclic
		roots = append(roots, node)
	}

	// Sort each node's children chronologically (drafts sort after published at identical timestamps)
	for _, node := range nodes {
		if len(node.Children) > 1 {
			slices.SortFunc(node.Children, compareCommentNodesChronologically)
		}
	}

	// Sort root threads by line number ascending, then chronologically
	slices.SortFunc(roots, func(a, b *CommentNode) int {
		if c := cmp.Compare(a.Comment.Line, b.Comment.Line); c != 0 {
			return c
		}
		return compareCommentNodesChronologically(a, b)
	})

	return roots
}

func compareCommentNodesChronologically(a, b *CommentNode) int {
	ca := a.Comment
	cb := b.Comment
	if (ca.Updated == nil) != (cb.Updated == nil) {
		if ca.Updated == nil {
			return -1
		}
		return 1
	}
	if ca.Updated != nil && cb.Updated != nil && !ca.Updated.Time.Equal(cb.Updated.Time) {
		return ca.Updated.Time.Compare(cb.Updated.Time)
	}
	if c := cmp.Compare(ca.PatchSet, cb.PatchSet); c != 0 {
		return c
	}
	if a.IsDraft != b.IsDraft {
		if !a.IsDraft {
			return -1
		}
		return 1
	}
	return cmp.Compare(ca.ID, cb.ID)
}

func createsCycle(child *CommentNode, parent *CommentNode, nodes map[string]*CommentNode) bool {
	curr := parent
	visited := make(map[string]bool)
	for curr != nil {
		if curr == child || visited[curr.Comment.ID] {
			return true
		}
		visited[curr.Comment.ID] = true
		if curr.Comment.InReplyTo == "" || curr.Comment.InReplyTo == curr.Comment.ID {
			break
		}
		curr = nodes[curr.Comment.InReplyTo]
	}
	return false
}

func collectThreadNodes(root *CommentNode) []*CommentNode {
	var nodes []*CommentNode
	var collect func(n *CommentNode)
	collect = func(n *CommentNode) {
		if n == nil {
			return
		}
		nodes = append(nodes, n)
		for _, ch := range n.Children {
			collect(ch)
		}
	}
	collect(root)
	return nodes
}

// isOlderOrEarlierInThread reports whether an earlier-visited node a is older
// than a later-visited node b by Updated timestamp, PatchSet, or IsDraft
// status, falling back to tree traversal order when those fields are equal.
func isOlderOrEarlierInThread(a, b *CommentNode) bool {
	ca := a.Comment
	cb := b.Comment
	if (ca.Updated == nil) != (cb.Updated == nil) {
		return ca.Updated == nil
	}
	if ca.Updated != nil && cb.Updated != nil && !ca.Updated.Time.Equal(cb.Updated.Time) {
		return ca.Updated.Time.Before(cb.Updated.Time)
	}
	if ca.PatchSet != cb.PatchSet {
		return ca.PatchSet < cb.PatchSet
	}
	if a.IsDraft != b.IsDraft {
		return !a.IsDraft
	}
	return true
}

// resolveThreadUnresolved finds the chronologically latest explicit Unresolved
// setting across all nodes in a thread. When includeDrafts is false, draft
// nodes are skipped.
func resolveThreadUnresolved(nodes []*CommentNode, includeDrafts bool) *bool {
	var latest *CommentNode
	for _, n := range nodes {
		if n == nil || (!includeDrafts && n.IsDraft) || n.Comment.Unresolved == nil {
			continue
		}
		if latest == nil || isOlderOrEarlierInThread(latest, n) {
			latest = n
		}
	}
	if latest == nil {
		return nil
	}
	return latest.Comment.Unresolved
}

// BuildCommentThreads builds structured CommentThread instances for a file,
// computing resolution status, draft reply flags, and latest active comment.
func BuildCommentThreads(path string, fileComments []gerrit.CommentInfo, draftReplies map[string]bool) []*CommentThread {
	roots := BuildCommentForest(fileComments)
	if len(roots) == 0 {
		return nil
	}

	threads := make([]*CommentThread, 0, len(roots))
	for _, root := range roots {
		nodes := collectThreadNodes(root)
		inThread := make([]gerrit.CommentInfo, len(nodes))
		for i, n := range nodes {
			inThread[i] = n.Comment
		}

		hasDraftReply := false
		if draftReplies != nil {
			for _, c := range inThread {
				if draftReplies[c.ID] {
					hasDraftReply = true
					break
				}
			}
		}

		// Find latest comment in thread
		latest := root.Comment
		for i := range inThread {
			if isNewerComment(&inThread[i], &latest) {
				latest = inThread[i]
			}
		}

		isUnresolved := false
		if u := resolveThreadUnresolved(nodes, true); u != nil {
			isUnresolved = *u
		}

		lineNum := root.Comment.Line
		if lineNum == 0 && latest.Line > 0 {
			lineNum = latest.Line
		}

		threads = append(threads, &CommentThread{
			Path:          path,
			Root:          root,
			Comments:      inThread,
			Latest:        latest,
			Line:          lineNum,
			Unresolved:    isUnresolved,
			HasDraftReply: hasDraftReply,
		})
	}

	return threads
}

// CountUnresolvedThreads returns the number of unresolved comment threads in published.
func CountUnresolvedThreads(published map[string][]gerrit.CommentInfo) int {
	count := 0
	for path, fileComments := range published {
		if len(fileComments) == 0 {
			continue
		}
		for _, th := range BuildCommentThreads(path, fileComments, nil) {
			if th.Unresolved {
				count++
			}
		}
	}
	return count
}

// FindLatestCommentAtLine searches a slice of comments on a file for the most recent
// comment located at the specified line number.
func FindLatestCommentAtLine(fileComments []gerrit.CommentInfo, line int) *gerrit.CommentInfo {
	return FindLatestCommentAtLineForPatchSet(fileComments, line, 0)
}

// FindLatestCommentAtLineForPatchSet searches a slice of comments on a file for the
// most recent comment at line, optionally restricted to patchSet (when patchSet > 0).
func FindLatestCommentAtLineForPatchSet(fileComments []gerrit.CommentInfo, line int, patchSet int) *gerrit.CommentInfo {
	var latest *gerrit.CommentInfo
	for i := range fileComments {
		c := &fileComments[i]
		if c.Line != line {
			continue
		}
		if patchSet > 0 && c.PatchSet != 0 && c.PatchSet != patchSet {
			continue
		}
		if isNewerComment(c, latest) {
			latest = c
		}
	}
	return latest
}

// FindMatchingDraftsAtTarget returns all draft comments in fileDrafts that match
// the target location.
//
//   - If patchSet > 0, only drafts with PatchSet == patchSet (or PatchSet == 0) are matched.
//   - If inReplyTo != "", only draft replies with d.InReplyTo == inReplyTo are matched;
//     standalone drafts (InReplyTo == "") and draft replies on other threads on the same
//     line are never matched so unrelated drafts are never clobbered.
//   - If inReplyTo == "", all drafts at line are matched.
func FindMatchingDraftsAtTarget(fileDrafts []gerrit.CommentInfo, line int, inReplyTo string, patchSet int) []gerrit.CommentInfo {
	if len(fileDrafts) == 0 {
		return nil
	}
	matchesPatchSet := func(d gerrit.CommentInfo) bool {
		return patchSet <= 0 || d.PatchSet == 0 || d.PatchSet == patchSet
	}

	if inReplyTo != "" {
		var exactReplies []gerrit.CommentInfo
		for _, d := range fileDrafts {
			if !matchesPatchSet(d) {
				continue
			}
			if d.InReplyTo == inReplyTo {
				exactReplies = append(exactReplies, d)
			}
		}
		return exactReplies
	}

	var matches []gerrit.CommentInfo
	for _, d := range fileDrafts {
		if !matchesPatchSet(d) {
			continue
		}
		if d.Line == line {
			matches = append(matches, d)
		}
	}
	return matches
}

// FindDraftAtTarget searches a slice of draft comments on a file for an existing
// draft to update or delete. When inReplyTo is non-empty, only draft replies are
// considered so standalone drafts on the same line are not clobbered.
func FindDraftAtTarget(fileDrafts []gerrit.CommentInfo, line int, inReplyTo string) *gerrit.CommentInfo {
	matches := FindMatchingDraftsAtTarget(fileDrafts, line, inReplyTo, 0)
	var latest *gerrit.CommentInfo
	for i := range matches {
		if isNewerComment(&matches[i], latest) {
			latest = &matches[i]
		}
	}
	return latest
}

// isPatchsetLevelPath reports whether path refers to Gerrit's top-level
// patchset comment pseudo-path.
func isPatchsetLevelPath(path string) bool {
	return path == "" || path == "/PATCHSET_LEVEL"
}

// formatPatchSetTag returns " [PS<N>]" when patchSet > 0, or "" otherwise.
func formatPatchSetTag(patchSet int) string {
	if patchSet > 0 {
		return fmt.Sprintf(" [PS%d]", patchSet)
	}
	return ""
}

// formatShortCommentTarget formats a file/line target for human-readable output.
func formatShortCommentTarget(path string, line int) string {
	if isPatchsetLevelPath(path) {
		return "Change comment"
	}
	if line > 0 {
		return fmt.Sprintf("%s:%d", path, line)
	}
	return path
}

// formatCommentTargetWithPatchSet formats a file/line target and optional [PS<N>] tag.
func formatCommentTargetWithPatchSet(path string, line int, patchSet int) string {
	return formatShortCommentTarget(path, line) + formatPatchSetTag(patchSet)
}

func formatCommentNodeAuthor(n *CommentNode, depth int) string {
	author := FormatAccount(n.Comment.Author)
	if !n.IsDraft {
		if author == "" {
			return "Unknown"
		}
		return author
	}

	badge := "[DRAFT]"
	if depth > 0 && n.Comment.Unresolved != nil {
		if *n.Comment.Unresolved {
			badge = "[DRAFT, unresolved]"
		} else {
			badge = "[DRAFT, resolved]"
		}
	}
	if author == "" {
		return badge
	}
	return author + " " + badge
}

func formatThreadResolutionTag(path string, root *CommentNode) string {
	nodes := collectThreadNodes(root)
	unresolved := resolveThreadUnresolved(nodes, false)
	if unresolved == nil {
		unresolved = resolveThreadUnresolved(nodes, true)
	}
	if unresolved == nil {
		return ""
	}
	if *unresolved {
		return " [unresolved]"
	}
	// Single-node change-level (/PATCHSET_LEVEL) comments default to
	// Unresolved=false in Gerrit even when they are plain non-threaded
	// comments or drafts; omit the noisy [resolved] tag unless it is a
	// multi-comment thread.
	if isPatchsetLevelPath(path) && len(nodes) == 1 {
		return ""
	}
	return " [resolved]"
}

// CommentFormatOptions configures resolved-thread suppression and unresolved
// thread hysteresis folding in FormatCommentForestWithOptions.
type CommentFormatOptions struct {
	IncludeAll            bool
	MaxExpandedUnresolved int
}

func isActionableThread(root *CommentNode) bool {
	nodes := collectThreadNodes(root)
	for _, n := range nodes {
		if n.IsDraft {
			return true
		}
	}
	if u := resolveThreadUnresolved(nodes, false); u != nil && *u {
		return true
	}
	return false
}

func latestThreadNode(root *CommentNode) *CommentNode {
	nodes := collectThreadNodes(root)
	var latest *CommentNode
	for _, n := range nodes {
		if latest == nil || isOlderOrEarlierInThread(latest, n) {
			latest = n
		}
	}
	if latest == nil {
		return root
	}
	return latest
}

func printCommentTreeNode(b *strings.Builder, path string, n *CommentNode, depth int, parentPatchSet int) {
	author := formatCommentNodeAuthor(n, depth)
	message := strings.TrimSpace(n.Comment.Message)

	indent := strings.Repeat("  ", depth)
	if depth == 0 {
		line := "-"
		if n.Comment.Line != 0 {
			line = fmt.Sprintf("%d", n.Comment.Line)
		}
		psTag := formatPatchSetTag(n.Comment.PatchSet)
		statusTag := formatThreadResolutionTag(path, n)
		fmt.Fprintf(b, "  Line %s%s: %s%s\n    %s\n", line, psTag, author, statusTag, message)
	} else {
		psTag := ""
		if n.Comment.PatchSet > 0 && n.Comment.PatchSet != parentPatchSet {
			psTag = formatPatchSetTag(n.Comment.PatchSet)
		}
		indentedMsg := strings.ReplaceAll(message, "\n", "\n"+indent+"       ")
		fmt.Fprintf(b, "%s  -> %s%s: %s\n", indent, author, psTag, indentedMsg)
	}

	for _, child := range n.Children {
		printCommentTreeNode(b, path, child, depth+1, n.Comment.PatchSet)
	}
}

// FormatCommentForest renders a nested, human-readable terminal tree of comments
// grouped by file in alphabetical order.
func FormatCommentForest(comments map[string][]gerrit.CommentInfo) string {
	return FormatCommentForestWithDrafts(comments, nil)
}

// FormatCommentForestWithDrafts renders a nested, human-readable terminal tree
// combining published comments and unpublished drafts grouped by file in
// alphabetical order.
func FormatCommentForestWithDrafts(published, drafts map[string][]gerrit.CommentInfo) string {
	return FormatCommentForestWithOptions(published, drafts, CommentFormatOptions{IncludeAll: true})
}

// FormatCommentForestWithOptions renders a nested, human-readable terminal tree
// combining published comments and unpublished drafts grouped by file in
// alphabetical order, optionally suppressing resolved threads and folding excess
// unresolved threads when opts.IncludeAll is false.
func FormatCommentForestWithOptions(published, drafts map[string][]gerrit.CommentInfo, opts CommentFormatOptions) string {
	if len(published) == 0 && len(drafts) == 0 {
		return ""
	}

	pathSet := make(map[string]bool)
	for path, list := range published {
		if len(list) > 0 {
			pathSet[path] = true
		}
	}
	for path, list := range drafts {
		if len(list) > 0 {
			pathSet[path] = true
		}
	}
	if len(pathSet) == 0 {
		return ""
	}

	type fileForest struct {
		path  string
		roots []*CommentNode
	}
	var forests []fileForest
	actionableCount := 0
	resolvedCount := 0

	for _, path := range slices.Sorted(maps.Keys(pathSet)) {
		var filePublished []gerrit.CommentInfo
		if published != nil {
			filePublished = published[path]
		}
		var fileDrafts []gerrit.CommentInfo
		if drafts != nil {
			fileDrafts = drafts[path]
		}

		roots := BuildCommentForestWithDrafts(filePublished, fileDrafts)
		if len(roots) == 0 {
			continue
		}
		forests = append(forests, fileForest{path: path, roots: roots})
		for _, r := range roots {
			if isActionableThread(r) {
				actionableCount++
			} else {
				resolvedCount++
			}
		}
	}

	if len(forests) == 0 {
		return ""
	}

	suppressResolved := !opts.IncludeAll && resolvedCount > 0 && (actionableCount > 0 || resolvedCount > 2)
	maxExpanded := opts.MaxExpandedUnresolved
	if maxExpanded <= 0 {
		maxExpanded = 5
	}
	foldUnresolved := !opts.IncludeAll && actionableCount > maxExpanded

	type foldedThread struct {
		path string
		root *CommentNode
	}
	var folded []foldedThread
	expandedActionable := 0

	var b strings.Builder
	for _, ff := range forests {
		var rootsToPrint []*CommentNode
		for _, r := range ff.roots {
			if isActionableThread(r) {
				if foldUnresolved && expandedActionable >= maxExpanded {
					folded = append(folded, foldedThread{path: ff.path, root: r})
					continue
				}
				expandedActionable++
				rootsToPrint = append(rootsToPrint, r)
			} else {
				if suppressResolved {
					continue
				}
				rootsToPrint = append(rootsToPrint, r)
			}
		}

		if len(rootsToPrint) == 0 {
			continue
		}

		if isPatchsetLevelPath(ff.path) {
			fmt.Fprintf(&b, "File: /PATCHSET_LEVEL (Change comment)\n")
		} else {
			fmt.Fprintf(&b, "File: %s\n", ff.path)
		}

		for _, r := range rootsToPrint {
			printCommentTreeNode(&b, ff.path, r, 0, 0)
		}
		fmt.Fprintln(&b)
	}

	if len(folded) > 0 {
		for _, ft := range folded {
			latest := latestThreadNode(ft.root)
			lineNum := ft.root.Comment.Line
			if lineNum == 0 && latest.Comment.Line > 0 {
				lineNum = latest.Comment.Line
			}
			ps := latest.Comment.PatchSet
			if ps == 0 {
				ps = ft.root.Comment.PatchSet
			}
			loc := formatCommentTargetWithPatchSet(ft.path, lineNum, ps)
			author := formatCommentNodeAuthor(latest, 0)
			snippet := formatCommentSnippet(latest.Comment.Message, 80)
			fmt.Fprintf(&b, "  • %s %s: %s\n", loc, author, snippet)
		}
		fmt.Fprintf(&b, "  (use --all to expand all %d unresolved threads)\n", actionableCount)
	}

	if suppressResolved {
		fmt.Fprintf(&b, "  (✓ %d resolved thread(s) omitted; use --all to view all)\n", resolvedCount)
	}

	return b.String()
}
