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
	"os"
	"sort"
	"strconv"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	commentMessage     string
	commentBody        string
	commentFile        string
	commentLine        int
	commentBodyFile    string
	commentResolved    bool
	commentPatchset    string
	commentDraft       bool
	commentDeleteDraft bool
)

// resolveCommentMessage validates and resolves --message (-m), --body (-b), and
// --body-file (-F) without mutating flag variables.
func resolveCommentMessage(message, body, bodyFile string, required bool) (string, error) {
	if message != "" && body != "" {
		return "", fmt.Errorf("cannot specify both --message (-m) and --body (-b)")
	}
	resolved := message
	if body != "" {
		resolved = body
	}
	if resolved != "" && bodyFile != "" {
		return "", fmt.Errorf("cannot specify both --message/--body and --body-file (-F)")
	}
	if bodyFile != "" {
		content, err := os.ReadFile(bodyFile)
		if err != nil {
			return "", fmt.Errorf("failed to read body file %q: %w", bodyFile, err)
		}
		resolved = string(content)
		if required && strings.TrimSpace(resolved) == "" {
			return "", fmt.Errorf("body file %q is empty; comment message cannot be empty", bodyFile)
		}
	}
	if required && strings.TrimSpace(resolved) == "" {
		return "", fmt.Errorf("must specify either --message (-m), --body (-b), or --body-file (-F)")
	}
	return resolved, nil
}

