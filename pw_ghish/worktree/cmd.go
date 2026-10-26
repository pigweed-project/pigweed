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
	"strings"
	"text/tabwriter"

	"github.com/spf13/cobra"
	"pigweed.dev/pw_ghish"
)

// NewCommand constructs the `wt` (worktree) Cobra command tree.
func NewCommand(mgr *Manager) *cobra.Command {
	wtCmd := &cobra.Command{
		Use:           "wt",
		Aliases:       []string{"worktree"},
		SilenceUsage:  true,
		SilenceErrors: true,
		Short:         "Manage warm worktree slots, project symlinks, Bazel caches, and Jetski IDE sync",
		Long: `Manage a bounded pool of warm Git worktree slots (~/wrk/slots/pw-01..N) paired with
logical project symlinks (~/wrk/projects/<name>), shared Bazel disk/repo caches,
and automatic Jetski IDE left-sidebar project synchronization.`,
	}

	wtCmd.AddCommand(
		newInitCommand(mgr),
		newAuditCommand(mgr),
		newUseCommand(mgr),
		newParkCommand(mgr),
		newNextCommand(mgr),
		newListCommand(mgr),
		newCloseCommand(mgr),
		newGCCommand(mgr),
		pw_ghish.NewSenseCommand(),
	)

	return wtCmd
}

// checklistCounts tallies the warning, error, and repaired items in a wt
// init/audit checklist.
type checklistCounts struct {
	warnings, errors, repaired int
}

// countChecklistErrors returns the number of ChecklistError items.
func countChecklistErrors(items []ChecklistItem) int {
	n := 0
	for _, item := range items {
		if item.Status == ChecklistError {
			n++
		}
	}
	return n
}

// printChecklist renders checklist items as aligned status rows and returns
// their tallies.
func printChecklist(out io.Writer, items []ChecklistItem) checklistCounts {
	var counts checklistCounts
	for _, item := range items {
		switch item.Status {
		case ChecklistWarning:
			counts.warnings++
		case ChecklistError:
			counts.errors++
		case ChecklistRepaired:
			counts.repaired++
		}
		fmt.Fprintf(out, "  %s %-24s %s\n", item.Status.Glyph(), item.Category+":", item.Summary)
		if item.Detail != "" {
			detail := item.Detail
			if len(detail) > 100 {
				detail = detail[:97] + "..."
			}
			fmt.Fprintf(out, "      Note: %s\n", detail)
		}
	}
	return counts
}

// runAudit performs the read-only worktree health check shared by `wt audit`
// and `wt init --check`, ending with a Verdict:/Next: footer.
func runAudit(cmd *cobra.Command, mgr *Manager, slots int, jsonOutput bool) error {
	if mgr == nil {
		return fmt.Errorf("internal error: worktree manager is uninitialized")
	}
	items, err := mgr.Init(slots, true)
	if jsonOutput {
		// The checklist error is reported in the payload's "error" field.
		if jsonErr := writeJSON(cmd.OutOrStdout(), map[string]any{
			"checklist": items,
			"error":     errString(err),
		}); jsonErr != nil {
			return jsonErr
		}
		if err != nil {
			return err
		}
		if n := countChecklistErrors(items); n > 0 {
			return fmt.Errorf("worktree environment has %d error(s); run 'gh wt init' to repair", n)
		}
		return nil
	}

	out := cmd.OutOrStdout()
	fmt.Fprintln(out, "Inspecting gh-ish Worktree Environment (Read-Only Check)...")
	fmt.Fprintln(out)
	counts := printChecklist(out, items)
	fmt.Fprintln(out)
	switch {
	case counts.errors > 0:
		fmt.Fprintf(out, "Verdict: unhealthy (%d errors, %d warnings)\n", counts.errors, counts.warnings)
		fmt.Fprintln(out, "Next:    gh wt init")
	case counts.warnings > 0:
		fmt.Fprintf(out, "Verdict: healthy with warnings (%d warnings)\n", counts.warnings)
		fmt.Fprintln(out, "Next:    gh wt gc  (or gh wt use <project>)")
	default:
		fmt.Fprintln(out, "Verdict: healthy (all checks passed)")
		fmt.Fprintln(out, "Next:    gh wt use <project>")
	}
	if err != nil {
		return err
	}
	if counts.errors > 0 {
		return fmt.Errorf("worktree environment has %d error(s); run 'gh wt init' to repair", counts.errors)
	}
	return nil
}

