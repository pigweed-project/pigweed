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
	"sync"

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

// maxGerritQueryBatchSize caps the number of `change:<id>` predicates per
// Gerrit QueryChanges request so large fleets (e.g. 200+ parked projects)
// never exceed Gerrit's index.maxTerms limit.
const maxGerritQueryBatchSize = 30

// BatchChangeIDQueries deduplicates non-empty Change-IDs and groups their
// `change:<id>` predicates into `OR`-joined query strings of at most batchSize
// terms each.
func BatchChangeIDQueries(changeIDs []string, batchSize int) []string {
	if batchSize <= 0 {
		batchSize = maxGerritQueryBatchSize
	}
	var predicates []string
	seen := make(map[string]bool, len(changeIDs))
	for _, cid := range changeIDs {
		trimmed := strings.TrimSpace(cid)
		if trimmed == "" || seen[trimmed] {
			continue
		}
		seen[trimmed] = true
		predicates = append(predicates, fmt.Sprintf("change:%s", trimmed))
	}
	if len(predicates) == 0 {
		return nil
	}

	var queries []string
	for start := 0; start < len(predicates); start += batchSize {
		end := start + batchSize
		if end > len(predicates) {
			end = len(predicates)
		}
		queries = append(queries, strings.Join(predicates[start:end], " OR "))
	}
	return queries
}

const maxGerritBatchConcurrency = 4

func changesToStatusMap(changes []pw_ghish.ChangeInfo) map[string]ChangeStatus {
	batchMap := make(map[string]ChangeStatus)
	for _, c := range changes {
		batchMap[c.ChangeID] = ChangeStatus{
			ChangeID:          c.ChangeID,
			Number:            c.Number,
			Status:            c.Status,
			CodeReviewScore:   pw_ghish.ExtractLabelScore(c.Labels, "Code-Review"),
			CommitQueueScore:  pw_ghish.ExtractLabelScore(c.Labels, "Commit-Queue"),
			UnresolvedThreads: c.UnresolvedCommentCount,
			Subject:           c.Subject,
		}
	}
	return batchMap
}

func queryChangeBatches(
	ctx context.Context,
	queries []string,
	queryBatch func(ctx context.Context, query string) ([]pw_ghish.ChangeInfo, error),
) (map[string]ChangeStatus, error) {
	res := make(map[string]ChangeStatus)
	if len(queries) == 0 {
		return res, nil
	}
	if len(queries) == 1 {
		changes, err := queryBatch(ctx, queries[0])
		if err != nil {
			return res, err
		}
		return changesToStatusMap(changes), nil
	}

	type batchResult struct {
		statuses map[string]ChangeStatus
		err      error
	}
	batchResults := make([]batchResult, len(queries))
	sem := make(chan struct{}, maxGerritBatchConcurrency)
	var wg sync.WaitGroup
	for i, q := range queries {
		wg.Add(1)
		go func(idx int, query string) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()

			changes, err := queryBatch(ctx, query)
			if err != nil {
				batchResults[idx] = batchResult{err: err}
				return
			}
			batchResults[idx] = batchResult{statuses: changesToStatusMap(changes)}
		}(i, q)
	}
	wg.Wait()

	for _, br := range batchResults {
		if br.err != nil {
			return res, br.err
		}
		for k, v := range br.statuses {
			res[k] = v
		}
	}
	return res, nil
}

func (a *gerritStatusAdapter) QueryChangesByID(ctx context.Context, changeIDs []string) (map[string]ChangeStatus, error) {
	queries := BatchChangeIDQueries(changeIDs, maxGerritQueryBatchSize)
	if len(queries) == 0 {
		return make(map[string]ChangeStatus), nil
	}

	client, err := pw_ghish.NewGerritClient(ctx, a.rootCmd)
	if err != nil {
		return make(map[string]ChangeStatus), err
	}

	return queryChangeBatches(ctx, queries, func(ctx context.Context, query string) ([]pw_ghish.ChangeInfo, error) {
		return client.QueryChanges(ctx, query, 0, "DETAILED_LABELS", "SUBMITTABLE")
	})
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

	change, ref, err := chCtx.ResolveFetchRef()
	if err != nil {
		return "", "", fmt.Errorf("failed to resolve git fetch ref for %q: %w", clRef, err)
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