var commentCmd = &cobra.Command{
	Use:   "comment [<id>]",
	Short: "Add, update, or delete a comment on a change",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if commentLine < 0 {
			return fmt.Errorf("--line must be a positive line number (got %d)", commentLine)
		}
		if commentFile == "" && (commentLine != 0 || cmd.Flags().Changed("line")) {
			return fmt.Errorf("--line requires --path to specify the target file")
		}

		var message string
		if commentDeleteDraft {
			if commentDraft {
				return fmt.Errorf("cannot specify both --draft and --delete-draft")
			}
			if commentMessage != "" || commentBody != "" || commentBodyFile != "" {
				return fmt.Errorf("cannot specify --message, --body, or --body-file with --delete-draft")
			}
			if commentResolved {
				return fmt.Errorf("cannot specify --resolved with --delete-draft")
			}
		} else {
			if commentResolved && commentFile == "" {
				return fmt.Errorf("--resolved requires --path (and --line for inline comments) to identify the comment thread to resolve")
			}
			var err error
			message, err = resolveCommentMessage(commentMessage, commentBody, commentBodyFile, true)
			if err != nil {
				return err
			}
		}

		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}
		ctx := chCtx.Context
		changeID := chCtx.ChangeID
		client := chCtx.Client

		explicitPatchset := cmd.Flags().Changed("patchset")
		targetPatchset := commentPatchset
		if !explicitPatchset && chCtx.Revision != "" && chCtx.Revision != "current" {
			targetPatchset = chCtx.Revision
		}
		explicitPatchsetNum := 0
		if explicitPatchset || (chCtx.Revision != "" && chCtx.Revision != "current") {
			if n, pErr := strconv.Atoi(targetPatchset); pErr == nil && n > 0 {
				explicitPatchsetNum = n
			}
		}

		targetPath := "/PATCHSET_LEVEL"
		if commentFile != "" {
			targetPath = commentFile
		}

		if commentDeleteDraft {
			drafts, _, dErr := client.Changes.ListChangeDrafts(ctx, changeID)
			if dErr != nil {
				return chCtx.FormatError(dErr, "listing draft comments for")
			}
			var draftMap map[string][]gerrit.CommentInfo
			if drafts != nil {
				draftMap = *drafts
			}
			matches := matchDraftsWithRevisionFallback(draftMap[targetPath], commentLine, "", explicitPatchsetNum, explicitPatchset)
			if len(matches) == 0 {
				filterPS := 0
				if explicitPatchset {
					filterPS = explicitPatchsetNum
				}
				return formatMissingDraftToDeleteError(changeID, targetPath, commentLine, filterPS, draftMap)
			}
			if len(matches) > 1 {
				return formatAmbiguousDraftError(changeID, targetPath, commentLine, matches, true)
			}
			existing := matches[0]
			if existing.ID == "" {
				return fmt.Errorf("internal error: Gerrit returned draft comment without an ID at %s on change %s",
					formatCommentLocation(targetPath, commentLine), changeID)
			}
			draftRev := draftRevisionForAPI(existing.PatchSet, targetPatchset)
			if _, dErr := client.Changes.DeleteDraft(ctx, changeID, draftRev, existing.ID); dErr != nil {
				return chCtx.FormatError(dErr, "deleting draft comment for")
			}
			delPS := effectivePatchSet(existing.PatchSet, explicitPatchsetNum)
			fmt.Fprintf(cmd.OutOrStdout(), "Draft comment deleted successfully (%s).\n",
				formatCommentTargetWithPatchSet(targetPath, commentLine, delPS))
			return nil
		}

		var (
			inReplyTo      string
			parentPatchSet int
			foundThread    bool
		)
		if commentFile != "" {
			comments, _, err := client.Changes.ListChangeComments(ctx, changeID)
			if err != nil {
				if commentResolved {
					return fmt.Errorf("failed to list change comments to resolve thread: %w", chCtx.FormatError(err, "listing comments for"))
				}
				return chCtx.FormatError(err, "listing comments for")
			}
			if comments != nil {
				if fileComments, ok := (*comments)[commentFile]; ok {
					latestComment := FindLatestCommentAtLineForPatchSet(fileComments, commentLine, explicitPatchsetNum)
					if latestComment == nil && !explicitPatchset && explicitPatchsetNum > 0 {
						// When the CL was referenced with a patchset suffix (e.g. pwrev/123/23)
						// and no comment exists on PS23 itself, fall back to threads carried
						// forward from earlier patchsets.
						latestComment = FindLatestCommentAtLine(fileComments, commentLine)
					}
					if latestComment != nil {
						foundThread = true
						inReplyTo = latestComment.ID
						parentPatchSet = latestComment.PatchSet
						// Gerrit requires replies (in_reply_to) to be posted to the revision
						// that owns the parent comment.
						if !explicitPatchset && latestComment.PatchSet != 0 {
							targetPatchset = strconv.Itoa(latestComment.PatchSet)
						}
					}
				}
			}
		}

		var existingDraft *gerrit.CommentInfo
		if commentDraft || (commentResolved && !foundThread) {
			drafts, _, dErr := client.Changes.ListChangeDrafts(ctx, changeID)
			if dErr != nil {
				return chCtx.FormatError(dErr, "listing draft comments for")
			}
			if drafts != nil {
				matches := matchDraftsWithRevisionFallback((*drafts)[targetPath], commentLine, inReplyTo, explicitPatchsetNum, explicitPatchset)
				if len(matches) > 1 && commentDraft {
					return formatAmbiguousDraftError(changeID, targetPath, commentLine, matches, false)
				}
				if len(matches) > 0 {
					latest := &matches[0]
					for i := 1; i < len(matches); i++ {
						if isNewerComment(&matches[i], latest) {
							latest = &matches[i]
						}
					}
					existingDraft = latest
					foundThread = true
				}
			}
		}

		if commentResolved && !foundThread {
			fmt.Fprintf(cmd.ErrOrStderr(), "Warning: no existing comment thread found on %s to resolve.\nRun 'gh pr view --comments' to inspect existing comment threads and file paths.\n",
				formatShortCommentTarget(commentFile, commentLine))
		}

		var unresolved *bool
		if commentResolved {
			resolvedFalse := false
			unresolved = &resolvedFalse
		} else if commentFile != "" && inReplyTo == "" && existingDraft == nil {
			// Gerrit's REST API defaults root comments with nil Unresolved to false
			// (already resolved), whereas new inline/file comments should open an
			// unresolved thread just like the Gerrit Web UI.
			unresolvedTrue := true
			unresolved = &unresolvedTrue
		}

		if commentDraft {
			draftInput := &gerrit.CommentInput{
				Path:       targetPath,
				Line:       commentLine,
				Message:    message,
				InReplyTo:  inReplyTo,
				Unresolved: unresolved,
			}
			if commentFile != "" && commentLine != 0 {
				draftInput.Side = "REVISION"
			}
			if existingDraft != nil && existingDraft.ID != "" {
				draftRev := draftRevisionForAPI(existingDraft.PatchSet, targetPatchset)
				if draftInput.InReplyTo == "" && existingDraft.InReplyTo != "" {
					draftInput.InReplyTo = existingDraft.InReplyTo
				}
				if draftInput.Unresolved == nil && existingDraft.Unresolved != nil {
					draftInput.Unresolved = existingDraft.Unresolved
				}
				if existingDraft.Range != nil {
					draftInput.Range = existingDraft.Range
				}
				if existingDraft.Side != "" {
					draftInput.Side = existingDraft.Side
				}
				updatedDraft, _, uErr := client.Changes.UpdateDraft(ctx, changeID, draftRev, existingDraft.ID, draftInput)
				if uErr != nil {
					return chCtx.FormatError(uErr, "updating draft comment for")
				}
				ps := effectivePatchSet(existingDraft.PatchSet, explicitPatchsetNum)
				if updatedDraft != nil && updatedDraft.PatchSet > 0 {
					ps = updatedDraft.PatchSet
				}
				fmt.Fprintf(cmd.OutOrStdout(), "Draft comment updated successfully %s.\n",
					formatCommentActionSummary(targetPath, commentLine, ps, draftInput.InReplyTo != "", draftInput.Unresolved))
				return nil
			}
			createdDraft, _, err := client.Changes.CreateDraft(ctx, changeID, targetPatchset, draftInput)
			if err != nil {
				return chCtx.FormatError(err, "creating draft comment for")
			}
			ps := effectivePatchSet(parentPatchSet, explicitPatchsetNum)
			if createdDraft != nil && createdDraft.PatchSet > 0 {
				ps = createdDraft.PatchSet
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Draft comment saved successfully %s.\n",
				formatCommentActionSummary(targetPath, commentLine, ps, draftInput.InReplyTo != "", draftInput.Unresolved))
			return nil
		}

		input := &gerrit.ReviewInput{
			Drafts: "KEEP",
		}

		if commentFile != "" {
			input.Comments = make(map[string][]gerrit.CommentInput)
			comment := gerrit.CommentInput{
				Line:       commentLine,
				Message:    message,
				InReplyTo:  inReplyTo,
				Unresolved: unresolved,
			}
			if commentLine != 0 {
				comment.Side = "REVISION"
			}
			input.Comments[commentFile] = []gerrit.CommentInput{comment}
		} else {
			input.Message = message
		}

		if err := chCtx.SetReviewRevision(targetPatchset, input); err != nil {
			return fmt.Errorf("failed to set review/comment for change %s: %w", changeID, err)
		}

		ps := effectivePatchSet(parentPatchSet, explicitPatchsetNum)
		fmt.Fprintf(cmd.OutOrStdout(), "Comment submitted successfully %s.\n",
			formatCommentActionSummary(targetPath, commentLine, ps, inReplyTo != "", unresolved))
		return nil
	},
}

