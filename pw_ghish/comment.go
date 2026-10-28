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
	"context"
	"fmt"
	"io"
	"maps"
	"os"
	"slices"
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
	commentPublish     bool
	commentDeleteDraft bool
	commentAgent       = agentAuto
)

// resolveCommentMessage validates and resolves --message (-m), --body (-b), and
// --body-file (-F) without mutating flag variables. Passing "-" for bodyFile
// reads from stdin.
func resolveCommentMessage(message, body, bodyFile string, stdin io.Reader, required bool) (string, error) {
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
	if resolved == "-" && bodyFile == "" {
		bodyFile = "-"
		resolved = ""
	}
	if bodyFile != "" {
		var (
			content []byte
			err     error
		)
		if bodyFile == "-" {
			in := stdin
			if in == nil {
				in = os.Stdin
			}
			content, err = io.ReadAll(in)
			if err != nil {
				return "", fmt.Errorf("failed to read body from stdin: %w", err)
			}
		} else {
			content, err = os.ReadFile(bodyFile)
			if err != nil {
				return "", fmt.Errorf("failed to read body file %q: %w", bodyFile, err)
			}
		}
		resolved = string(content)
		if required && strings.TrimSpace(resolved) == "" {
			if bodyFile == "-" {
				return "", fmt.Errorf("body from stdin is empty; comment message cannot be empty")
			}
			return "", fmt.Errorf("body file %q is empty; comment message cannot be empty", bodyFile)
		}
	}
	if required && strings.TrimSpace(resolved) == "" {
		return "", fmt.Errorf("must specify either --message (-m), --body (-b), or --body-file (-F)")
	}
	return resolved, nil
}

