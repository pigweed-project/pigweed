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

// CommentNode represents a single node within a threaded comment hierarchy.
type CommentNode struct {
	Comment  gerrit.CommentInfo
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

// BuildCommentForest organizes a flat list of comments on a single file into a
// forest (slice of root trees), resolving InReplyTo parentage, detecting cycles,
// promoting orphaned replies to roots, and sorting threads by line and timestamp.
func BuildCommentForest(comments []gerrit.CommentInfo) []*CommentNode {
	if len(comments) == 0 {
		return nil
	}

	nodes := make(map[string]*CommentNode, len(comments))
	for _, c := range comments {
		nodes[c.ID] = &CommentNode{Comment: c}
	}

	var roots []*CommentNode
	for _, c := range comments {
		node := nodes[c.ID]

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

	// Sort each node's children chronologically
	for _, node := range nodes {
		if len(node.Children) > 1 {
			sort.Slice(node.Children, func(i, j int) bool {
				ci := node.Children[i].Comment
				cj := node.Children[j].Comment
				if ci.Updated != nil && cj.Updated != nil && !ci.Updated.Time.Equal(cj.Updated.Time) {
					return ci.Updated.Time.Before(cj.Updated.Time)
				}
				if ci.PatchSet != cj.PatchSet {
					return ci.PatchSet < cj.PatchSet
				}
				return ci.ID < cj.ID
			})
		}
	}

	// Sort root threads by line number ascending, then by updated timestamp / ID
	sort.Slice(roots, func(i, j int) bool {
		if roots[i].Comment.Line != roots[j].Comment.Line {
			return roots[i].Comment.Line < roots[j].Comment.Line
		}
		ri := roots[i].Comment
		rj := roots[j].Comment
		if ri.Updated != nil && rj.Updated != nil && !ri.Updated.Time.Equal(rj.Updated.Time) {
			return ri.Updated.Time.Before(rj.Updated.Time)
		}
		return ri.ID < rj.ID
	})

	return roots
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

// BuildCommentThreads builds structured CommentThread instances for a file,
// computing resolution status, draft reply flags, and latest active comment.
func BuildCommentThreads(path string, fileComments []gerrit.CommentInfo, draftReplies map[string]bool) []*CommentThread {
	roots := BuildCommentForest(fileComments)
	if len(roots) == 0 {
		return nil
	}

	threads := make([]*CommentThread, 0, len(roots))
	for _, root := range roots {
		var inThread []gerrit.CommentInfo
		var collect func(n *CommentNode)
		collect = func(n *CommentNode) {
			inThread = append(inThread, n.Comment)
			for _, ch := range n.Children {
				collect(ch)
			}
		}
		collect(root)

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
		for _, c := range inThread {
			if latest.Updated == nil && c.Updated != nil {
				latest = c
			} else if latest.Updated != nil && c.Updated != nil {
				if c.Updated.Time.After(latest.Updated.Time) {
					latest = c
				}
			} else if c.PatchSet > latest.PatchSet {
				latest = c
			}
		}

		// Thread resolution status:
		// Walk backwards from latest comment to find the most recent explicit Unresolved setting.
		// If none found in thread, default to root's Unresolved if present, else false.
		isUnresolved := false
		foundExplicit := false
		for i := len(inThread) - 1; i >= 0; i-- {
			if inThread[i].Unresolved != nil {
				isUnresolved = *inThread[i].Unresolved
				foundExplicit = true
				break
			}
		}
		if !foundExplicit && root.Comment.Unresolved != nil {
			isUnresolved = *root.Comment.Unresolved
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

// FindLatestCommentAtLine searches a slice of comments on a file for the most recent
// comment located at the specified line number.
func FindLatestCommentAtLine(fileComments []gerrit.CommentInfo, line int) *gerrit.CommentInfo {
	var latest *gerrit.CommentInfo
	for i := range fileComments {
		c := &fileComments[i]
		if c.Line != line {
			continue
		}
		if latest == nil {
			latest = c
			continue
		}
		if latest.Updated == nil && c.Updated != nil {
			latest = c
		} else if latest.Updated != nil && c.Updated != nil {
			if c.Updated.Time.After(latest.Updated.Time) {
				latest = c
			}
		} else if c.PatchSet > latest.PatchSet {
			latest = c
		}
	}
	return latest
}

// FormatCommentForest renders a nested, human-readable terminal tree of comments
// grouped by file in alphabetical order.
func FormatCommentForest(comments map[string][]gerrit.CommentInfo) string {
	if len(comments) == 0 {
		return ""
	}

	var b strings.Builder
	var paths []string
	for path := range comments {
		paths = append(paths, path)
	}
	sort.Strings(paths)

	for _, path := range paths {
		fileComments := comments[path]
		if len(fileComments) == 0 {
			continue
		}

		fmt.Fprintf(&b, "File: %s\n", path)
		roots := BuildCommentForest(fileComments)

		var printNode func(*CommentNode, int)
		printNode = func(n *CommentNode, depth int) {
			author := FormatAccount(n.Comment.Author)
			if author == "" {
				author = "Unknown"
			}
			message := strings.TrimSpace(n.Comment.Message)

			indent := strings.Repeat("  ", depth)
			if depth == 0 {
				line := "-"
				if n.Comment.Line != 0 {
					line = fmt.Sprintf("%d", n.Comment.Line)
				}
				fmt.Fprintf(&b, "  Line %s: %s\n    %s\n", line, author, message)
			} else {
				indentedMsg := strings.ReplaceAll(message, "\n", "\n"+indent+"       ")
				fmt.Fprintf(&b, "%s  -> %s: %s\n", indent, author, indentedMsg)
			}

			for _, child := range n.Children {
				printNode(child, depth+1)
			}
		}

		for _, r := range roots {
			printNode(r, 0)
		}
		fmt.Fprintln(&b)
	}

	return b.String()
}