func effectivePatchSet(primaryPS, fallbackPS int) int {
	if primaryPS > 0 {
		return primaryPS
	}
	return fallbackPS
}

func formatCommentActionSummary(targetPath string, line int, patchSet int, isReply bool, unresolved *bool) string {
	loc := formatCommentTargetWithPatchSet(targetPath, line, patchSet)
	var tags []string
	if !isPatchsetLevelPath(targetPath) {
		if isReply {
			tags = append(tags, "reply")
		} else {
			tags = append(tags, "new thread")
		}
	} else if isReply {
		tags = append(tags, "reply")
	}
	if unresolved != nil {
		if *unresolved {
			tags = append(tags, "unresolved")
		} else {
			tags = append(tags, "resolved")
		}
	}
	if len(tags) > 0 {
		return fmt.Sprintf("on %s (%s)", loc, strings.Join(tags, ", "))
	}
	return fmt.Sprintf("on %s", loc)
}

// matchDraftsWithRevisionFallback matches drafts at (line, inReplyTo).
// When patchSet > 0:
//   - If a draft exists on patchSet, it narrows to that patchset (allowing
//     both `--patchset 23` and `pwrev/NNNN/23` to disambiguate multiple drafts).
//   - If no draft exists on patchSet and `--patchset` was NOT explicitly passed
//     (i.e. the patchset came from a positional URL/shortlink like `pwrev/123/23`),
//     it falls back to drafts across earlier patchsets so drafts carried forward
//     from earlier patchsets are still found and updated/deleted cleanly.
func matchDraftsWithRevisionFallback(fileDrafts []gerrit.CommentInfo, line int, inReplyTo string, patchSet int, explicitPatchsetFlag bool) []gerrit.CommentInfo {
	if patchSet > 0 {
		psMatches := FindMatchingDraftsAtTarget(fileDrafts, line, inReplyTo, patchSet)
		if len(psMatches) > 0 || explicitPatchsetFlag {
			return psMatches
		}
	}
	return FindMatchingDraftsAtTarget(fileDrafts, line, inReplyTo, 0)
}

