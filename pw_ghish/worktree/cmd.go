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

package worktree

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"text/tabwriter"

	"github.com/spf13/cobra"
	"pigweed.dev/pw_ghish"
)

// NewCommand constructs the `wt` (worktree) Cobra command tree.
func NewCommand(mgr *Manager) *cobra.Command {
	wtCmd := &cobra.Command{
		Use:     "wt",
		Aliases: []string{"worktree"},
		Short:   "Manage warm worktree slots, project symlinks, Bazel caches, and Jetski IDE sync",
		Long: `Manage a bounded pool of warm Git worktree slots (~/wrk/slots/pw-01..N) paired with
logical project symlinks (~/wrk/projects/<name>), shared Bazel disk/repo caches,
and zero-click Jetski IDE left-sidebar project synchronization.`,
	}

	wtCmd.AddCommand(
		newInitCommand(mgr),
		newUseCommand(mgr),
		newParkCommand(mgr),
		newNextCommand(mgr),
		newListCommand(mgr),
		newCloseCommand(mgr),
		newGCCommand(mgr),
	)

	return wtCmd
}

func newInitCommand(mgr *Manager) *cobra.Command {
	var slots int
	var checkOnly bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:   "init",
		Short: "Idempotently examine, configure, and repair worktree slots, hooks, and Bazel caches",
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			items, err := mgr.Init(slots, checkOnly)
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), map[string]any{
					"checklist": items,
					"error":     errString(err),
				})
			}

			out := cmd.OutOrStdout()
			if checkOnly {
				fmt.Fprintln(out, "Inspecting gh-ish Worktree Environment (Read-Only Check)...")
			} else {
				fmt.Fprintln(out, "Inspecting & Converging gh-ish Worktree Environment...")
			}
			fmt.Fprintln(out, "======================================================")
			fmt.Fprintln(out)

			for _, item := range items {
				fmt.Fprintf(out, "[%s] %-24s %s\n", item.Status, item.Category+":", item.Summary)
				if item.Detail != "" {
					fmt.Fprintf(out, "    💡 %s\n", item.Detail)
				}
			}
			fmt.Fprintln(out)
			if err != nil {
				return err
			}
			fmt.Fprintln(out, "Environment is healthy and ready! Use `./gh wt use <project-name>` to start, resume, or swap in a project.")
			return nil
		},
	}

	cmd.Flags().IntVar(&slots, "slots", 0, "Number of physical worktree slots in ~/wrk/slots/ (default: 10, or preserves existing pool size)")
	cmd.Flags().BoolVar(&checkOnly, "check", false, "Perform read-only diagnostic health check without modifying any files")
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output checklist results as structured JSON")
	return cmd
}