func newAuditCommand(mgr *Manager) *cobra.Command {
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:         "audit",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindView},
		Short:       "Read-only health check of worktree slots, hooks, and Bazel caches (same as wt init --check)",
		Args:        cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runAudit(cmd, mgr, 0, jsonOutput)
		},
	}

	//ghish:flag mode=extension reason="Structured JSON checklist for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output checklist results as structured JSON (ghish-only)")
	return cmd
}

func newInitCommand(mgr *Manager) *cobra.Command {
	var slots int
	var checkOnly bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:         "init",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindMutation},
		Short:       "Idempotently examine, configure, and repair worktree slots, hooks, and Bazel caches",
		Args:        cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			if checkOnly {
				return runAudit(cmd, mgr, slots, jsonOutput)
			}
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			items, err := mgr.Init(slots, false)
			if jsonOutput {
				if jsonErr := writeJSON(cmd.OutOrStdout(), map[string]any{
					"checklist": items,
					"error":     errString(err),
				}); jsonErr != nil {
					return jsonErr
				}
				if err != nil {
					return err
				}
				// Match text mode: unresolved checklist errors exit non-zero.
				if n := countChecklistErrors(items); n > 0 {
					return fmt.Errorf("worktree environment has %d unresolved error(s); see the checklist JSON", n)
				}
				return nil
			}

			out := cmd.OutOrStdout()
			fmt.Fprintln(out, "Inspecting & Converging gh-ish Worktree Environment...")
			fmt.Fprintln(out)
			counts := printChecklist(out, items)
			fmt.Fprintln(out)
			if err != nil {
				return err
			}
			if counts.errors > 0 {
				fmt.Fprintf(out, "Verdict: unhealthy (%d errors, %d warnings)\n", counts.errors, counts.warnings)
				fmt.Fprintln(out, "Next:    gh wt audit")
				return fmt.Errorf("worktree environment has %d unresolved error(s); see the checklist above", counts.errors)
			}
			receipt := pw_ghish.Receipt{
				Summary: fmt.Sprintf("Converged worktree environment (%d repaired)", counts.repaired),
				Next:    "gh wt use <project>",
			}
			if counts.warnings > 0 {
				receipt.Details = append(receipt.Details, pw_ghish.ReceiptField{
					Key:   "Warnings",
					Value: fmt.Sprintf("%d (gh wt audit to re-check, gh wt gc to clean up)", counts.warnings),
				})
			}
			pw_ghish.PrintReceipt(out, receipt)
			return nil
		},
	}

	//ghish:flag mode=extension reason="Size of the warm worktree slot pool"
	cmd.Flags().IntVar(&slots, "slots", 0, "Number of physical worktree slots in ~/wrk/slots/ (default: 10, or preserves existing pool size) (ghish-only)")
	//ghish:flag mode=extension reason="Read-only health check (same as wt audit)"
	cmd.Flags().BoolVar(&checkOnly, "check", false, "Perform read-only diagnostic health check without modifying any files (same as wt audit) (ghish-only)")
	//ghish:flag mode=extension reason="Structured JSON checklist for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output checklist results as structured JSON (ghish-only)")
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
		Use:         "use [<project>]",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindMutation},
		Short:       "Allocate or resume a project in a warm slot and sync its symlink/IDE state",
		Args:        cobra.MaximumNArgs(1),
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
			receipt := pw_ghish.Receipt{
				Summary: fmt.Sprintf("Project %q mounted in slot %s (branch: %s)", res.Project, res.Slot, res.Branch),
				Next:    "cd " + res.SymlinkPath,
			}
			if res.ForkedFrom != "" {
				receipt.Summary = fmt.Sprintf("Active writer lease collision on %q; automatically warm-forked to %q (%s)", res.ForkedFrom, res.Project, res.Slot)
			} else if res.SwappedOut != "" {
				receipt.Summary = fmt.Sprintf("Swapped out idle project %q -> PARKED; mounted %q in warm slot %s", res.SwappedOut, res.Project, res.Slot)
			}
			if res.IssueID > 0 {
				receipt.Details = append(receipt.Details, pw_ghish.ReceiptField{Key: "Issue", Value: fmt.Sprintf("b/%d", res.IssueID)})
			}
			receipt.Details = append(receipt.Details, pw_ghish.ReceiptField{Key: "Directory", Value: res.SymlinkPath})
			for _, w := range res.Warnings {
				warning := fmt.Sprintf("[%s] %s", w.Subsystem, w.Message)
				if w.Remediation != "" {
					warning += " (" + w.Remediation + ")"
				}
				receipt.Details = append(receipt.Details, pw_ghish.ReceiptField{Key: "Warning", Value: warning})
			}
			pw_ghish.PrintReceipt(out, receipt)
			return nil
		},
	}

	//ghish:flag mode=extension reason="Branch name for the mounted project"
	cmd.Flags().StringVarP(&branchName, "branch", "b", "", "Git branch name (defaults to project name) (ghish-only)")
	//ghish:flag mode=extension reason="Gerrit CL to associate with the project"
	cmd.Flags().StringVar(&clRef, "cl", "", "Optional Gerrit CL number or URL to associate/checkout (ghish-only)")
	//ghish:flag mode=extension reason="Buganizer issue to link and name the branch after"
	cmd.Flags().StringVar(&issueFlag, "issue", "", "Optional Buganizer issue ID or URL to link and auto-name branch (ghish-only)")
	//ghish:flag mode=extension reason="Slot lease mode (write or read)"
	cmd.Flags().StringVar(&modeStr, "mode", "write", "Lease mode: 'write' or 'read' (ghish-only)")
	//ghish:flag mode=extension reason="Agent ID that holds the slot lease"
	cmd.Flags().StringVar(&agentID, "agent", "", "Agent or Conversation ID acquiring the lease (defaults to $CONVERSATION_ID) (ghish-only)")
	//ghish:flag mode=extension reason="Structured JSON result for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON (ghish-only)")
	return cmd
}