var commentCmd = &cobra.Command{
	Use:         "comment [<id>]",
	Annotations: map[string]string{KindAnnotation: KindMutation},
	Short:       "Add, update, or delete a comment on a change",
	Args:        cobra.MaximumNArgs(1),
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
			if commentPublish {
				return fmt.Errorf("cannot specify both --publish and --delete-draft")
			}
			if commentMessage != "" || commentBody != "" || commentBodyFile != "" {
				return fmt.Errorf("cannot specify --message, --body, or --body-file with --delete-draft")
			}
			if commentResolved {
				return fmt.Errorf("cannot specify --resolved with --delete-draft")
			}
		} else {
			if commentDraft && commentPublish {
				return fmt.Errorf("cannot specify both --draft and --publish")
			}
			if commentResolved && commentFile == "" {
				return fmt.Errorf("--resolved requires --path (and --line for inline comments) to identify the comment thread to resolve")
			}
			var err error
			message, err = resolveCommentMessage(commentMessage, commentBody, commentBodyFile, cmd.InOrStdin(), true)
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
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: fmt.Sprintf("Draft comment deleted successfully (%s).",
					formatCommentTargetWithPatchSet(targetPath, commentLine, delPS)),
				Next: fmt.Sprintf("gh pr view %s --comments", changeID),
			})
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

		// Inline and file-level comments (--path) default to staging as an unpublished
		// server-side draft (matching the Gerrit Web UI) unless --publish is passed,
		// preventing multiple 'pr comment --path' calls from publishing a separate
		// Gerrit review and notification email per comment. Change-level comments
		// (without --path) default to publishing immediately unless --draft is passed.
		isDraft := commentDraft || (commentFile != "" && !commentPublish)

		var existingDraft *gerrit.CommentInfo
		if isDraft || (commentResolved && !foundThread) {
			drafts, _, dErr := client.Changes.ListChangeDrafts(ctx, changeID)
			if dErr != nil {
				return chCtx.FormatError(dErr, "listing draft comments for")
			}
			if drafts != nil {
				matches := matchDraftsWithRevisionFallback((*drafts)[targetPath], commentLine, inReplyTo, explicitPatchsetNum, explicitPatchset)
				if len(matches) > 1 && isDraft {
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

		isAI := commentAgent.isAI()

		if isDraft {
			draftInput := &commentInput{
				CommentInput: gerrit.CommentInput{
					Path:       targetPath,
					Line:       commentLine,
					Message:    message,
					InReplyTo:  inReplyTo,
					Unresolved: unresolved,
				},
				IsAI: isAI,
			}
			if commentFile != "" && commentLine != 0 {
				draftInput.Side = "REVISION"
			}

			var (
				savedDraft *gerrit.CommentInfo
				fallbackPS = parentPatchSet
				action     = "saved"
			)
			if existingDraft != nil {
				if existingDraft.ID == "" {
					return fmt.Errorf("internal error: Gerrit returned draft comment without an ID at %s on change %s",
						formatCommentLocation(targetPath, commentLine), changeID)
				}
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
				path, uErr := draftPath(changeID, draftRev, existingDraft.ID)
				if uErr != nil {
					return chCtx.FormatError(uErr, "updating draft comment for")
				}
				savedDraft = new(gerrit.CommentInfo)
				if _, uErr = client.Call(ctx, "PUT", path, draftInput, savedDraft); uErr != nil {
					return chCtx.FormatError(uErr, "updating draft comment for")
				}
				fallbackPS = existingDraft.PatchSet
				action = "updated"
			} else {
				path, cErr := draftsPath(changeID, targetPatchset)
				if cErr != nil {
					return chCtx.FormatError(cErr, "creating draft comment for")
				}
				savedDraft = new(gerrit.CommentInfo)
				if _, cErr = client.Call(ctx, "PUT", path, draftInput, savedDraft); cErr != nil {
					return chCtx.FormatError(cErr, "creating draft comment for")
				}
			}

			var savedPS int
			if savedDraft != nil {
				savedPS = savedDraft.PatchSet
			}
			ps := effectivePatchSet(savedPS, fallbackPS, explicitPatchsetNum)
			PrintReceipt(cmd.OutOrStdout(), Receipt{
				Summary: fmt.Sprintf("Draft comment %s successfully %s.",
					action, formatCommentActionSummary(targetPath, commentLine, ps, draftInput.InReplyTo != "", draftInput.Unresolved)),
				Next: "gh pr review --publish  (or gh pr push --publish)",
			})
			return nil
		}

		comment := commentInput{
			CommentInput: gerrit.CommentInput{
				Line:       commentLine,
				Message:    message,
				InReplyTo:  inReplyTo,
				Unresolved: unresolved,
			},
			IsAI: isAI,
		}
		if commentLine != 0 {
			comment.Side = "REVISION"
		}
		input := &reviewInput{ReviewInput: gerrit.ReviewInput{Drafts: "KEEP"}}
		if commentFile != "" || (isAI != nil && *isAI) {
			// Only comments can carry is_ai, so AI change-level text is posted as
			// a patchset-level comment rather than the review message.
			input.Comments = map[string][]commentInput{targetPath: {comment}}
		} else {
			input.Message = message
		}

		if err := postReview(ctx, client, changeID, targetPatchset, input); err != nil {
			return chCtx.FormatError(err, "posting comment for")
		}

		ps := effectivePatchSet(parentPatchSet, explicitPatchsetNum)
		PrintReceipt(cmd.OutOrStdout(), Receipt{
			Summary: fmt.Sprintf("Comment submitted successfully %s.",
				formatCommentActionSummary(targetPath, commentLine, ps, inReplyTo != "", unresolved)),
			Next: fmt.Sprintf("gh pr view %s --comments", changeID),
		})
		return nil
	},
}

// commentInput is a gerrit.CommentInput plus Gerrit's is_ai field, which
// go-gerrit does not model.
type commentInput struct {
	gerrit.CommentInput
	IsAI *bool `json:"is_ai,omitempty"`
}

// reviewInput is a gerrit.ReviewInput whose comments can carry is_ai. Its
// Comments field shadows the embedded one when encoded as JSON.
type reviewInput struct {
	gerrit.ReviewInput
	Comments map[string][]commentInput `json:"comments,omitempty"`
}

// aiMessageReview returns input with message moved into a patchset-level
// comment marked with isAI, because review messages cannot carry is_ai. It is
// still a single review, so votes and draft handling are unchanged.
func aiMessageReview(input gerrit.ReviewInput, message string, isAI *bool) *reviewInput {
	input.Message = ""
	return &reviewInput{
		ReviewInput: input,
		Comments: map[string][]commentInput{
			"/PATCHSET_LEVEL": {{CommentInput: gerrit.CommentInput{Message: message}, IsAI: isAI}},
		},
	}
}

// postReview posts a review built from reviewInput, which go-gerrit's
// SetReview cannot send.
func postReview(ctx context.Context, client *gerrit.Client, changeID, revision string, input *reviewInput) error {
	path, err := reviewPath(changeID, revision)
	if err != nil {
		return err
	}
	_, err = client.Call(ctx, "POST", path, input, nil)
	return err
}

// The REST paths below are built by hand only because go-gerrit cannot send
// is_ai. Call sites use these builders rather than formatting paths, so every
// segment is escaped by gerritPath. An empty revision means "current".

// draftsPath is the endpoint that creates a draft on a revision (PUT).
func draftsPath(changeID, revision string) (string, error) {
	return gerritPath("changes/%s/revisions/%s/drafts", changeID, cmp.Or(revision, "current"))
}

// draftPath is the endpoint that updates an existing draft (PUT).
func draftPath(changeID, revision, draftID string) (string, error) {
	return gerritPath("changes/%s/revisions/%s/drafts/%s", changeID, cmp.Or(revision, "current"), draftID)
}

// reviewPath is the endpoint that posts a review on a revision (POST).
func reviewPath(changeID, revision string) (string, error) {
	return gerritPath("changes/%s/revisions/%s/review", changeID, cmp.Or(revision, "current"))
}

func effectivePatchSet(patchSets ...int) int {
	for _, ps := range patchSets {
		if ps > 0 {
			return ps
		}
	}
	return 0
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
	for _, p := range slices.Sorted(maps.Keys(drafts)) {
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
	//ghish:flag mode=upstream gh="pr comment --body"
	commentCmd.Flags().StringVarP(&commentMessage, "message", "m", "", "Comment message")
	//ghish:flag mode=upstream gh="pr comment --body"
	commentCmd.Flags().StringVarP(&commentBody, "body", "b", "", "Comment body (alias for --message)")
	//ghish:flag mode=extension reason="Gerrit inline file path targeting"
	commentCmd.Flags().StringVar(&commentFile, "path", "", "File path for inline comment (ghish-only)")
	//ghish:flag mode=extension reason="Gerrit inline line number targeting"
	commentCmd.Flags().IntVarP(&commentLine, "line", "l", 0, "Line number for inline comment (ghish-only)")
	//ghish:flag mode=upstream gh="pr comment --body-file"
	commentCmd.Flags().StringVarP(&commentBodyFile, "body-file", "F", "", "Read comment body from file (use \"-\" to read from standard input)")
	//ghish:flag mode=extension reason="Gerrit comment thread resolution state"
	commentCmd.Flags().BoolVar(&commentResolved, "resolved", false, "Mark the comment thread as resolved (ghish-only)")
	//ghish:flag mode=extension reason="Gerrit patchset revision targeting"
	commentCmd.Flags().StringVar(&commentPatchset, "patchset", "current", "Patchset number or 'current' (ghish-only)")
	//ghish:flag mode=divergent gh="pr create --draft" doc="ghDivergenceHelp"
	commentCmd.Flags().BoolVar(&commentDraft, "draft", false, "Save or update comment as an unpublished draft (default when --path is specified) (ghish-only)")
	//ghish:flag mode=extension reason="Publish Gerrit inline or file comment immediately"
	commentCmd.Flags().BoolVar(&commentPublish, "publish", false, "Publish inline or file comment immediately instead of staging as a draft (ghish-only)")
	//ghish:flag mode=extension reason="Delete unpublished Gerrit server-side draft comment"
	commentCmd.Flags().BoolVar(&commentDeleteDraft, "delete-draft", false, "Delete an unpublished draft comment at the target location (ghish-only)")
	//ghish:flag mode=extension reason="Gerrit is_ai marker for agent-written comments"
	commentCmd.Flags().VarPF(&commentAgent, "agent", "", agentFlagUsage).NoOptDefVal = string(agentTrue)
	PrCmd.AddCommand(commentCmd)
}