func newUseCommand(mgr *Manager) *cobra.Command {
	var branchName string
	var clRef string
	var issueFlag string
	var modeStr string
	var agentID string
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:   "use [<project>]",
		Short: "Allocate or resume a project in a warm slot and sync its symlink/IDE state",
		Args:  cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			if len(args) == 0 && issueFlag == "" {
				return fmt.Errorf("requires either a <project> name argument or --issue <id> flag\nRemediation: Run `./gh wt use <project>` or `./gh wt use --issue <bug_id>`")
			}
			mode := LeaseMode(strings.ToLower(strings.TrimSpace(modeStr)))
			if mode != LeaseModeWrite && mode != LeaseModeRead {
				return fmt.Errorf(
					"invalid lease mode %q: must be 'write' or 'read'\nRemediation: Pass `--mode=write` (default) or `--mode=read`",
					modeStr,
				)
			}
			if agentID == "" {
				agentID = os.Getenv("CONVERSATION_ID")
			}

			var projectName string
			if len(args) > 0 {
				if issueFlag == "" && strings.HasPrefix(args[0], "b/") {
					issueFlag = args[0]
				} else {
					projectName = args[0]
				}
			}

			var res *UseResult
			var err error
			if issueFlag != "" {
				issueID, parseErr := pw_ghish.ParseIssueID(issueFlag)
				if parseErr != nil {
					return fmt.Errorf("invalid issue ID %q: %w", issueFlag, parseErr)
				}
				var issueTitle string
				if client, clientErr := pw_ghish.NewIssueTrackerClientForCommand(cmd.Context(), cmd); clientErr == nil && client != nil {
					if iss, issErr := client.GetIssue(cmd.Context(), issueID); issErr == nil && iss != nil {
						issueTitle = iss.State.Title
					}
				}
				res, err = mgr.UseWithIssue(projectName, branchName, issueID, issueTitle, mode, agentID)
			} else {
				res, err = mgr.Use(projectName, branchName, clRef, mode, agentID)
			}
			if err != nil {
				return err
			}

			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), res)
			}

			out := cmd.OutOrStdout()
			if res.ForkedFrom != "" {
				fmt.Fprintf(out, "⚡ Active writer lease collision on %q; automatically warm-forked to %q (%s)\n", res.ForkedFrom, res.Project, res.Slot)
			} else if res.SwappedOut != "" {
				fmt.Fprintf(out, "🔄 Swapped out idle project %q -> PARKED; mounted %q in warm slot %s\n", res.SwappedOut, res.Project, res.Slot)
			} else {
				fmt.Fprintf(out, "✓ Project %q mounted in slot %s (branch: %s)\n", res.Project, res.Slot, res.Branch)
			}
			if res.IssueID > 0 {
				fmt.Fprintf(out, "  Issue:     b/%d\n", res.IssueID)
			}
			fmt.Fprintf(out, "  Directory: %s\n", res.SymlinkPath)
			return nil
		},
	}

	cmd.Flags().StringVarP(&branchName, "branch", "b", "", "Git branch name (defaults to project name)")
	cmd.Flags().StringVar(&clRef, "cl", "", "Optional Gerrit CL number or URL to associate/checkout")
	cmd.Flags().StringVar(&issueFlag, "issue", "", "Optional Buganizer issue ID or URL to link and auto-name branch")
	cmd.Flags().StringVar(&modeStr, "mode", "write", "Lease mode: 'write' or 'read'")
	cmd.Flags().StringVar(&agentID, "agent", "", "Agent or Conversation ID acquiring the lease (defaults to $CONVERSATION_ID)")
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON")
	return cmd
}

func newParkCommand(mgr *Manager) *cobra.Command {
	var force bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:   "park <project>",
		Short: "Unmount a project from its physical slot (MOUNTED -> PARKED), freeing the slot",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			projName := args[0]
			if err := mgr.Park(projName, force); err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), map[string]any{
					"project":   projName,
					"residency": ResidencyParked,
				})
			}
			fmt.Fprintf(cmd.OutOrStdout(), "💤 Parked project %q (slot freed; branch and Gerrit CL remain tracked in `./gh wt list`)\n", projName)
			return nil
		},
	}

	cmd.Flags().BoolVarP(&force, "force", "f", false, "Force parking even if working tree has uncommitted changes")
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON")
	return cmd
}

func newNextCommand(mgr *Manager) *cobra.Command {
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:   "next [<project>]",
		Short: "Rebase a mounted project onto origin/main in-place to start its next CL",
		Args:  cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			var projName string
			if len(args) > 0 {
				projName = args[0]
			} else {
				// Infer from current working directory if inside ~/wrk/projects/<name> or ~/wrk/slots/pw-XX
				cwd, _ := os.Getwd()
				projName = filepath.Base(cwd)
			}
			if err := mgr.Next(projName); err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), map[string]any{
					"project": projName,
					"status":  "REBASED_ORIGIN_MAIN",
				})
			}
			fmt.Fprintf(cmd.OutOrStdout(), "✨ Rebased project %q onto origin/main in-place! Ready for next CL.\n", projName)
			return nil
		},
	}

	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON")
	return cmd
}