func newParkCommand(mgr *Manager) *cobra.Command {
	var force bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:         "park [<project>]",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindMutation},
		Short:       "Unmount a project from its physical slot (MOUNTED -> PARKED), freeing the slot",
		Args:        cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			var rawArg string
			if len(args) > 0 {
				rawArg = args[0]
			}
			resolvedName, _ := mgr.ResolveProjectName(rawArg)
			if resolvedName == "" {
				resolvedName = rawArg
			}
			if err := mgr.Park(rawArg, force); err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), map[string]any{
					"project":   resolvedName,
					"residency": ResidencyParked,
				})
			}
			pw_ghish.PrintReceipt(cmd.OutOrStdout(), pw_ghish.Receipt{
				Summary: fmt.Sprintf("Parked project %q (slot freed; branch and Gerrit CL remain tracked in `./gh wt list`)", resolvedName),
				Next:    fmt.Sprintf("gh wt use %s  (or gh wt list)", resolvedName),
			})
			return nil
		},
	}

	//ghish:flag mode=extension reason="Park despite uncommitted changes"
	cmd.Flags().BoolVarP(&force, "force", "f", false, "Force parking even if working tree has uncommitted changes (ghish-only)")
	//ghish:flag mode=extension reason="Structured JSON result for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON (ghish-only)")
	return cmd
}

func newNextCommand(mgr *Manager) *cobra.Command {
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:         "next [<project>]",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindMutation},
		Short:       "Rebase a mounted project onto origin/main in-place to start its next CL",
		Args:        cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			var rawArg string
			if len(args) > 0 {
				rawArg = args[0]
			}
			resolvedName, _ := mgr.ResolveProjectName(rawArg)
			if resolvedName == "" {
				resolvedName = rawArg
			}
			if err := mgr.Next(rawArg); err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), map[string]any{
					"project": resolvedName,
					"status":  "REBASED_ORIGIN_MAIN",
				})
			}
			pw_ghish.PrintReceipt(cmd.OutOrStdout(), pw_ghish.Receipt{
				Summary: fmt.Sprintf("Rebased project %q onto origin/main in-place! Ready for next CL.", resolvedName),
				Next:    "gh pr create",
			})
			return nil
		},
	}

	//ghish:flag mode=extension reason="Structured JSON result for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON (ghish-only)")
	return cmd
}

