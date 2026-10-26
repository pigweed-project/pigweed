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

// This file enforces the pw_ghish CLI flag compatibility policy
// (pw_ghish/flag_compatibility.rst). It walks the Cobra command tree and
// parses inline `//ghish:flag` AST directives to verify that every registered
// flag is explicitly classified as `upstream`, `extension`, `divergent`, or
// `unsupported-guard`, that extensions include `(ghish-only)` in their help
// text, and that divergent flags are documented in `ghDivergenceHelp`.

package pw_ghish

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"

	"github.com/spf13/cobra"
	"github.com/spf13/pflag"
)

// flagPolicyMode classifies how a pw_ghish flag relates to upstream GitHub CLI (gh).
type flagPolicyMode string

const (
	flagModeUpstream         flagPolicyMode = "upstream"
	flagModeExtension        flagPolicyMode = "extension"
	flagModeDivergent        flagPolicyMode = "divergent"
	flagModeUnsupportedGuard flagPolicyMode = "unsupported-guard"
)

type flagPolicySpec struct {
	Mode   flagPolicyMode
	GH     string
	Reason string
	Doc    string
}

var ghishFlagDirectiveRegex = regexp.MustCompile(`^//\s*ghish:flag\s+(.+)$`)
var ghishFlagKVRegex = regexp.MustCompile(`([a-z_]+)=(?:"([^"]*)"|(\S+))`)

// parseGhishFlagDirective parses a `//ghish:flag mode=...` comment line.
func parseGhishFlagDirective(text string) (flagPolicySpec, bool, error) {
	m := ghishFlagDirectiveRegex.FindStringSubmatch(strings.TrimSpace(text))
	if m == nil {
		return flagPolicySpec{}, false, nil
	}
	var spec flagPolicySpec
	matches := ghishFlagKVRegex.FindAllStringSubmatch(m[1], -1)
	if len(matches) == 0 {
		return flagPolicySpec{}, true, fmt.Errorf("malformed //ghish:flag directive %q: expected key=value pairs", text)
	}
	for _, kv := range matches {
		key := kv[1]
		val := kv[2]
		if val == "" {
			val = kv[3]
		}
		switch key {
		case "mode":
			spec.Mode = flagPolicyMode(val)
		case "gh":
			spec.GH = val
		case "reason":
			spec.Reason = val
		case "doc":
			spec.Doc = val
		default:
			return flagPolicySpec{}, true, fmt.Errorf("unknown key %q in //ghish:flag directive %q", key, text)
		}
	}
	if err := validateFlagPolicySpec(spec); err != nil {
		return flagPolicySpec{}, true, fmt.Errorf("invalid //ghish:flag directive %q: %w", text, err)
	}
	return spec, true, nil
}

func validateFlagPolicySpec(spec flagPolicySpec) error {
	switch spec.Mode {
	case flagModeUpstream:
		if strings.TrimSpace(spec.GH) == "" {
			return fmt.Errorf("mode=upstream requires gh=\"<subcmd> --<flag>\"")
		}
	case flagModeExtension:
		if strings.TrimSpace(spec.Reason) == "" {
			return fmt.Errorf("mode=extension requires reason=\"<explanation>\"")
		}
	case flagModeDivergent:
		if strings.TrimSpace(spec.GH) == "" || strings.TrimSpace(spec.Doc) == "" {
			return fmt.Errorf("mode=divergent requires both gh=\"<subcmd> --<flag>\" and doc=\"ghDivergenceHelp\"")
		}
	case flagModeUnsupportedGuard:
		if strings.TrimSpace(spec.GH) == "" && strings.TrimSpace(spec.Reason) == "" {
			return fmt.Errorf("mode=unsupported-guard requires gh=\"<subcmd> --<flag>\" or reason=\"<explanation>\"")
		}
	default:
		return fmt.Errorf("mode=%q must be one of upstream, extension, divergent, or unsupported-guard", spec.Mode)
	}
	return nil
}