func newListCommand(mgr *Manager) *cobra.Command {
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:     "list",
		Aliases: []string{"status", "ls"},
		Short:   "Display live dashboard of mounted and parked projects with Gerrit review/CI statuses",
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			report, err := mgr.List(cmd.Context())
			if err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), report)
			}

			out := cmd.OutOrStdout()
			fmt.Fprintf(out, "MOUNTED PROJECTS (%d/%d Slots Occupied, %d Available)\n",
				report.OccupiedSlots, report.TotalSlots, report.AvailableSlots)
			fmt.Fprintln(out, "=====================================================")

			if len(report.MountedProjects) == 0 {
				fmt.Fprintln(out, "  (No projects currently mounted in slots. Use `./gh wt use <project>` to mount.)")
			} else {
				tw := tabwriter.NewWriter(out, 0, 2, 2, ' ', 0)
				fmt.Fprintln(tw, "PROJECT\tSLOT\tSTATUS\tGERRIT CL & DETAILS\tRECOMMENDED ACTION")
				for _, p := range report.MountedProjects {
					fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\n",
						p.Project, p.Slot, p.StatusBadge, p.Details, p.RecommendedAction)
				}
				tw.Flush()
			}
			fmt.Fprintln(out)

			fmt.Fprintf(out, "PARKED PROJECTS (%d Shelved in Git/Gerrit — 0 Slots Used)\n", len(report.ParkedProjects))
			fmt.Fprintln(out, "=========================================================")
			if len(report.ParkedProjects) == 0 {
				fmt.Fprintln(out, "  (No parked projects.)")
			} else {
				tw := tabwriter.NewWriter(out, 0, 2, 2, ' ', 0)
				fmt.Fprintln(tw, "PROJECT\tSLOT\tSTATUS\tGERRIT CL & DETAILS\tRECOMMENDED ACTION")
				for _, p := range report.ParkedProjects {
					fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\n",
						p.Project, p.Slot, p.StatusBadge, p.Details, p.RecommendedAction)
				}
				tw.Flush()
			}
			return nil
		},
	}

	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output dashboard as structured JSON")
	return cmd
}

func newCloseCommand(mgr *Manager) *cobra.Command {
	var force bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:   "close <project>",
		Short: "Permanently close a completed workstream, freeing its slot and removing its active symlink",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			projName := args[0]
			var closedIssueID int64
			if st, loadErr := mgr.Store.Load(); loadErr == nil && st != nil {
				if proj, ok := st.Projects[projName]; ok {
					closedIssueID = proj.IssueID
					if closedIssueID == 0 {
						if id, ok := pw_ghish.ExtractIssueIDFromBranchName(proj.Branch); ok {
							closedIssueID = id
						}
					}
				}
			}

			if err := mgr.Close(projName, force); err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), map[string]any{
					"project":  projName,
					"status":   "CLOSED",
					"issue_id": closedIssueID,
				})
			}
			fmt.Fprintf(cmd.OutOrStdout(), "✓ Closed project %q (slot returned to available pool; Jetski conversations archived)\n", projName)
			if closedIssueID > 0 {
				fmt.Fprintf(cmd.OutOrStdout(), "  💡 Issue b/%d is associated with this project. Run './gh issue close %d' if resolved.\n", closedIssueID, closedIssueID)
			}
			return nil
		},
	}

	cmd.Flags().BoolVarP(&force, "force", "f", false, "Force closing even if uncommitted working tree changes exist")
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON")
	return cmd
}

func newGCCommand(mgr *Manager) *cobra.Command {
	var dryRun bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:   "gc",
		Short: "Sweep ~/.cache/bazel/_bazel_$USER/ for orphaned output bases from deleted worktrees",
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			report, err := mgr.GarbageCollect(dryRun)
			if err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), report)
			}

			out := cmd.OutOrStdout()
			if len(report.OrphansFound) == 0 {
				fmt.Fprintln(out, "✓ No orphaned Bazel output bases found.")
				return nil
			}
			if dryRun {
				fmt.Fprintf(out, "Found %d orphaned Bazel output base(s) (Dry Run):\n", len(report.OrphansFound))
			} else {
				fmt.Fprintf(out, "Cleaned up %d/%d orphaned Bazel output base(s):\n", report.RemovedCount, len(report.OrphansFound))
			}
			for _, o := range report.OrphansFound {
				fmt.Fprintf(out, "  - %s (workspace: %s, reason: %s)\n", o.OutputBaseDir, o.WorkspacePath, o.Reason)
			}
			return nil
		},
	}

	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "List orphaned output bases without deleting them")
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output report as structured JSON")
	return cmd
}

func writeJSON(w io.Writer, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Errorf("failed to serialize JSON output: %w", err)
	}
	fmt.Fprintln(w, string(data))
	return nil
}

func errString(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}