func formatListDetails(p ProjectListEntry) string {
	switch p.StatusBadge {
	case BadgeNeedsAttention:
		if p.Residency == ResidencyParked {
			return fmt.Sprintf("%s → ./gh wt use %s", p.Details, p.Project)
		}
		return fmt.Sprintf("%s → ./gh pr view", p.Details)
	case BadgeReadyToLand:
		return fmt.Sprintf("%s → ./gh pr merge --cq", p.Details)
	case BadgeCLMerged:
		return fmt.Sprintf("%s → ./gh wt next", p.Details)
	case BadgeLocalWIP:
		if !p.IsDirty {
			return fmt.Sprintf("%s → ./gh pr create", p.Details)
		}
		return p.Details
	default:
		if strings.Contains(p.Details, "(Abandoned)") {
			return fmt.Sprintf("%s → ./gh wt close", p.Details)
		}
		return p.Details
	}
}

func newListCommand(mgr *Manager) *cobra.Command {
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:         "list",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindList},
		Aliases:     []string{"status", "ls"},
		Short:       "Display live dashboard of mounted and parked projects with Gerrit review/CI statuses",
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

			if len(report.MountedProjects) == 0 {
				fmt.Fprintln(out, "  (No projects currently mounted in slots. Use `./gh wt use <project>` to mount.)")
			} else {
				tw := tabwriter.NewWriter(out, 0, 2, 2, ' ', 0)
				fmt.Fprintln(tw, "PROJECT\tSLOT\tSTATUS\tDETAILS")
				for _, p := range report.MountedProjects {
					fmt.Fprintf(tw, "%s\t%s\t%s\t%s\n",
						p.Project, p.Slot, p.StatusBadge.Display(), formatListDetails(p))
				}
				tw.Flush()
			}
			fmt.Fprintln(out)

			if len(report.ParkedProjects) == 0 {
				fmt.Fprintln(out, "Parked projects: none")
			} else {
				fmt.Fprintf(out, "PARKED PROJECTS (%d Shelved in Git/Gerrit — 0 Slots Used)\n", len(report.ParkedProjects))
				tw := tabwriter.NewWriter(out, 0, 2, 2, ' ', 0)
				fmt.Fprintln(tw, "PROJECT\tSLOT\tSTATUS\tDETAILS")
				for _, p := range report.ParkedProjects {
					fmt.Fprintf(tw, "%s\t%s\t%s\t%s\n",
						p.Project, p.Slot, p.StatusBadge.Display(), formatListDetails(p))
				}
				tw.Flush()
			}
			fmt.Fprintln(out)
			if report.OccupiedSlots == 0 {
				fmt.Fprintf(out, "Verdict: 0/%d slots occupied (%d available)\n", report.TotalSlots, report.AvailableSlots)
				fmt.Fprintln(out, "Next:    gh wt use <project>")
			} else {
				fmt.Fprintf(out, "Verdict: %d/%d slots occupied (%d available)\n", report.OccupiedSlots, report.TotalSlots, report.AvailableSlots)
				fmt.Fprintln(out, "Next:    gh wt use <project>  (or gh wt next <project>)")
			}
			return nil
		},
	}

	//ghish:flag mode=extension reason="Structured JSON dashboard for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output dashboard as structured JSON (ghish-only)")
	return cmd
}