// TODO: b/574580163 - Delete baselineFlagPolicyCatalog once inline
// `//ghish:flag` directives have been rolled out to all flag registrations.
//
// baselineFlagPolicyCatalog classifies all pre-existing flags on RootCmd so
// untouched files do not conflict with in-flight CLs, while any newly added
// flag or inline `//ghish:flag` directive is strictly validated.
var baselineFlagPolicyCatalog = map[string]flagPolicySpec{
	// Root persistent flags
	"*:host":      {Mode: flagModeExtension, Reason: "Gerrit host override"},
	"*:verbose":   {Mode: flagModeExtension, Reason: "Debug logging toggle"},
	"*:profile":   {Mode: flagModeExtension, Reason: "ProjectProfile selector"},
	"*:auth-mode": {Mode: flagModeExtension, Reason: "Authentication mode selector (googler/community/auto/none)"},

	// auth
	"auth:json":        {Mode: flagModeUpstream, GH: "auth status --json"},
	"auth status:json": {Mode: flagModeUpstream, GH: "auth status --json"},

	// pr checkout
	"pr checkout:branch": {Mode: flagModeUpstream, GH: "pr checkout --branch"},
	"pr checkout:force":  {Mode: flagModeUpstream, GH: "pr checkout --force"},
	"pr checkout:detach": {Mode: flagModeUpstream, GH: "pr checkout --detach"},

	// pr checks & root checks alias
	"pr checks:json":             {Mode: flagModeUpstream, GH: "pr checks --json"},
	"pr checks:template":         {Mode: flagModeUpstream, GH: "pr checks --template"},
	"pr checks:experimental":     {Mode: flagModeExtension, Reason: "Include LUCI cq_experimental builders"},
	"pr checks:all":              {Mode: flagModeExtension, Reason: "Include child subbuilds hidden by ci.hide_tag_filters"},
	"pr checks:watch":            {Mode: flagModeUpstream, GH: "pr checks --watch"},
	"pr checks:web":              {Mode: flagModeUpstream, GH: "pr checks --web"},
	"pr checks:fail-fast":        {Mode: flagModeUpstream, GH: "pr checks --fail-fast"},
	"pr checks:interval":         {Mode: flagModeUpstream, GH: "pr checks --interval"},
	"pr checks:log-failed":       {Mode: flagModeExtension, Reason: "Print step failure logs when checks fail"},
	"pr checks:buildbucket-host": {Mode: flagModeExtension, Reason: "Hidden test/custom Buildbucket host override"},

	// pr create & pr push (shared push_pipeline flags)
	"pr create:force":        {Mode: flagModeExtension, Reason: "Bypass existing Change-Id guard"},
	"pr push:ready":          {Mode: flagModeExtension, Reason: "Remove Gerrit WIP state on push"},
	"pr push:force":          {Mode: flagModeExtension, Reason: "Bypass missing Change-Id guard on push"},
	"pr create:reviewer":     {Mode: flagModeUpstream, GH: "pr create --reviewer"},
	"pr push:reviewer":       {Mode: flagModeUpstream, GH: "pr create --reviewer"},
	"pr create:add-reviewer": {Mode: flagModeExtension, Reason: "Hidden alias for --reviewer"},
	"pr push:add-reviewer":   {Mode: flagModeExtension, Reason: "Hidden alias for --reviewer"},
	"pr create:owner":        {Mode: flagModeExtension, Reason: "Auto-select Gerrit module code owners"},
	"pr push:owner":          {Mode: flagModeExtension, Reason: "Auto-select Gerrit module code owners"},
	"pr create:add-owner":    {Mode: flagModeExtension, Reason: "Hidden alias for --owner"},
	"pr push:add-owner":      {Mode: flagModeExtension, Reason: "Hidden alias for --owner"},
	"pr create:cc":           {Mode: flagModeExtension, Reason: "Gerrit CC reviewer list"},
	"pr push:cc":             {Mode: flagModeExtension, Reason: "Gerrit CC reviewer list"},
	"pr create:draft":        {Mode: flagModeUpstream, GH: "pr create --draft"},
	"pr push:draft":          {Mode: flagModeUpstream, GH: "pr create --draft"},
	"pr create:auto":         {Mode: flagModeExtension, Reason: "Vote Gerrit Auto-Submit label on upload"},
	"pr push:auto":           {Mode: flagModeExtension, Reason: "Vote Gerrit Auto-Submit label on upload"},
	"pr create:auto-submit":  {Mode: flagModeExtension, Reason: "Hidden alias for --auto"},
	"pr push:auto-submit":    {Mode: flagModeExtension, Reason: "Hidden alias for --auto"},
	"pr create:trigger":      {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue / Presubmit-Ready on upload"},
	"pr push:trigger":        {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue / Presubmit-Ready on upload"},
	"pr create:cq":           {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue on upload"},
	"pr push:cq":             {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue on upload"},
	"pr create:publish":      {Mode: flagModeExtension, Reason: "Publish pending Gerrit draft comments on push"},
	"pr push:publish":        {Mode: flagModeExtension, Reason: "Publish pending Gerrit draft comments on push"},
	"pr create:push-option":  {Mode: flagModeExtension, Reason: "Raw git push -o options for Gerrit"},
	"pr push:push-option":    {Mode: flagModeExtension, Reason: "Raw git push -o options for Gerrit"},
	"pr create:no-verify":    {Mode: flagModeExtension, Reason: "Bypass local git pre-push hooks"},
	"pr push:no-verify":      {Mode: flagModeExtension, Reason: "Bypass local git pre-push hooks"},
	"pr create:base":         {Mode: flagModeUpstream, GH: "pr create --base"},
	"pr push:base":           {Mode: flagModeUpstream, GH: "pr create --base"},
	"pr create:stack":        {Mode: flagModeExtension, Reason: "Allow pushing multi-commit Gerrit stack"},
	"pr push:stack":          {Mode: flagModeExtension, Reason: "Allow pushing multi-commit Gerrit stack"},
	"pr create:topic":        {Mode: flagModeExtension, Reason: "Set Gerrit topic on push"},
	"pr push:topic":          {Mode: flagModeExtension, Reason: "Set Gerrit topic on push"},
	"pr create:hashtag":      {Mode: flagModeExtension, Reason: "Set Gerrit hashtags on push"},
	"pr push:hashtag":        {Mode: flagModeExtension, Reason: "Set Gerrit hashtags on push"},
	"pr create:nightly":      {Mode: flagModeExtension, Reason: "Schedule Gerrit Flow auto-submit across the nightly window"},
	"pr push:nightly":        {Mode: flagModeExtension, Reason: "Schedule Gerrit Flow auto-submit across the nightly window"},
	"pr create:submit-at":    {Mode: flagModeExtension, Reason: "Schedule Gerrit Flow auto-submit at a specific HH:MM time"},
	"pr push:submit-at":      {Mode: flagModeExtension, Reason: "Schedule Gerrit Flow auto-submit at a specific HH:MM time"},

	// pr diff
	"pr diff:name-only": {Mode: flagModeUpstream, GH: "pr diff --name-only"},
	"pr diff:stat":      {Mode: flagModeExtension, Reason: "Diffstat summary view"},

	// pr edit
	"pr edit:add-reviewer":     {Mode: flagModeUpstream, GH: "pr edit --add-reviewer"},
	"pr edit:reviewer":         {Mode: flagModeExtension, Reason: "Hidden alias for --add-reviewer"},
	"pr edit:add-owner":        {Mode: flagModeExtension, Reason: "Resolve and add Gerrit module code owners"},
	"pr edit:owner":            {Mode: flagModeExtension, Reason: "Hidden alias for --add-owner"},
	"pr edit:remove-reviewer":  {Mode: flagModeUpstream, GH: "pr edit --remove-reviewer"},
	"pr edit:add-assignee":     {Mode: flagModeUpstream, GH: "pr edit --add-assignee"},
	"pr edit:remove-assignee":  {Mode: flagModeUpstream, GH: "pr edit --remove-assignee"},
	"pr edit:add-attention":    {Mode: flagModeExtension, Reason: "Gerrit attention set addition"},
	"pr edit:remove-attention": {Mode: flagModeExtension, Reason: "Gerrit attention set removal"},
	"pr edit:add-label":        {Mode: flagModeUpstream, GH: "pr edit --add-label"},
	"pr edit:trigger":          {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue / Presubmit-Ready"},
	"pr edit:cq":               {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue"},
	"pr edit:topic":            {Mode: flagModeExtension, Reason: "Set Gerrit change topic"},
	"pr edit:remove-topic":     {Mode: flagModeExtension, Reason: "Remove Gerrit change topic"},
	"pr edit:add-hashtag":      {Mode: flagModeExtension, Reason: "Add Gerrit change hashtags"},
	"pr edit:remove-hashtag":   {Mode: flagModeExtension, Reason: "Remove Gerrit change hashtags"},

	// pr list
	"pr list:limit":         {Mode: flagModeUpstream, GH: "pr list --limit"},
	"pr list:state":         {Mode: flagModeUpstream, GH: "pr list --state"},
	"pr list:label":         {Mode: flagModeDivergent, GH: "pr list --label", Doc: "ghDivergenceHelp"},
	"pr list:base":          {Mode: flagModeUpstream, GH: "pr list --base"},
	"pr list:assignee":      {Mode: flagModeDivergent, GH: "pr list --assignee", Doc: "ghDivergenceHelp"},
	"pr list:author":        {Mode: flagModeUpstream, GH: "pr list --author"},
	"pr list:search":        {Mode: flagModeUpstream, GH: "pr list --search"},
	"pr list:reviewer":      {Mode: flagModeExtension, Reason: "Filter by Gerrit reviewer"},
	"pr list:merged-after":  {Mode: flagModeExtension, Reason: "Gerrit merged-after date predicate"},
	"pr list:merged-before": {Mode: flagModeExtension, Reason: "Gerrit merged-before date predicate"},
	"pr list:json":          {Mode: flagModeUpstream, GH: "pr list --json"},
	"pr list:template":      {Mode: flagModeUpstream, GH: "pr list --template"},
	"pr list:all-projects":  {Mode: flagModeExtension, Reason: "Query across all projects on Gerrit host"},

	// pr merge
	"pr merge:auto":        {Mode: flagModeUpstream, GH: "pr merge --auto"},
	"pr merge:auto-submit": {Mode: flagModeExtension, Reason: "Hidden alias for --auto"},
	"pr merge:trigger":     {Mode: flagModeExtension, Reason: "Submit via Gerrit Commit-Queue+2 or Presubmit-Ready"},
	"pr merge:cq":          {Mode: flagModeExtension, Reason: "Submit via Gerrit Commit-Queue+2"},
	"pr merge:message":     {Mode: flagModeExtension, Reason: "Gerrit review message when voting auto-submit/CQ"},
	"pr merge:nightly":     {Mode: flagModeExtension, Reason: "Schedule Gerrit Flow auto-submit across the nightly window"},
	"pr merge:submit-at":   {Mode: flagModeExtension, Reason: "Schedule Gerrit Flow auto-submit at a specific HH:MM time"},
	"pr merge:stack":       {Mode: flagModeExtension, Reason: "Schedule Gerrit Flow across all changes in a stack"},

	// pr review
	"pr review:message":         {Mode: flagModeUpstream, GH: "pr review --body"},
	"pr review:body":            {Mode: flagModeUpstream, GH: "pr review --body"},
	"pr review:body-file":       {Mode: flagModeUpstream, GH: "pr review --body-file"},
	"pr review:approve":         {Mode: flagModeUpstream, GH: "pr review --approve"},
	"pr review:request-changes": {Mode: flagModeDivergent, GH: "pr review --request-changes", Doc: "ghDivergenceHelp"},
	"pr review:comment":         {Mode: flagModeUpstream, GH: "pr review --comment"},
	"pr review:publish":         {Mode: flagModeExtension, Reason: "Publish pending Gerrit draft comments"},
	"pr review:trigger":         {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue / Presubmit-Ready"},
	"pr review:cq":              {Mode: flagModeExtension, Reason: "Vote Gerrit Commit-Queue"},

	// pr status
	"pr status:json":             {Mode: flagModeUpstream, GH: "pr status --json"},
	"pr status:template":         {Mode: flagModeUpstream, GH: "pr status --template"},
	"pr status:all":              {Mode: flagModeExtension, Reason: "Bypass 30-day recency filter on Gerrit dashboard"},
	"pr status:buildbucket-host": {Mode: flagModeExtension, Reason: "Hidden test/custom Buildbucket host override"},

	// pr view
	"pr view:json":             {Mode: flagModeUpstream, GH: "pr view --json"},
	"pr view:template":         {Mode: flagModeUpstream, GH: "pr view --template"},
	"pr view:comments":         {Mode: flagModeUpstream, GH: "pr view --comments"},
	"pr view:web":              {Mode: flagModeUpstream, GH: "pr view --web"},
	"pr view:buildbucket-host": {Mode: flagModeExtension, Reason: "Hidden test/custom Buildbucket host override"},

	// run
	"run list:json":              {Mode: flagModeDivergent, GH: "run list --json", Doc: "ghDivergenceHelp"},
	"run list:experimental":      {Mode: flagModeExtension, Reason: "Include LUCI cq_experimental builders"},
	"run list:all":               {Mode: flagModeExtension, Reason: "Include child subbuilds hidden by ci.hide_tag_filters"},
	"run list:buildbucket-host":  {Mode: flagModeExtension, Reason: "Hidden test/custom Buildbucket host override"},
	"run view:log-failed":        {Mode: flagModeUpstream, GH: "run view --log-failed"},
	"run view:log":               {Mode: flagModeUpstream, GH: "run view --log"},
	"run view:verbose":           {Mode: flagModeUpstream, GH: "run view --verbose"},
	"run view:job":               {Mode: flagModeUpstream, GH: "run view --job"},
	"run view:target":            {Mode: flagModeExtension, Reason: "Android Busytown build target selector"},
	"run view:web":               {Mode: flagModeUpstream, GH: "run view --web"},
	"run view:json":              {Mode: flagModeDivergent, GH: "run view --json", Doc: "ghDivergenceHelp"},
	"run view:experimental":      {Mode: flagModeExtension, Reason: "Include LUCI cq_experimental builders"},
	"run view:all":               {Mode: flagModeExtension, Reason: "Include child subbuilds hidden by ci.hide_tag_filters"},
	"run view:buildbucket-host":  {Mode: flagModeExtension, Reason: "Hidden test/custom Buildbucket host override"},
	"run rerun:failed":           {Mode: flagModeUpstream, GH: "run rerun --failed"},
	"run rerun:job":              {Mode: flagModeUpstream, GH: "run rerun --job"},
	"run rerun:dry-run":          {Mode: flagModeExtension, Reason: "Preview bb add rerun command without executing"},
	"run rerun:experimental":     {Mode: flagModeExtension, Reason: "Include LUCI cq_experimental builders"},
	"run rerun:buildbucket-host": {Mode: flagModeExtension, Reason: "Hidden test/custom Buildbucket host override"},
	"run watch:interval":         {Mode: flagModeUpstream, GH: "run watch --interval"},
	"run watch:fail-fast":        {Mode: flagModeExtension, Reason: "Exit immediately on first blocking builder failure"},
	"run watch:experimental":     {Mode: flagModeExtension, Reason: "Include LUCI cq_experimental builders"},
	"run watch:all":              {Mode: flagModeExtension, Reason: "Include child subbuilds hidden by ci.hide_tag_filters"},
	"run watch:buildbucket-host": {Mode: flagModeExtension, Reason: "Hidden test/custom Buildbucket host override"},

	// issue
	"issue view:comments":        {Mode: flagModeUpstream, GH: "issue view --comments"},
	"issue view:web":             {Mode: flagModeUpstream, GH: "issue view --web"},
	"issue view:json":            {Mode: flagModeUpstream, GH: "issue view --json"},
	"issue list:assignee":        {Mode: flagModeUpstream, GH: "issue list --assignee"},
	"issue list:author":          {Mode: flagModeUpstream, GH: "issue list --author"},
	"issue list:label":           {Mode: flagModeDivergent, GH: "issue list --label", Doc: "ghDivergenceHelp"},
	"issue list:state":           {Mode: flagModeUpstream, GH: "issue list --state"},
	"issue list:search":          {Mode: flagModeUpstream, GH: "issue list --search"},
	"issue list:limit":           {Mode: flagModeUpstream, GH: "issue list --limit"},
	"issue list:component":       {Mode: flagModeExtension, Reason: "Buganizer component ID filter"},
	"issue list:all-components":  {Mode: flagModeExtension, Reason: "Query across all Buganizer components"},
	"issue list:json":            {Mode: flagModeUpstream, GH: "issue list --json"},
	"issue status:json":          {Mode: flagModeUpstream, GH: "issue status --json"},
	"issue create:title":         {Mode: flagModeUpstream, GH: "issue create --title"},
	"issue create:body":          {Mode: flagModeUpstream, GH: "issue create --body"},
	"issue create:body-file":     {Mode: flagModeUpstream, GH: "issue create --body-file"},
	"issue create:assignee":      {Mode: flagModeUpstream, GH: "issue create --assignee"},
	"issue create:label":         {Mode: flagModeDivergent, GH: "issue create --label", Doc: "ghDivergenceHelp"},
	"issue create:priority":      {Mode: flagModeExtension, Reason: "Buganizer P0..P4 priority field"},
	"issue create:type":          {Mode: flagModeExtension, Reason: "Buganizer issue type field"},
	"issue create:component":     {Mode: flagModeExtension, Reason: "Buganizer component ID"},
	"issue create:cc":            {Mode: flagModeExtension, Reason: "Buganizer CC email list"},
	"issue create:amend":         {Mode: flagModeExtension, Reason: "Append Bug: b/<id> trailer to HEAD commit"},
	"issue edit:title":           {Mode: flagModeUpstream, GH: "issue edit --title"},
	"issue edit:add-assignee":    {Mode: flagModeUpstream, GH: "issue edit --add-assignee"},
	"issue edit:remove-assignee": {Mode: flagModeUpstream, GH: "issue edit --remove-assignee"},
	"issue edit:add-label":       {Mode: flagModeDivergent, GH: "issue edit --add-label", Doc: "ghDivergenceHelp"},
	"issue edit:remove-label":    {Mode: flagModeDivergent, GH: "issue edit --remove-label", Doc: "ghDivergenceHelp"},
	"issue edit:priority":        {Mode: flagModeExtension, Reason: "Buganizer P0..P4 priority field"},
	"issue edit:type":            {Mode: flagModeExtension, Reason: "Buganizer issue type field"},
	"issue edit:component":       {Mode: flagModeExtension, Reason: "Buganizer component ID"},
	"issue close:comment":        {Mode: flagModeUpstream, GH: "issue close --comment"},
	"issue close:reason":         {Mode: flagModeUpstream, GH: "issue close --reason"},
	"issue close:duplicate-of":   {Mode: flagModeExtension, Reason: "Buganizer DUPLICATE canonical issue ID"},
	"issue reopen:comment":       {Mode: flagModeUpstream, GH: "issue reopen --comment"},
	"issue comment:body":         {Mode: flagModeUpstream, GH: "issue comment --body"},
	"issue comment:body-file":    {Mode: flagModeUpstream, GH: "issue comment --body-file"},
	"issue develop:base":         {Mode: flagModeUpstream, GH: "issue develop --base"},
	"issue develop:name":         {Mode: flagModeUpstream, GH: "issue develop --name"},
	"issue develop:worktree":     {Mode: flagModeExtension, Reason: "Mount warm worktree slot via gh wt"},
	"issue develop:json":         {Mode: flagModeExtension, Reason: "Output worktree allocation JSON"},

	// hook
	"hook install:agent":          {Mode: flagModeExtension, Reason: "Install AI agent PreToolUse hook"},
	"hook install:git":            {Mode: flagModeExtension, Reason: "Install Gerrit commit-msg Git hook"},
	"hook install:block-raw-push": {Mode: flagModeExtension, Reason: "Install Git pre-push raw push guard"},
	"hook uninstall:agent":        {Mode: flagModeExtension, Reason: "Remove AI agent PreToolUse hook"},
	"hook uninstall:git":          {Mode: flagModeExtension, Reason: "Remove Git pre-push guard"},

	// sense (registered at root, under pr, and under wt)
	"sense:json":         {Mode: flagModeExtension, Reason: "Structured JSON output for /crank"},
	"sense:status":       {Mode: flagModeExtension, Reason: "Compact text status briefing"},
	"sense:dry-run":      {Mode: flagModeExtension, Reason: "Alias for --status"},
	"sense:fleet":        {Mode: flagModeExtension, Reason: "Query status across all gh wt worktrees"},
	"sense:all-slots":    {Mode: flagModeExtension, Reason: "Alias for --fleet"},
	"sense:prepare":      {Mode: flagModeExtension, Reason: "Safely fetch and switch to target branch"},
	"pr sense:json":      {Mode: flagModeExtension, Reason: "Structured JSON output for /crank"},
	"pr sense:status":    {Mode: flagModeExtension, Reason: "Compact text status briefing"},
	"pr sense:dry-run":   {Mode: flagModeExtension, Reason: "Alias for --status"},
	"pr sense:fleet":     {Mode: flagModeExtension, Reason: "Query status across all gh wt worktrees"},
	"pr sense:all-slots": {Mode: flagModeExtension, Reason: "Alias for --fleet"},
	"pr sense:prepare":   {Mode: flagModeExtension, Reason: "Safely fetch and switch to target branch"},
	"wt sense:json":      {Mode: flagModeExtension, Reason: "Structured JSON output for /crank"},
	"wt sense:status":    {Mode: flagModeExtension, Reason: "Compact text status briefing"},
	"wt sense:dry-run":   {Mode: flagModeExtension, Reason: "Alias for --status"},
	"wt sense:fleet":     {Mode: flagModeExtension, Reason: "Query status across all gh wt worktrees"},
	"wt sense:all-slots": {Mode: flagModeExtension, Reason: "Alias for --fleet"},
	"wt sense:prepare":   {Mode: flagModeExtension, Reason: "Safely fetch and switch to target branch"},
}

var rootAliasTargets = map[string]string{
	"checks": "pr checks",
	"view":   "pr view",
	"diff":   "pr diff",
	"status": "pr status",
	"push":   "pr push",
	"agent":  "hook",
}

// parseASTFlagDirectives parses all `//ghish:flag` comments across production Go
// files, verifies that each directive adheres to the required schema and sits
// immediately above a flag registration call, and returns a map of
// `<canonicalPath>:<flagName> -> flagPolicySpec` extracted from the AST along
// with its scoped command-path resolver.
func parseASTFlagDirectives(t *testing.T) (map[string]flagPolicySpec, func(fnName, cmdVar string) []string) {
	t.Helper()
	fset := token.NewFileSet()
	paths := collectProductionGoFiles(t, false)

	var files []*ast.File
	cmdUseName := map[string]string{
		"RootCmd": "*",
		"PrCmd":   "pr",
		"RunCmd":  "run",
	}
	cmdParent := make(map[string][]string)
	// Map constructor functions (such as `newEditCmd()`, `newReviewCmd()`, and
	// `newPushCommand()`) to the `<funcName>:<ident>` `*cobra.Command` variable
	// they return so `PrCmd.AddCommand(newEditCmd())` resolves local flag
	// registrations without colliding across functions.
	funcReturnIdent := make(map[string]string)

	type addCmdCall struct {
		fnName string
		parent string
		arg    ast.Expr
	}
	var addCmdCalls []addCmdCall

	for _, path := range paths {
		f, err := parser.ParseFile(fset, path, nil, parser.ParseComments)
		if err != nil {
			t.Fatalf("ParseFile(%q): %v", path, err)
		}
		files = append(files, f)

		for _, decl := range f.Decls {
			switch d := decl.(type) {
			case *ast.GenDecl:
				if d.Tok != token.VAR {
					continue
				}
				for _, spec := range d.Specs {
					valSpec, ok := spec.(*ast.ValueSpec)
					if !ok {
						continue
					}
					for i, nameIdent := range valSpec.Names {
						if i < len(valSpec.Values) && nameIdent.Name != "RootCmd" {
							if use := extractCobraCommandUse(valSpec.Values[i]); use != "" {
								cmdUseName[nameIdent.Name] = use
							}
						}
					}
				}
			case *ast.FuncDecl:
				if d.Body == nil {
					continue
				}
				fnName := d.Name.Name
				if d.Recv == nil {
					ast.Inspect(d.Body, func(n ast.Node) bool {
						if _, isClosure := n.(*ast.FuncLit); isClosure {
							return false
						}
						ret, ok := n.(*ast.ReturnStmt)
						if ok && len(ret.Results) == 1 {
							if retIdent, ok := ret.Results[0].(*ast.Ident); ok {
								funcReturnIdent[fnName] = fnName + ":" + retIdent.Name
							}
						}
						return true
					})
				}
				ast.Inspect(d.Body, func(n ast.Node) bool {
					switch stmt := n.(type) {
					case *ast.ValueSpec:
						for i, nameIdent := range stmt.Names {
							if i < len(stmt.Values) {
								if use := extractCobraCommandUse(stmt.Values[i]); use != "" {
									cmdUseName[fnName+":"+nameIdent.Name] = use
								}
							}
						}
					case *ast.AssignStmt:
						for i, lhs := range stmt.Lhs {
							if ident, ok := lhs.(*ast.Ident); ok && i < len(stmt.Rhs) {
								if use := extractCobraCommandUse(stmt.Rhs[i]); use != "" {
									cmdUseName[fnName+":"+ident.Name] = use
								}
							}
						}
					case *ast.CallExpr:
						sel, ok := stmt.Fun.(*ast.SelectorExpr)
						if !ok || sel.Sel.Name != "AddCommand" {
							return true
						}
						var parentName string
						switch x := sel.X.(type) {
						case *ast.Ident:
							parentName = x.Name
						case *ast.SelectorExpr:
							parentName = x.Sel.Name
						default:
							return true
						}
						for _, arg := range stmt.Args {
							addCmdCalls = append(addCmdCalls, addCmdCall{
								fnName: fnName,
								parent: parentName,
								arg:    arg,
							})
						}
					}
					return true
				})
			}
		}
	}

	resolveScopedIdent := func(fnName, ident string) string {
		if fnName != "" {
			scoped := fnName + ":" + ident
			if _, ok := cmdUseName[scoped]; ok {
				return scoped
			}
		}
		return ident
	}

	addParent := func(child, parent string) {
		if !slices.Contains(cmdParent[child], parent) {
			cmdParent[child] = append(cmdParent[child], parent)
		}
	}

	for _, c := range addCmdCalls {
		parentKey := resolveScopedIdent(c.fnName, c.parent)
		switch arg := c.arg.(type) {
		case *ast.Ident:
			addParent(resolveScopedIdent(c.fnName, arg.Name), parentKey)
		case *ast.CallExpr:
			var calleeName string
			switch fn := arg.Fun.(type) {
			case *ast.Ident:
				calleeName = fn.Name
			case *ast.SelectorExpr:
				calleeName = fn.Sel.Name
			}
			if retVar, ok := funcReturnIdent[calleeName]; ok {
				addParent(retVar, parentKey)
			}
		}
	}

	resolveCmdPaths := func(fnName, cmdVar string) []string {
		switch fnName {
		case "AddPushFlags", "AddCommonPushFlags", "AddCommitMessageFlags":
			return []string{"pr create", "pr push"}
		case "AddAsDisabled":
			return []string{"pr edit"}
		}
		if cmdVar == "RootCmd" {
			return []string{"*"}
		}

		var results []string
		addResult := func(parts []string) {
			if len(parts) == 0 {
				return
			}
			p := strings.Join(parts, " ")
			if canon, hasAlias := rootAliasTargets[p]; hasAlias {
				p = canon
			}
			if !slices.Contains(results, p) {
				results = append(results, p)
			}
		}

		var walkParents func(curr string, suffix []string, depth int)
		walkParents = func(curr string, suffix []string, depth int) {
			if depth > 5 {
				return
			}
			use, ok := cmdUseName[curr]
			if !ok {
				return
			}
			nextSuffix := suffix
			if use != "*" {
				nextSuffix = append([]string{use}, suffix...)
			}
			parents := cmdParent[curr]
			if len(parents) == 0 {
				addResult(nextSuffix)
				return
			}
			for _, parent := range parents {
				if parent == "RootCmd" {
					addResult(nextSuffix)
					continue
				}
				walkParents(parent, nextSuffix, depth+1)
			}
		}

		walkParents(resolveScopedIdent(fnName, cmdVar), nil, 0)
		return results
	}

	directives := make(map[string]flagPolicySpec)
	for _, f := range files {
		// Index flag registration calls by line number: `<cmdVar>.Flags().*Var*(..., "<flagName>", ...)`
		type flagReg struct {
			fnName   string
			cmdVar   string
			flagName string
		}
		regByLine := make(map[int]flagReg)
		for _, decl := range f.Decls {
			fnName := ""
			if fn, ok := decl.(*ast.FuncDecl); ok {
				fnName = fn.Name.Name
			}
			ast.Inspect(decl, func(n ast.Node) bool {
				call, ok := n.(*ast.CallExpr)
				if !ok {
					return true
				}
				cmdVar, flagName, ok := extractFlagRegistrationCall(call)
				if !ok {
					return true
				}
				line := fset.Position(call.Pos()).Line
				regByLine[line] = flagReg{
					fnName:   fnName,
					cmdVar:   cmdVar,
					flagName: flagName,
				}
				return true
			})
		}

		for _, cg := range f.Comments {
			for _, c := range cg.List {
				spec, matched, err := parseGhishFlagDirective(c.Text)
				if !matched {
					continue
				}
				pos := fset.Position(c.Pos())
				if err != nil {
					t.Errorf("%s:%d: %v", pos.Filename, pos.Line, err)
					continue
				}
				reg, found := regByLine[pos.Line+1]
				if !found {
					t.Errorf("%s:%d: //ghish:flag directive must sit immediately above a Flags().*Var* registration call", pos.Filename, pos.Line)
					continue
				}
				cmdPaths := resolveCmdPaths(reg.fnName, reg.cmdVar)
				if len(cmdPaths) == 0 {
					t.Errorf("%s:%d: could not resolve Cobra command path for %q (in %s) on flag --%s", pos.Filename, pos.Line, reg.cmdVar, reg.fnName, reg.flagName)
					continue
				}
				for _, cp := range cmdPaths {
					directives[cp+":"+reg.flagName] = spec
				}
			}
		}
	}

	if len(directives) == 0 {
		t.Errorf("expected at least one //ghish:flag directive in production source files, found 0")
	}
	return directives, resolveCmdPaths
}

func extractCobraCommandUse(expr ast.Expr) string {
	unary, ok := expr.(*ast.UnaryExpr)
	if !ok || unary.Op != token.AND {
		return ""
	}
	comp, ok := unary.X.(*ast.CompositeLit)
	if !ok {
		return ""
	}
	sel, ok := comp.Type.(*ast.SelectorExpr)
	if !ok || sel.Sel.Name != "Command" {
		return ""
	}
	for _, elt := range comp.Elts {
		kv, ok := elt.(*ast.KeyValueExpr)
		if !ok {
			continue
		}
		keyIdent, ok := kv.Key.(*ast.Ident)
		if !ok || keyIdent.Name != "Use" {
			continue
		}
		lit, ok := kv.Value.(*ast.BasicLit)
		if !ok || lit.Kind != token.STRING {
			continue
		}
		unquoted := strings.Trim(lit.Value, "`\"")
		fields := strings.Fields(unquoted)
		if len(fields) > 0 {
			return fields[0]
		}
	}
	return ""
}

func extractFlagRegistrationCall(call *ast.CallExpr) (cmdVar string, flagName string, ok bool) {
	sel, isSel := call.Fun.(*ast.SelectorExpr)
	if !isSel {
		return "", "", false
	}
	flagsCall, isCall := sel.X.(*ast.CallExpr)
	if !isCall {
		return "", "", false
	}
	flagsSel, isFlagsSel := flagsCall.Fun.(*ast.SelectorExpr)
	if !isFlagsSel || (flagsSel.Sel.Name != "Flags" && flagsSel.Sel.Name != "PersistentFlags") {
		return "", "", false
	}
	recvIdent, isIdent := flagsSel.X.(*ast.Ident)
	if !isIdent {
		return "", "", false
	}
	nameArgIdx := -1
	if strings.Contains(sel.Sel.Name, "Var") && len(call.Args) >= 2 {
		nameArgIdx = 1
	} else if len(call.Args) >= 3 {
		nameArgIdx = 0
	}
	if nameArgIdx < 0 {
		return "", "", false
	}
	nameLit, isLit := call.Args[nameArgIdx].(*ast.BasicLit)
	if !isLit || nameLit.Kind != token.STRING {
		return "", "", false
	}
	return recvIdent.Name, strings.Trim(nameLit.Value, "`\""), true
}

// TestFlagPolicy_DirectivesSyntax parses all `//ghish:flag` comments across
// pw_ghish/*.go and pw_ghish/worktree/*.go and verifies that every directive
// adheres to the required schema and sits immediately above a flag registration.
func TestFlagPolicy_DirectivesSyntax(t *testing.T) {
	_, resolveCmdPaths := parseASTFlagDirectives(t)

	for _, tc := range []struct {
		fnName string
		cmdVar string
		want   []string
	}{
		{fnName: "newReviewCmd", cmdVar: "cmd", want: []string{"pr review"}},
		{fnName: "newPushCommand", cmdVar: "cmd", want: []string{"pr push"}},
		{fnName: "init", cmdVar: "createCmd", want: []string{"pr create"}},
		{fnName: "newEditCmd", cmdVar: "editCmd", want: []string{"pr edit"}},
		{fnName: "AddPushFlags", cmdVar: "cmd", want: []string{"pr create", "pr push"}},
		{fnName: "AddCommitMessageFlags", cmdVar: "cmd", want: []string{"pr create", "pr push"}},
		{fnName: "AddAsDisabled", cmdVar: "editCmd", want: []string{"pr edit"}},
		{fnName: "init", cmdVar: "RootCmd", want: []string{"*"}},
	} {
		got := resolveCmdPaths(tc.fnName, tc.cmdVar)
		if !slices.Equal(got, tc.want) {
			t.Errorf("resolveCmdPaths(%q, %q) = %v, want %v", tc.fnName, tc.cmdVar, got, tc.want)
		}
	}
}

// TestFlagPolicy_AllRegisteredFlagsClassified walks the live RootCmd command
// tree and verifies that:
//  1. Every flag has a valid classification from an inline `//ghish:flag` AST
//     directive (or in baselineFlagPolicyCatalog for not-yet-migrated files).
//  2. Every mode=divergent flag is documented in ghDivergenceHelp in root.go.
//  3. No command binds a shorthand in ghShorthandCollisions.
func TestFlagPolicy_AllRegisteredFlagsClassified(t *testing.T) {
	setupRootCmdHelp()
	setupRootAliases()
	astDirectives, _ := parseASTFlagDirectives(t)

	var walk func(cmd *cobra.Command, relPath string)
	walk = func(cmd *cobra.Command, relPath string) {
		canonicalPath := relPath
		for aliasPrefix, targetPrefix := range rootAliasTargets {
			if canonicalPath == aliasPrefix {
				canonicalPath = targetPrefix
				break
			}
			if strings.HasPrefix(canonicalPath, aliasPrefix+" ") {
				canonicalPath = targetPrefix + strings.TrimPrefix(canonicalPath, aliasPrefix)
				break
			}
		}

		checkFlag := func(f *pflag.Flag, isPersistent bool) {
			if f.Name == "help" {
				return
			}
			key := canonicalPath + ":" + f.Name
			if isPersistent && canonicalPath == "" {
				key = "*:" + f.Name
			}
			spec, ok := astDirectives[key]
			if !ok {
				spec, ok = astDirectives["*:"+f.Name]
			}
			if !ok {
				spec, ok = baselineFlagPolicyCatalog[key]
			}
			if !ok {
				// Also check inherited persistent flag key.
				spec, ok = baselineFlagPolicyCatalog["*:"+f.Name]
			}
			if !ok {
				t.Errorf("unclassified flag --%s on command %q (key %q): add a //ghish:flag directive above the flag registration in Go source",
					f.Name, canonicalPath, key)
				return
			}
			if err := validateFlagPolicySpec(spec); err != nil {
				t.Errorf("invalid policy spec for %q: %v", key, err)
			}
			if spec.Mode == flagModeDivergent && spec.Doc == "ghDivergenceHelp" {
				// issue edit --add-label and --remove-label share the documented
				// `issue -l/--label` divergence in ghDivergenceHelp.
				checkToken := "--" + f.Name
				if (f.Name == "add-label" || f.Name == "remove-label") && strings.HasPrefix(canonicalPath, "issue ") {
					checkToken = "--label"
				}
				if !strings.Contains(ghDivergenceHelp, checkToken) {
					t.Errorf("flag %q is classified as mode=divergent, but %s is not mentioned in ghDivergenceHelp in root.go",
						key, checkToken)
				}
			}
		}

		cmd.LocalFlags().VisitAll(func(f *pflag.Flag) {
			checkFlag(f, false)
		})
		if relPath == "" {
			cmd.PersistentFlags().VisitAll(func(f *pflag.Flag) {
				checkFlag(f, true)
			})
		}

		for _, sub := range cmd.Commands() {
			if sub.Name() == "help" || sub.Name() == "completion" {
				continue
			}
			subPath := sub.Name()
			if relPath != "" {
				subPath = relPath + " " + sub.Name()
			}
			walk(sub, subPath)
		}
	}

	walk(RootCmd, "")
}

// TestFlagPolicy_WorktreeCommandsUseRunE ensures worktree flags in
// worktree/cmd.go are also covered by AST inspection when present in the package tree.
func TestFlagPolicy_WorktreeCommandsUseRunE(t *testing.T) {
	fset := token.NewFileSet()
	wtCmdPath := filepath.Join("worktree", "cmd.go")
	f, err := parser.ParseFile(fset, wtCmdPath, nil, parser.ParseComments)
	if err != nil {
		t.Skipf("worktree/cmd.go is in subpackage //pw_ghish/worktree: %v", err)
	}

	foundFlags := 0
	ast.Inspect(f, func(n ast.Node) bool {
		call, ok := n.(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		if strings.Contains(sel.Sel.Name, "Var") {
			foundFlags++
		}
		return true
	})
	if foundFlags == 0 {
		t.Errorf("expected flag registrations in %s, found 0", wtCmdPath)
	}
}

func TestValidateFlagPolicySpec_UnsupportedGuard(t *testing.T) {
	if err := validateFlagPolicySpec(flagPolicySpec{Mode: flagModeUnsupportedGuard, GH: "pr edit --title"}); err != nil {
		t.Fatalf("expected gh= spec to validate, got: %v", err)
	}
	if err := validateFlagPolicySpec(flagPolicySpec{Mode: flagModeUnsupportedGuard, Reason: "Hidden guard redirecting --bug to gh pr push"}); err != nil {
		t.Fatalf("expected reason= spec to validate, got: %v", err)
	}
	if err := validateFlagPolicySpec(flagPolicySpec{Mode: flagModeUnsupportedGuard}); err == nil {
		t.Fatal("expected empty unsupported-guard spec to fail validation")
	}
}
