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
	"context"
	"fmt"
	"strings"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
	"pigweed.dev/pw_ghish"
)

func init() {
	pw_ghish.RootCmd.AddCommand(NewWTCommand(pw_ghish.RootCmd))
	pw_ghish.RegisteredWorkspaceIntegration = &WorkspaceIntegrationAdapter{
		ManagerFn: func() (*Manager, error) {
			return newDefaultManager(pw_ghish.RootCmd)
		},
	}
}

// gerritStatusAdapter bridges pw_ghish's authenticated Gerrit client to GerritStatusProvider.
type gerritStatusAdapter struct {
	rootCmd *cobra.Command
}

func (a *gerritStatusAdapter) QueryChangesByID(ctx context.Context, changeIDs []string) (map[string]ChangeStatus, error) {
	res := make(map[string]ChangeStatus)
	if len(changeIDs) == 0 {
		return res, nil
	}

	var predicates []string
	for _, cid := range changeIDs {
		if trimmed := strings.TrimSpace(cid); trimmed != "" {
			predicates = append(predicates, fmt.Sprintf("change:%s", trimmed))
		}
	}
	if len(predicates) == 0 {
		return res, nil
	}

	client, err := pw_ghish.NewGerritClient(ctx, a.rootCmd)
	if err != nil {
		return res, err
	}

	query := strings.Join(predicates, " OR ")
	opt := &gerrit.QueryChangeOptions{
		QueryOptions: gerrit.QueryOptions{
			Query: []string{query},
		},
		ChangeOptions: gerrit.ChangeOptions{
			AdditionalFields: []string{"DETAILED_LABELS", "SUBMITTABLE"},
		},
	}

	changes, _, err := client.Changes.QueryChanges(ctx, opt)
	if err != nil || changes == nil {
		return res, err
	}

	for _, c := range *changes {
		res[c.ChangeID] = ChangeStatus{
			ChangeID:          c.ChangeID,
			Number:            c.Number,
			Status:            c.Status,
			CodeReviewScore:   pw_ghish.ExtractLabelScore(c.Labels, "Code-Review"),
			CommitQueueScore:  pw_ghish.ExtractLabelScore(c.Labels, "Commit-Queue"),
			UnresolvedThreads: c.UnresolvedCommentCount,
			Subject:           c.Subject,
		}
	}
	return res, nil
}

// ResolveCLFetchRef resolves a Gerrit CL number, URL, shortlink, or Change-Id to its git fetch ref and Change-Id.
func (a *gerritStatusAdapter) ResolveCLFetchRef(ctx context.Context, clRef string) (string, string, error) {
	clRef = strings.TrimSpace(clRef)
	if clRef == "" {
		return "", "", fmt.Errorf("empty CL reference")
	}
	if strings.HasPrefix(clRef, "refs/changes/") {
		return clRef, "", nil
	}

	chCtx, err := pw_ghish.ResolveChangeContext(a.rootCmd, []string{clRef})
	if err != nil {
		return "", "", fmt.Errorf("failed to resolve CL %q: %w", clRef, err)
	}

	opt := &gerrit.ChangeOptions{}
	if chCtx.Revision != "" && chCtx.Revision != "current" {
		opt.AdditionalFields = []string{"ALL_REVISIONS"}
	} else {
		opt.AdditionalFields = []string{"CURRENT_REVISION"}
	}

	change, err := chCtx.GetChange(opt)
	if err != nil {
		return "", "", fmt.Errorf("failed to fetch Gerrit change details for %q: %w", clRef, err)
	}

	revision, err := chCtx.ExtractRevision(change)
	if err != nil {
		return "", "", fmt.Errorf("failed to extract revision for %q: %w", clRef, err)
	}

	ref, err := chCtx.ExtractFetchRef(change, revision)
	if err != nil {
		return "", "", fmt.Errorf("failed to extract git fetch ref for %q: %w", clRef, err)
	}

	return ref, change.ChangeID, nil
}

func newDefaultManager(rootCmd *cobra.Command) (*Manager, error) {
	store, storeErr := DefaultStateStore()
	if storeErr != nil {
		return nil, fmt.Errorf("failed to initialize state store: %w", storeErr)
	}
	bazelDriver, bazelErr := NewDefaultBazelDriver()
	if bazelErr != nil {
		return nil, fmt.Errorf("failed to initialize Bazel driver: %w", bazelErr)
	}
	ideDriver := NewDefaultJetskiIDEDriver()
	gitRunner := NewExecGitRunner()
	statusProvider := &gerritStatusAdapter{rootCmd: rootCmd}
	return NewManager(store, gitRunner, bazelDriver, ideDriver, statusProvider), nil
}

// NewWTCommand constructs the `wt` (worktree) command tree wired with live Gerrit and IDE drivers.
// Unlike the previous implementation, it NEVER falls back to /tmp or an empty BazelDriver if $HOME is missing.
func NewWTCommand(rootCmd *cobra.Command) *cobra.Command {
	mgr, initErr := newDefaultManager(rootCmd)
	cmd := NewCommand(mgr)
	if initErr != nil {
		cmd.PersistentPreRunE = func(c *cobra.Command, args []string) error {
			return initErr
		}
	}
	return cmd
}