func newCloseCommand(mgr *Manager) *cobra.Command {
	var force bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:         "close <project>",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindMutation},
		Short:       "Permanently close a completed workstream, freeing its slot and removing its active symlink",
		Args:        cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			projName := args[0]
			if resolved, _ := mgr.ResolveProjectName(projName); resolved != "" {
				projName = resolved
			}
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

			if err := mgr.Close(args[0], force); err != nil {
				return err
			}
			if jsonOutput {
				return writeJSON(cmd.OutOrStdout(), map[string]any{
					"project":  projName,
					"status":   "CLOSED",
					"issue_id": closedIssueID,
				})
			}
			receipt := pw_ghish.Receipt{
				Summary: fmt.Sprintf("Closed project %q (slot returned to available pool; Jetski conversations archived)", projName),
				Next:    "gh wt list",
			}
			if closedIssueID > 0 {
				receipt.Details = append(receipt.Details, pw_ghish.ReceiptField{
					Key:   "Issue",
					Value: fmt.Sprintf("b/%d is still open (run './gh issue close %d' if resolved)", closedIssueID, closedIssueID),
				})
			}
			pw_ghish.PrintReceipt(cmd.OutOrStdout(), receipt)
			return nil
		},
	}

	//ghish:flag mode=extension reason="Close despite uncommitted changes"
	cmd.Flags().BoolVarP(&force, "force", "f", false, "Force closing even if uncommitted working tree changes exist (ghish-only)")
	//ghish:flag mode=extension reason="Structured JSON result for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output result as structured JSON (ghish-only)")
	return cmd
}

// maxGCReceiptOrphans caps how many orphaned output bases `wt gc` lists in its
// receipt before folding the rest into a "… and N more" line.
const maxGCReceiptOrphans = 5

func newGCCommand(mgr *Manager) *cobra.Command {
	var dryRun bool
	var jsonOutput bool

	cmd := &cobra.Command{
		Use:         "gc",
		Annotations: map[string]string{pw_ghish.KindAnnotation: pw_ghish.KindMutation},
		Short:       "Sweep ~/.cache/bazel/_bazel_$USER/ for orphaned output bases from deleted worktrees",
		RunE: func(cmd *cobra.Command, args []string) error {
			if mgr == nil {
				return fmt.Errorf("internal error: worktree manager is uninitialized")
			}
			report, err := mgr.GarbageCollect(dryRun)
			if err != nil {
				return err
			}
			if jsonOutput {
				if report.OrphansFound == nil {
					// [] rather than null when nothing was found.
					report.OrphansFound = []OrphanedOutputBase{}
				}
				return writeJSON(cmd.OutOrStdout(), report)
			}

			out := cmd.OutOrStdout()
			if len(report.OrphansFound) == 0 {
				pw_ghish.PrintReceipt(out, pw_ghish.Receipt{
					Summary: "No orphaned Bazel output bases found.",
					Next:    "gh wt list",
				})
				return nil
			}
			receipt := pw_ghish.Receipt{
				Summary: fmt.Sprintf("Cleaned up %d/%d orphaned Bazel output base(s)", report.RemovedCount, len(report.OrphansFound)),
				Next:    "gh wt list",
			}
			if dryRun {
				receipt.Summary = fmt.Sprintf("Would clean up %d orphaned Bazel output base(s) (dry run)", len(report.OrphansFound))
				receipt.Next = "gh wt gc"
			}
			for i, o := range report.OrphansFound {
				if i == maxGCReceiptOrphans {
					receipt.Details = append(receipt.Details, pw_ghish.ReceiptField{
						Key:   "More",
						Value: fmt.Sprintf("… and %d more (use --json to list all)", len(report.OrphansFound)-i),
					})
					break
				}
				receipt.Details = append(receipt.Details, pw_ghish.ReceiptField{
					Key:   "Orphan",
					Value: fmt.Sprintf("%s (workspace: %s, reason: %s)", o.OutputBaseDir, o.WorkspacePath, o.Reason),
				})
			}
			pw_ghish.PrintReceipt(out, receipt)
			return nil
		},
	}

	//ghish:flag mode=extension reason="List orphaned Bazel output bases without deleting"
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "List orphaned output bases without deleting them (ghish-only)")
	//ghish:flag mode=extension reason="Structured JSON report for agents"
	cmd.Flags().BoolVar(&jsonOutput, "json", false, "Output report as structured JSON (ghish-only)")
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