// draftRevisionForAPI returns the revision string to use for UpdateDraft / DeleteDraft.
// In Gerrit, a draft comment exists under the specific revision it was created on.
func draftRevisionForAPI(draftPatchSet int, fallbackRev string) string {
	if draftPatchSet > 0 {
		return strconv.Itoa(draftPatchSet)
	}
	if fallbackRev != "" {
		return fallbackRev
	}
	return "current"
}

func formatCommentLocation(path string, line int) string {
	if isPatchsetLevelPath(path) {
		return "change level (/PATCHSET_LEVEL)"
	}
	return formatShortCommentTarget(path, line)
}

func formatMissingDraftToDeleteError(changeID, path string, line, patchSet int, drafts map[string][]gerrit.CommentInfo) error {
	loc := formatCommentLocation(path, line)
	if patchSet > 0 {
		loc = fmt.Sprintf("%s (patchset %d)", loc, patchSet)
	}

	var available []string
	var paths []string
	for p := range drafts {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	for _, p := range paths {
		for _, d := range drafts[p] {
			psFlag := ""
			if d.PatchSet > 0 {
				psFlag = fmt.Sprintf(" --patchset %d", d.PatchSet)
			}
			targetDesc := formatCommentTargetWithPatchSet(p, d.Line, d.PatchSet)
			if isPatchsetLevelPath(p) {
				available = append(available, fmt.Sprintf("  • %s (--delete-draft%s)", targetDesc, psFlag))
			} else if d.Line > 0 {
				available = append(available, fmt.Sprintf("  • %s (--path %s --line %d --delete-draft%s)", targetDesc, p, d.Line, psFlag))
			} else {
				available = append(available, fmt.Sprintf("  • %s (--path %s --delete-draft%s)", targetDesc, p, psFlag))
			}
		}
	}

	if len(available) == 0 {
		return fmt.Errorf("no unpublished draft comment found at %s on change %s (change has no unpublished drafts).\nRun 'gh pr view %s --comments' to inspect comments",
			loc, changeID, changeID)
	}

	return fmt.Errorf("no unpublished draft comment found at %s on change %s.\n\nExisting unpublished drafts on change %s:\n%s\n\nRun 'gh pr view %s --comments' to inspect all threads and drafts",
		loc, changeID, changeID, strings.Join(available, "\n"), changeID)
}

func formatAmbiguousDraftError(changeID, path string, line int, matches []gerrit.CommentInfo, isDelete bool) error {
	loc := formatCommentLocation(path, line)
	action := "update"
	flagHint := "--draft"
	if isDelete {
		action = "delete"
		flagHint = "--delete-draft"
	}

	var items []string
	for _, d := range matches {
		ps := "current"
		if d.PatchSet > 0 {
			ps = fmt.Sprintf("PS%d (--patchset %d or %s/%d)", d.PatchSet, d.PatchSet, changeID, d.PatchSet)
		}
		snippet := formatCommentSnippet(d.Message, 60)
		items = append(items, fmt.Sprintf("  • [%s] %q", ps, snippet))
	}

	return fmt.Errorf("multiple (%d) unpublished draft comments found at %s on change %s; refusing to guess which draft to %s.\n\nMatching drafts:\n%s\n\nTo disambiguate, specify --patchset <N> (or %s/<N>) with %s, or run 'gh pr view %s --comments' to inspect all drafts",
		len(matches), loc, changeID, action, strings.Join(items, "\n"), changeID, flagHint, changeID)
}

func init() {
	commentCmd.Flags().StringVarP(&commentMessage, "message", "m", "", "Comment message")
	commentCmd.Flags().StringVarP(&commentBody, "body", "b", "", "Comment body (alias for --message)")
	commentCmd.Flags().StringVar(&commentFile, "path", "", "File path for inline comment (ghish-only)")
	commentCmd.Flags().IntVarP(&commentLine, "line", "l", 0, "Line number for inline comment (ghish-only)")
	commentCmd.Flags().StringVarP(&commentBodyFile, "body-file", "F", "", "File containing comment body")
	commentCmd.Flags().BoolVar(&commentResolved, "resolved", false, "Mark the comment thread as resolved (ghish-only)")
	commentCmd.Flags().StringVar(&commentPatchset, "patchset", "current", "Patchset number or 'current' (ghish-only)")
	commentCmd.Flags().BoolVar(&commentDraft, "draft", false, "Save or update comment as an unpublished draft (ghish-only)")
	commentCmd.Flags().BoolVar(&commentDeleteDraft, "delete-draft", false, "Delete an unpublished draft comment at the target location (ghish-only)")
	PrCmd.AddCommand(commentCmd)
}
