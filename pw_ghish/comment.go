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

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

var (
	commentMessage  string
	commentFile     string
	commentLine     int
	commentBodyFile string
	commentResolved bool
	commentPatchset string
	commentDraft    bool
)

var commentCmd = &cobra.Command{
	Use:   "comment [<id>]",
	Short: "Add a comment to a change",
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		chCtx, err := ResolveChangeContext(cmd, args)
		if err != nil {
			return err
		}
		ctx := chCtx.Context
		changeID := chCtx.ChangeID
		client := chCtx.Client

		if !cmd.Flags().Changed("patchset") && chCtx.Revision != "current" {
			commentPatchset = chCtx.Revision
		}

		if commentMessage != "" && commentBodyFile != "" {
			return fmt.Errorf("cannot specify both --message and --body-file")
		}
		if commentMessage == "" && commentBodyFile == "" {
			return fmt.Errorf("must specify either --message or --body-file")
		}

		if commentResolved && (commentFile == "" || commentLine == 0) {
			return fmt.Errorf("--resolved requires both --path and --line to identify the comment thread to resolve")
		}

		if commentBodyFile != "" {
			content, err := os.ReadFile(commentBodyFile)
			if err != nil {
				return fmt.Errorf("failed to read body file: %w", err)
			}
			commentMessage = string(content)
		}

		var inReplyTo string
		if commentFile != "" && commentLine != 0 {
			comments, _, err := client.Changes.ListChangeComments(ctx, changeID)
			if err != nil {
				if commentResolved {
					return fmt.Errorf("failed to list change comments to resolve thread: %w", err)
				}
				fmt.Fprintf(cmd.ErrOrStderr(), "Warning: failed to list change comments: %v\n", err)
			} else if comments != nil {
				var found bool
				if fileComments, ok := (*comments)[commentFile]; ok {
					if latestComment := FindLatestCommentAtLine(fileComments, commentLine); latestComment != nil {
						found = true
						inReplyTo = latestComment.ID
						if !cmd.Flags().Changed("patchset") && chCtx.Revision == "current" && latestComment.PatchSet != 0 {
							commentPatchset = fmt.Sprintf("%d", latestComment.PatchSet)
						}
					}
				}
				if commentResolved && !found {
					fmt.Fprintf(cmd.ErrOrStderr(), "Warning: no existing comment thread found on %s:%d to resolve.\nRun 'gh pr view --comments' to inspect existing comment threads and file paths.\n", commentFile, commentLine)
				}
			}
		}

		var unresolved *bool
		if commentResolved {
			unresolved = new(bool) // Default value is false.
		}

		if commentDraft {
			path := "/PATCHSET_LEVEL"
			if commentFile != "" {
				path = commentFile
			}
			draftInput := &gerrit.CommentInput{
				Path:       path,
				Line:       commentLine,
				Message:    commentMessage,
				InReplyTo:  inReplyTo,
				Unresolved: unresolved,
			}
			if commentFile != "" && commentLine != 0 {
				draftInput.Side = "REVISION"
			}
			_, _, err = client.Changes.CreateDraft(ctx, changeID, commentPatchset, draftInput)
			if err != nil {
				return fmt.Errorf("failed to create draft comment for change %s: %w", changeID, err)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "Draft comment saved successfully.")
			return nil
		}

		input := &gerrit.ReviewInput{
			Drafts: "PUBLISH",
		}

		if commentFile != "" && commentLine != 0 {
			input.Comments = make(map[string][]gerrit.CommentInput)
			comment := gerrit.CommentInput{
				Line:       commentLine,
				Message:    commentMessage,
				InReplyTo:  inReplyTo,
				Side:       "REVISION",
				Unresolved: unresolved,
			}
			input.Comments[commentFile] = []gerrit.CommentInput{comment}
		} else {
			input.Message = commentMessage
		}

		if err := chCtx.SetReviewRevision(commentPatchset, input); err != nil {
			return fmt.Errorf("failed to set review/comment for change %s: %w", changeID, err)
		}

		fmt.Fprintln(cmd.OutOrStdout(), "Comment submitted successfully.")
		return nil
	},
}

func init() {
	commentCmd.Flags().StringVarP(&commentMessage, "message", "m", "", "Comment message")
	commentCmd.Flags().StringVar(&commentFile, "path", "", "File path for inline comment")
	commentCmd.Flags().IntVarP(&commentLine, "line", "l", 0, "Line number for inline comment")
	commentCmd.Flags().StringVarP(&commentBodyFile, "body-file", "F", "", "File containing comment body")
	commentCmd.Flags().BoolVar(&commentResolved, "resolved", false, "Mark the comment thread as resolved")
	commentCmd.Flags().StringVar(&commentPatchset, "patchset", "current", "Patchset number or 'current'")
	commentCmd.Flags().BoolVar(&commentDraft, "draft", false, "Save comment as draft without posting")
	PrCmd.AddCommand(commentCmd)
}
