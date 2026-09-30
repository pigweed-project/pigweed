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
	"bytes"
	"crypto/rand"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"time"

	"github.com/spf13/cobra"
)

var (
	// HookUserHomeDir returns the user's home directory for hook installation.
	// Overridable in hermetic tests.
	HookUserHomeDir = os.UserHomeDir

	// HookExecutablePath returns the path to the currently running gh-ish binary.
	// Overridable in hermetic tests.
	HookExecutablePath = os.Executable
)

var (
	gitPushCmdRegex = regexp.MustCompile(
		`(?:^|[;&|()\n` + "`" + `])\s*(?:(?:sudo|env|command)\s+)*\bgit\s+` +
			`(?:(?:-[Cc]|--git-dir|--work-tree|--namespace)\s+\S+\s+|-[^\s]+\s+)*` +
			`push\b`,
	)
	gitPushHelpRegex = regexp.MustCompile(`\bgit\s+(?:.*?\s+)?push\s+(?:--help|-h)\b`)

	gerritBuildbucketRestRegex = regexp.MustCompile(
		`(?is)\b(?:curl|gob-curl|wget)\b[^;&|]*?` +
			`(?:[a-z0-9-]+-review\.googlesource\.com|cr-buildbucket\.appspot\.com|\.gitcookies)` +
			`|\b(?:cat|grep|awk|sed|python3?)\b[^;&|]*?\.gitcookies\b`,
	)

	bbAddCmdRegex = regexp.MustCompile(
		`(?:^|[;&|()\n` + "`" + `])\s*(?:(?:sudo|env|command)\s+)*\bbb\s+add\b`,
	)

	legacyScriptsRegex = regexp.MustCompile(
		`(?:search_builds\.py|\./pw\s+change\s+(?:comments|push|review))\b`,
	)

	changeIDLineRegex      = regexp.MustCompile(`(?m)^Change-Id:\s*I[0-9a-fA-F]{40}\s*$`)
	signedOffByPrefixRegex = regexp.MustCompile(`^Signed-off-by:`)
)

// HookCmd manages AI agent PreToolUse hooks and Git lifecycle hooks.
var HookCmd = &cobra.Command{
	Use:     "hook",
	Aliases: []string{"agent"},
	Short:   "Manage AI agent PreToolUse guards and Git hooks",
	Long: `Manage and execute AI agent pre-execution tool guards (PreToolUse / beforeShellExecution)
and Git repository hooks (commit-msg, pre-push) in pure Go without Python or shell dependencies.`,
}

var (
	hookInstallAgentFlag        string
	hookInstallGitFlag          bool
	hookInstallBlockRawPushFlag bool

	hookUninstallAgentFlag string
	hookUninstallGitFlag   bool
)

var hookPreToolUseCmd = &cobra.Command{
	Use:   "pre-tool-use",
	Short: "Evaluate an AI agent PreToolUse / beforeShellExecution JSON payload from stdin",
	Args:  cobra.NoArgs,
	RunE: func(cmd *cobra.Command, args []string) error {
		rawInput, err := io.ReadAll(cmd.InOrStdin())
		if err != nil {
			return fmt.Errorf("failed to read PreToolUse JSON from stdin: %w", err)
		}
		respJSON, err := EvaluatePreToolUsePayload(rawInput)
		if err != nil {
			return err
		}
		_, err = fmt.Fprintln(cmd.OutOrStdout(), string(respJSON))
		return err
	},
}

var hookCommitMsgCmd = &cobra.Command{
	Use:   "commit-msg <file>",
	Short: "Ensure a Gerrit Change-Id trailer is present in a commit message file",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		return EnsureChangeIDInFile(args[0])
	},
}

var hookPrePushCmd = &cobra.Command{
	Use:          "pre-push",
	Short:        "Enforce that git push is invoked via ./gh when ghish.blockrawpush is enabled",
	Args:         cobra.ArbitraryArgs,
	SilenceUsage: true,
	RunE: func(cmd *cobra.Command, args []string) error {
		if os.Getenv("GH_ISH_ACTIVE") == "1" || os.Getenv("GH_ISH_ALLOW_RAW_PUSH") == "1" {
			return nil
		}
		cfg := GetConfig(cmd)
		if cfg == nil {
			return fmt.Errorf("internal error: command config is uninitialized")
		}
		val, err := cfg.GitClient().ConfigGet(cmd.Context(), "ghish.blockrawpush")
		if err != nil || !strings.EqualFold(strings.TrimSpace(val), "true") {
			return nil
		}
		return fmt.Errorf("raw 'git push' is blocked because 'ghish.blockrawpush' is enabled.\n\n" +
			"Why: Pushing directly with 'git push' bypasses branch memory, multi-commit stack guards,\n" +
			"and Commit-Queue flag validation.\n\n" +
			"Remedies:\n" +
			"  1. Create a new Gerrit CL:          ./gh pr create [--cq] [--draft]\n" +
			"  2. Upload a new patchset to a CL:   ./gh pr push [--cq]\n" +
			"  3. Bypass once for manual recovery: GH_ISH_ALLOW_RAW_PUSH=1 git push ...\n" +
			"  4. Disable this guard permanently:  ./gh hook uninstall --git\n" +
			"  5. Inspect active branch status:    ./gh pr status")
	},
}

var hookInstallCmd = &cobra.Command{
	Use:   "install",
	Short: "Install opt-in AI agent PreToolUse hooks and/or Git repository hooks",
	Args:  cobra.NoArgs,
	RunE:  runHookInstall,
}

var hookUninstallCmd = &cobra.Command{
	Use:   "uninstall",
	Short: "Remove installed AI agent PreToolUse hooks and/or Git pre-push hooks",
	Args:  cobra.NoArgs,
	RunE:  runHookUninstall,
}

var hookStatusCmd = &cobra.Command{
	Use:   "status",
	Short: "Show installation status of AI agent and Git hooks",
	Args:  cobra.NoArgs,
	RunE:  runHookStatus,
}

func init() {
	hookInstallCmd.Flags().StringVar(&hookInstallAgentFlag, "agent", "", "Install user-level agent PreToolUse hook (jetski, claude, cursor, all)")
	hookInstallCmd.Flags().Lookup("agent").NoOptDefVal = "all"
	hookInstallCmd.Flags().BoolVar(&hookInstallGitFlag, "git", false, "Install Git commit-msg hook in the current repository")
	hookInstallCmd.Flags().BoolVar(&hookInstallBlockRawPushFlag, "block-raw-push", false, "Enable ghish.blockrawpush and install the Git pre-push guard")

	hookUninstallCmd.Flags().StringVar(&hookUninstallAgentFlag, "agent", "", "Remove user-level agent PreToolUse hook (jetski, claude, cursor, all)")
	hookUninstallCmd.Flags().Lookup("agent").NoOptDefVal = "all"
	hookUninstallCmd.Flags().BoolVar(&hookUninstallGitFlag, "git", false, "Disable ghish.blockrawpush and remove the Git pre-push guard")

	HookCmd.AddCommand(hookPreToolUseCmd)
	HookCmd.AddCommand(hookCommitMsgCmd)
	HookCmd.AddCommand(hookPrePushCmd)
	HookCmd.AddCommand(hookInstallCmd)
	HookCmd.AddCommand(hookUninstallCmd)
	HookCmd.AddCommand(hookStatusCmd)

	RootCmd.AddCommand(HookCmd)
}

// CheckAgentCommand inspects a proposed shell command and returns a non-empty
// remediation string if the command should use ./gh instead or would clobber a
// Gerrit Change-Id trailer.
func CheckAgentCommand(command string) string {
	trimmed := strings.TrimSpace(command)
	if trimmed == "" {
		return ""
	}

	if reason := detectChangeIDClobberingCommand(command); reason != "" {
		return reason
	}

	if gitPushCmdRegex.MatchString(command) && !gitPushHelpRegex.MatchString(command) {
		return "Blocked raw 'git push' by AI agent. Always use './gh' instead of raw 'git push':\n" +
			"  • Create a new Gerrit CL:        ./gh pr create [--cq] [--draft]\n" +
			"  • Upload a new patchset to a CL: ./gh pr push [--cq]\n" +
			"  • Push a stack of commits:       ./gh pr push --stack (or ./gh pr create --stack)"
	}

	if gerritBuildbucketRestRegex.MatchString(command) {
		return "Blocked direct Gerrit/Buildbucket REST call or .gitcookies access. Always use './gh' " +
			"(which handles corp and cookie auth automatically):\n" +
			"  • View CL metadata & comments: ./gh pr view [<id>] --comments\n" +
			"  • View CL unified patch diff:  ./gh pr diff [<id>]\n" +
			"  • Reply/resolve inline thread: ./gh pr comment [<id>] --path <file> --line <line> -m '<msg>' --resolved [--draft]\n" +
			"  • Check or watch CI tryjobs:   ./gh pr checks [<id>] [--watch --fail-fast]\n" +
			"  • Inspect failed CI build log: ./gh run view [<id>] --log-failed"
	}

	if bbAddCmdRegex.MatchString(command) {
		return "Blocked manual 'bb add' invocation. Use './gh run rerun' instead:\n" +
			"  • Rerun all failed checks: ./gh run rerun [<id>] --failed\n" +
			"  • Rerun a single builder:  ./gh run rerun [<id>] -j <builder>"
	}

	if legacyScriptsRegex.MatchString(command) {
		return "Blocked legacy Gerrit helper script. Use './gh' instead:\n" +
			"  • Check CI builds:      ./gh pr checks [<id>]\n" +
			"  • Read review comments: ./gh pr view [<id>] --comments"
	}

	return ""
}

func formatChangeIDClobberBlockMessage(detail string) string {
	return fmt.Sprintf(
		"Blocked %s: overwriting or dropping an existing 'Change-Id:' trailer causes Gerrit to create a duplicate CL or reject the push.\n"+
			"  • Amend staged files without changing the message:\n"+
			"      git commit --amend --no-edit\n"+
			"  • Surgically edit the commit message via a file (preferred over re-typing -m):\n"+
			"      1. Dump current message: git log -1 --format=%%B HEAD > \"$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP\"\n"+
			"      2. Edit COMMIT_EDITMSG_TMP surgically (keep the original 'Change-Id: I...' footer intact;\n"+
			"         if combining multiple commits, keep ONLY the earliest commit's Change-Id)\n"+
			"      3. Apply edited message: git commit --amend --only -F \"$(git rev-parse --git-dir)/COMMIT_EDITMSG_TMP\"\n"+
			"  • Or explicitly include the existing Change-Id in -m:\n"+
			"      git commit --amend -m \"<subject>\" -m \"<body>\" -m \"Change-Id: I...\"",
		detail,
	)
}

// detectChangeIDClobberingCommand inspects a shell command for git operations
// that overwrite, drop, or duplicate Gerrit Change-Id footers:
//   - git commit --amend -m / --message without a Change-Id: trailer
//   - git commit --amend -F - / --file=- without a Change-Id: trailer in the command
//   - git commit --amend -F <file> when <file> already exists on disk and lacks Change-Id:
//   - git commit --amend -C / -c / --reuse-message=<rev> with a non-HEAD revision
//   - Chained git reset (soft/mixed) or git merge --squash followed by git commit -m without Change-Id:
//   - Any git commit message or -F file containing multiple distinct Change-Id: trailers
//   - git filter-branch
func detectChangeIDClobberingCommand(command string) string {
	segments := splitShellSegments(command)
	sawHistoryResetOrSquash := false
	var priorSegments strings.Builder

	for idx, seg := range segments {
		if idx > 0 {
			priorSegments.WriteString(" ; ")
			priorSegments.WriteString(segments[idx-1])
		}
		tokens := tokenizeShellArgs(seg)
		subcmd, rest, ok := parseGitSubcommandTokens(tokens)
		if !ok {
			continue
		}

		switch subcmd {
		case "filter-branch":
			return formatChangeIDClobberBlockMessage("'git filter-branch' (rewrites commit history without preserving Gerrit Change-Id trailers)")

		case "reset":
			if isCommitRewindingReset(rest) {
				sawHistoryResetOrSquash = true
			}

		case "merge":
			for _, t := range rest {
				if t == "--squash" {
					sawHistoryResetOrSquash = true
					break
				}
			}

		case "commit":
			if reason := checkGitCommitTokens(rest, command, priorSegments.String(), sawHistoryResetOrSquash); reason != "" {
				return reason
			}
		}
	}
	return ""
}

func parseGitSubcommandTokens(tokens []string) (string, []string, bool) {
	i := 0
	for i < len(tokens) {
		t := tokens[i]
		if t == "sudo" || t == "env" || t == "command" || (strings.Contains(t, "=") && !strings.HasPrefix(t, "-")) {
			i++
			continue
		}
		break
	}
	if i >= len(tokens) {
		return "", nil, false
	}
	exe := tokens[i]
	if exe != "git" && !strings.HasSuffix(exe, "/git") {
		return "", nil, false
	}
	i++

	// Skip global git flags before the subcommand.
	for i < len(tokens) {
		t := tokens[i]
		if !strings.HasPrefix(t, "-") {
			break
		}
		if t == "-C" || t == "-c" || t == "--git-dir" || t == "--work-tree" || t == "--namespace" {
			i += 2
			continue
		}
		i++
	}
	if i >= len(tokens) {
		return "", nil, false
	}
	return tokens[i], tokens[i+1:], true
}

var gitHashRegex = regexp.MustCompile(`^[0-9a-fA-F]{7,40}$`)

func isCommitRewindingReset(rest []string) bool {
	hasHard := false
	hasSoftOrMixed := false
	hasRevArg := false
	for _, t := range rest {
		if t == "--" {
			break
		}
		if t == "--hard" || t == "--keep" || t == "--merge" {
			hasHard = true
		}
		if t == "--soft" || t == "--mixed" {
			hasSoftOrMixed = true
		}
		if !strings.HasPrefix(t, "-") {
			if strings.Contains(t, "~") || strings.Contains(t, "^") || strings.Contains(t, "@") || t == "ORIG_HEAD" || strings.HasPrefix(t, "origin/") || gitHashRegex.MatchString(t) {
				hasRevArg = true
			}
		}
	}
	if hasHard {
		return false
	}
	return hasSoftOrMixed || hasRevArg
}

func checkGitCommitTokens(rest []string, fullCommand, priorSegments string, sawHistoryResetOrSquash bool) string {
	hasAmend := false
	hasMsg := false
	var msgParts []string
	hasFile := false
	var filePath string
	hasReuse := false
	var reuseRev string

	i := 0
	for i < len(rest) {
		t := rest[i]
		if t == "--" {
			break
		}
		if t == "--amend" {
			hasAmend = true
			i++
			continue
		}
		if t == "-m" || t == "--message" {
			hasMsg = true
			if i+1 < len(rest) {
				msgParts = append(msgParts, rest[i+1])
				i += 2
				continue
			}
			i++
			continue
		}
		if strings.HasPrefix(t, "--message=") {
			hasMsg = true
			msgParts = append(msgParts, strings.TrimPrefix(t, "--message="))
			i++
			continue
		}
		if t == "-F" || t == "--file" {
			hasFile = true
			if i+1 < len(rest) {
				filePath = rest[i+1]
				i += 2
				continue
			}
			i++
			continue
		}
		if strings.HasPrefix(t, "--file=") {
			hasFile = true
			filePath = strings.TrimPrefix(t, "--file=")
			i++
			continue
		}
		if t == "-C" || t == "--reuse-message" || t == "-c" || t == "--reedit-message" {
			hasReuse = true
			if i+1 < len(rest) {
				reuseRev = rest[i+1]
				i += 2
				continue
			}
			i++
			continue
		}
		if strings.HasPrefix(t, "--reuse-message=") {
			hasReuse = true
			reuseRev = strings.TrimPrefix(t, "--reuse-message=")
			i++
			continue
		}
		if strings.HasPrefix(t, "--reedit-message=") {
			hasReuse = true
			reuseRev = strings.TrimPrefix(t, "--reedit-message=")
			i++
			continue
		}
		if t == "--author" || t == "--date" || t == "-t" || t == "--template" || t == "--cleanup" {
			i += 2
			continue
		}
		if strings.HasPrefix(t, "-") && !strings.HasPrefix(t, "--") {
			cluster := t[1:]
			if mIdx := strings.IndexByte(cluster, 'm'); mIdx >= 0 {
				hasMsg = true
				afterM := cluster[mIdx+1:]
				if afterM != "" {
					msgParts = append(msgParts, afterM)
					i++
					continue
				}
				if i+1 < len(rest) {
					msgParts = append(msgParts, rest[i+1])
					i += 2
					continue
				}
			}
			if fIdx := strings.IndexByte(cluster, 'F'); fIdx >= 0 {
				hasFile = true
				afterF := cluster[fIdx+1:]
				if afterF != "" {
					filePath = afterF
					i++
					continue
				}
				if i+1 < len(rest) {
					filePath = rest[i+1]
					i += 2
					continue
				}
			}
		}
		i++
	}

	if hasMsg {
		combined := strings.ReplaceAll(strings.Join(msgParts, "\n\n"), `\n`, "\n")
		if err := CheckMultipleChangeIDs(combined, "commit message"); err != nil {
			return formatChangeIDClobberBlockMessage(fmt.Sprintf("'git commit' with multiple Change-Id trailers (%v)", err))
		}
		if hasAmend && !changeIDLineRegex.MatchString(combined) {
			return formatChangeIDClobberBlockMessage("'git commit --amend -m' without a 'Change-Id:' trailer")
		}
		if sawHistoryResetOrSquash && !changeIDLineRegex.MatchString(combined) {
			return formatChangeIDClobberBlockMessage("'git reset / merge --squash' followed by 'git commit -m' without preserving the original 'Change-Id:' trailer")
		}
	}

	if hasFile {
		if filePath == "-" {
			normalizedCmd := strings.ReplaceAll(fullCommand, `\n`, "\n")
			if err := CheckMultipleChangeIDs(normalizedCmd, "stdin commit message"); err != nil {
				return formatChangeIDClobberBlockMessage(fmt.Sprintf("'git commit -F -' with multiple Change-Id trailers (%v)", err))
			}
			if (hasAmend || sawHistoryResetOrSquash) && !changeIDLineRegex.MatchString(normalizedCmd) {
				return formatChangeIDClobberBlockMessage("'git commit --amend -F -' without a 'Change-Id:' trailer in stdin")
			}
		} else if filePath != "" && !strings.Contains(priorSegments, filePath) {
			if data, err := os.ReadFile(filePath); err == nil {
				content := string(data)
				if mErr := CheckMultipleChangeIDs(content, filePath); mErr != nil {
					return formatChangeIDClobberBlockMessage(fmt.Sprintf("'git commit -F %s' with multiple Change-Id trailers (%v)", filePath, mErr))
				}
				if (hasAmend || sawHistoryResetOrSquash) && !changeIDLineRegex.MatchString(content) {
					return formatChangeIDClobberBlockMessage(fmt.Sprintf("'git commit --amend -F %s' because %s does not contain a 'Change-Id:' trailer", filePath, filePath))
				}
			}
		}
	}

	if hasAmend && hasReuse {
		trimmedRev := strings.TrimSpace(reuseRev)
		if trimmedRev != "" && trimmedRev != "HEAD" && trimmedRev != "@" {
			return formatChangeIDClobberBlockMessage(fmt.Sprintf("'git commit --amend -C %s' (replaces HEAD's Change-Id with %s's message)", trimmedRev, trimmedRev))
		}
	}

	return ""
}

func splitShellSegments(s string) []string {
	var segments []string
	var cur strings.Builder
	var quote byte
	escaped := false

	for i := 0; i < len(s); i++ {
		ch := s[i]
		if escaped {
			cur.WriteByte(ch)
			escaped = false
			continue
		}
		if ch == '\\' && quote != '\'' {
			cur.WriteByte(ch)
			escaped = true
			continue
		}
		if quote != 0 {
			if ch == quote {
				quote = 0
			}
			cur.WriteByte(ch)
			continue
		}
		if ch == '\'' || ch == '"' {
			quote = ch
			cur.WriteByte(ch)
			continue
		}
		if ch == ';' || ch == '|' || ch == '&' || ch == '\n' {
			if seg := strings.TrimSpace(cur.String()); seg != "" {
				segments = append(segments, seg)
			}
			cur.Reset()
			continue
		}
		cur.WriteByte(ch)
	}
	if seg := strings.TrimSpace(cur.String()); seg != "" {
		segments = append(segments, seg)
	}
	return segments
}

func tokenizeShellArgs(seg string) []string {
	var tokens []string
	var cur strings.Builder
	inToken := false
	var quote byte
	escaped := false

	for i := 0; i < len(seg); i++ {
		ch := seg[i]
		if escaped {
			if quote == '"' && ch != '"' && ch != '\\' && ch != '$' && ch != '`' {
				cur.WriteByte('\\')
			}
			cur.WriteByte(ch)
			inToken = true
			escaped = false
			continue
		}
		if ch == '\\' && quote != '\'' {
			escaped = true
			inToken = true
			continue
		}
		if quote != 0 {
			if ch == quote {
				quote = 0
			} else {
				cur.WriteByte(ch)
			}
			inToken = true
			continue
		}
		if ch == '\'' || ch == '"' {
			quote = ch
			inToken = true
			continue
		}
		if ch == ' ' || ch == '\t' || ch == '\r' {
			if inToken {
				tokens = append(tokens, cur.String())
				cur.Reset()
				inToken = false
			}
			continue
		}
		cur.WriteByte(ch)
		inToken = true
	}
	if inToken {
		tokens = append(tokens, cur.String())
	}
	return tokens
}

// ExtractAgentCommandAndHarness extracts the command string and harness identifier
// ("jetski", "claude", or "cursor") from a PreToolUse JSON payload.
func ExtractAgentCommandAndHarness(payload map[string]any) (string, string) {
	if toolCall, ok := payload["toolCall"].(map[string]any); ok {
		if args, ok := toolCall["args"].(map[string]any); ok {
			if cmd, ok := args["CommandLine"].(string); ok && cmd != "" {
				return cmd, "jetski"
			}
			if cmd, ok := args["command"].(string); ok && cmd != "" {
				return cmd, "jetski"
			}
		}
		return "", "jetski"
	}

	if eventName, _ := payload["hook_event_name"].(string); eventName == "beforeShellExecution" {
		cmd, _ := payload["command"].(string)
		return cmd, "cursor"
	}
	if cmd, ok := payload["command"].(string); ok {
		if _, hasToolInput := payload["tool_input"]; !hasToolInput {
			return cmd, "cursor"
		}
	}

	if toolInput, ok := payload["tool_input"].(map[string]any); ok {
		if cmd, ok := toolInput["command"].(string); ok && cmd != "" {
			return cmd, "claude"
		}
		if cmd, ok := toolInput["CommandLine"].(string); ok && cmd != "" {
			return cmd, "claude"
		}
		return "", "claude"
	}

	return "", "jetski"
}

// FormatPreToolUseResponse builds the harness-specific JSON response map.
func FormatPreToolUseResponse(reason, harness string) map[string]any {
	switch harness {
	case "cursor":
		if reason != "" {
			return map[string]any{
				"permission":    "deny",
				"user_message":  reason,
				"agent_message": reason,
			}
		}
		return map[string]any{"permission": "allow"}

	case "claude":
		if reason != "" {
			return map[string]any{
				"decision": "deny",
				"reason":   reason,
				"hookSpecificOutput": map[string]any{
					"hookEventName":            "PreToolUse",
					"permissionDecision":       "deny",
					"permissionDecisionReason": reason,
				},
			}
		}
		return map[string]any{}

	default:
		if reason != "" {
			return map[string]any{
				"decision": "deny",
				"reason":   reason,
			}
		}
		return map[string]any{
			"decision": "allow",
		}
	}
}

// EvaluatePreToolUsePayload parses a raw JSON payload and returns the serialized response JSON.
func EvaluatePreToolUsePayload(rawInput []byte) ([]byte, error) {
	if len(bytes.TrimSpace(rawInput)) == 0 {
		return json.Marshal(map[string]any{"decision": "allow"})
	}
	var payload map[string]any
	if err := json.Unmarshal(rawInput, &payload); err != nil || payload == nil {
		return json.Marshal(map[string]any{"decision": "allow"})
	}
	command, harness := ExtractAgentCommandAndHarness(payload)
	reason := CheckAgentCommand(command)
	return json.Marshal(FormatPreToolUseResponse(reason, harness))
}

// EnsureChangeIDInFile reads a commit message file and injects a Gerrit Change-Id trailer if missing.
func EnsureChangeIDInFile(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("failed to read commit message file %s: %w", path, err)
	}
	updated, changed := InjectChangeID(string(data))
	if !changed {
		return nil
	}
	if err := os.WriteFile(path, []byte(updated), 0644); err != nil {
		return fmt.Errorf("failed to write updated commit message file %s: %w", path, err)
	}
	return nil
}

// InjectChangeID inserts a Gerrit Change-Id trailer into msg if one is not already present.
func InjectChangeID(msg string) (string, bool) {
	// Strip comment lines (starting with '#') when checking for existing Change-Id or empty message.
	lines := strings.Split(strings.ReplaceAll(msg, "\r\n", "\n"), "\n")
	var nonCommentLines []string
	for _, line := range lines {
		if !strings.HasPrefix(strings.TrimSpace(line), "#") {
			nonCommentLines = append(nonCommentLines, line)
		}
	}
	cleanMsg := strings.TrimSpace(strings.Join(nonCommentLines, "\n"))
	if cleanMsg == "" {
		return msg, false
	}
	if changeIDLineRegex.MatchString(cleanMsg) {
		return msg, false
	}

	var randBytes [32]byte
	_, _ = rand.Read(randBytes[:])
	h := sha1.New()
	_, _ = fmt.Fprintf(h, "%s\n%d\n%x", cleanMsg, time.Now().UnixNano(), randBytes)
	changeIDLine := "Change-Id: I" + hex.EncodeToString(h.Sum(nil))

	var out []string
	inserted := false
	for _, line := range lines {
		if !inserted && signedOffByPrefixRegex.MatchString(line) {
			out = append(out, changeIDLine)
			inserted = true
		}
		out = append(out, line)
	}
	if !inserted {
		// Insert before trailing git comment block if present, else append at end.
		commentIdx := -1
		for i, line := range out {
			if strings.HasPrefix(strings.TrimSpace(line), "#") {
				commentIdx = i
				break
			}
		}
		if commentIdx >= 0 {
			prefix := out[:commentIdx]
			suffix := out[commentIdx:]
			for len(prefix) > 0 && strings.TrimSpace(prefix[len(prefix)-1]) == "" {
				prefix = prefix[:len(prefix)-1]
			}
			var combined []string
			combined = append(combined, prefix...)
			combined = append(combined, "", changeIDLine, "")
			combined = append(combined, suffix...)
			out = combined
		} else {
			for len(out) > 0 && strings.TrimSpace(out[len(out)-1]) == "" {
				out = out[:len(out)-1]
			}
			out = append(out, "", changeIDLine, "")
		}
	}
	return strings.Join(out, "\n"), true
}

func parseAgentTargets(raw string) ([]string, error) {
	val := strings.ToLower(strings.TrimSpace(raw))
	switch val {
	case "all":
		return []string{"jetski", "claude", "cursor"}, nil
	case "jetski", "antigravity":
		return []string{"jetski"}, nil
	case "claude":
		return []string{"claude"}, nil
	case "cursor":
		return []string{"cursor"}, nil
	default:
		return nil, fmt.Errorf(
			"invalid --agent target %q: must be one of 'jetski', 'claude', 'cursor', or 'all'\n\n"+
				"Examples:\n"+
				"  ./gh hook install --agent=all\n"+
				"  ./gh hook install --agent=jetski\n"+
				"  ./gh hook install --agent=claude\n"+
				"  ./gh hook status",
			raw,
		)
	}
}

func stagedHookBinaryPath(homeDir string) string {
	binName := "gh-ish"
	if runtime.GOOS == "windows" {
		binName = "gh-ish.exe"
	}
	return filepath.Join(homeDir, ".config", "pw_ghish", "bin", binName)
}

func stageHookBinary(homeDir string) (string, error) {
	srcPath, err := HookExecutablePath()
	if err != nil {
		return "", fmt.Errorf("failed to resolve current gh-ish executable path: %w", err)
	}
	dstPath := stagedHookBinaryPath(homeDir)
	if err := os.MkdirAll(filepath.Dir(dstPath), 0755); err != nil {
		return "", fmt.Errorf("failed to create directory %s: %w", filepath.Dir(dstPath), err)
	}
	if filepath.Clean(srcPath) != filepath.Clean(dstPath) {
		sf, err := os.Open(srcPath)
		if err != nil {
			return "", fmt.Errorf("failed to open executable %s: %w", srcPath, err)
		}
		defer sf.Close()

		tmpPath := dstPath + ".tmp"
		df, err := os.OpenFile(tmpPath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0755)
		if err != nil {
			return "", fmt.Errorf("failed to create staged binary %s: %w", tmpPath, err)
		}
		if _, err := io.Copy(df, sf); err != nil {
			_ = df.Close()
			_ = os.Remove(tmpPath)
			return "", fmt.Errorf("failed to copy staged binary %s: %w", tmpPath, err)
		}
		if err := df.Close(); err != nil {
			_ = os.Remove(tmpPath)
			return "", fmt.Errorf("failed to close staged binary %s: %w", tmpPath, err)
		}
		if err := os.Rename(tmpPath, dstPath); err != nil {
			_ = os.Remove(tmpPath)
			return "", fmt.Errorf("failed to finalize staged binary %s: %w", dstPath, err)
		}
	}
	return dstPath, nil
}

func agentConfigPath(homeDir, target string) string {
	switch target {
	case "jetski":
		return filepath.Join(homeDir, ".gemini", "config", "hooks.json")
	case "claude":
		return filepath.Join(homeDir, ".claude", "settings.json")
	case "cursor":
		return filepath.Join(homeDir, ".cursor", "hooks.json")
	default:
		return ""
	}
}

func isGhishHookCommand(cmd string) bool {
	return strings.Contains(cmd, "pre-tool-use") || strings.Contains(cmd, "ghish_tool_guard")
}

// removeGhishFromHookList removes any gh-ish hook entries from an agent hook slice,
// preserving all non-gh-ish hooks (both sibling entries and sibling commands within
// a shared matcher's "hooks" array).
func removeGhishFromHookList(list []any) ([]any, bool) {
	var remaining []any
	removed := false
	for _, item := range list {
		m, ok := item.(map[string]any)
		if !ok {
			remaining = append(remaining, item)
			continue
		}
		if cmd, ok := m["command"].(string); ok && isGhishHookCommand(cmd) {
			removed = true
			continue
		}
		if innerHooks, ok := m["hooks"].([]any); ok {
			var keptInner []any
			for _, h := range innerHooks {
				if hm, ok := h.(map[string]any); ok {
					if cmd, ok := hm["command"].(string); ok && isGhishHookCommand(cmd) {
						removed = true
						continue
					}
				}
				keptInner = append(keptInner, h)
			}
			if len(keptInner) == 0 && len(innerHooks) > 0 {
				continue
			}
			if len(keptInner) != len(innerHooks) {
				cloned := make(map[string]any, len(m))
				for k, v := range m {
					cloned[k] = v
				}
				cloned["hooks"] = keptInner
				remaining = append(remaining, cloned)
				continue
			}
		}
		remaining = append(remaining, item)
	}
	return remaining, removed
}

func installAgentHookConfig(homeDir, target, hookCommand string) (string, error) {
	cfgPath := agentConfigPath(homeDir, target)
	if err := os.MkdirAll(filepath.Dir(cfgPath), 0755); err != nil {
		return "", fmt.Errorf("failed to create config directory for %s: %w", cfgPath, err)
	}

	root := make(map[string]any)
	if existing, err := os.ReadFile(cfgPath); err == nil && len(bytes.TrimSpace(existing)) > 0 {
		if err := json.Unmarshal(existing, &root); err != nil {
			return "", fmt.Errorf("failed to parse existing %s: %w", cfgPath, err)
		}
	}

	hooksMap, _ := root["hooks"].(map[string]any)
	if hooksMap == nil {
		hooksMap = make(map[string]any)
	}

	var key string
	var newEntry map[string]any
	switch target {
	case "cursor":
		if _, ok := root["version"]; !ok {
			root["version"] = 1
		}
		key = "beforeShellExecution"
		newEntry = map[string]any{
			"command": hookCommand,
		}
	case "claude":
		key = "PreToolUse"
		newEntry = map[string]any{
			"matcher": "Bash",
			"hooks": []any{
				map[string]any{
					"type":    "command",
					"command": hookCommand,
				},
			},
		}
	case "jetski":
		key = "PreToolUse"
		newEntry = map[string]any{
			"matcher": "run_command",
			"hooks": []any{
				map[string]any{
					"type":    "command",
					"command": hookCommand,
				},
			},
		}
	}

	existingList, _ := hooksMap[key].([]any)
	cleaned, _ := removeGhishFromHookList(existingList)
	hooksMap[key] = append(cleaned, newEntry)

	root["hooks"] = hooksMap
	encoded, err := json.MarshalIndent(root, "", "  ")
	if err != nil {
		return "", fmt.Errorf("failed to encode %s: %w", cfgPath, err)
	}
	if err := os.WriteFile(cfgPath, append(encoded, '\n'), 0644); err != nil {
		return "", fmt.Errorf("failed to write %s: %w", cfgPath, err)
	}
	return cfgPath, nil
}

func uninstallAgentHookConfig(homeDir, target string) (string, bool, error) {
	cfgPath := agentConfigPath(homeDir, target)
	existing, err := os.ReadFile(cfgPath)
	if err != nil {
		if os.IsNotExist(err) {
			return cfgPath, false, nil
		}
		return cfgPath, false, fmt.Errorf("failed to read %s: %w", cfgPath, err)
	}
	var root map[string]any
	if err := json.Unmarshal(existing, &root); err != nil {
		return cfgPath, false, fmt.Errorf("failed to parse %s: %w", cfgPath, err)
	}
	hooksMap, ok := root["hooks"].(map[string]any)
	if !ok {
		return cfgPath, false, nil
	}
	key := "PreToolUse"
	if target == "cursor" {
		key = "beforeShellExecution"
	}
	existingList, ok := hooksMap[key].([]any)
	if !ok {
		return cfgPath, false, nil
	}
	remaining, removed := removeGhishFromHookList(existingList)
	if !removed {
		return cfgPath, false, nil
	}
	if len(remaining) == 0 {
		delete(hooksMap, key)
	} else {
		hooksMap[key] = remaining
	}
	if len(hooksMap) == 0 {
		delete(root, "hooks")
	} else {
		root["hooks"] = hooksMap
	}
	encoded, err := json.MarshalIndent(root, "", "  ")
	if err != nil {
		return cfgPath, false, err
	}
	if err := os.WriteFile(cfgPath, append(encoded, '\n'), 0644); err != nil {
		return cfgPath, false, err
	}
	return cfgPath, true, nil
}

func isAgentHookInstalled(homeDir, target string) bool {
	cfgPath := agentConfigPath(homeDir, target)
	data, err := os.ReadFile(cfgPath)
	if err != nil {
		return false
	}
	return isGhishHookCommand(string(data))
}

func resolveGitHooksDir(cmd *cobra.Command) (string, error) {
	cfg := GetConfig(cmd)
	if cfg == nil {
		return "", fmt.Errorf("internal error: command config is uninitialized")
	}
	var stdout, stderr bytes.Buffer
	if err := cfg.Git.Run(cmd.Context(), &stdout, &stderr, "rev-parse", "--git-common-dir"); err != nil {
		return "", fmt.Errorf("not inside a Git repository (git rev-parse --git-common-dir failed: %w)", err)
	}
	gitDir := strings.TrimSpace(stdout.String())
	if !filepath.IsAbs(gitDir) && cfg.CWD != "" {
		gitDir = filepath.Join(cfg.CWD, gitDir)
	}
	return filepath.Join(gitDir, "hooks"), nil
}

func runHookInstall(cmd *cobra.Command, args []string) error {
	if hookInstallAgentFlag == "" && !hookInstallGitFlag && !hookInstallBlockRawPushFlag {
		return fmt.Errorf(
			"missing required target flag for 'hook install': specify --agent, --git, and/or --block-raw-push\n\n" +
				"Examples:\n" +
				"  • Install pre-tool guard for all supported agents: ./gh hook install --agent\n" +
				"  • Install pre-tool guard for Antigravity/Jetski:   ./gh hook install --agent=jetski\n" +
				"  • Install Git commit-msg & pre-push guards:        ./gh hook install --git --block-raw-push\n" +
				"  • Check current hook installation status:          ./gh hook status",
		)
	}

	homeDir, err := HookUserHomeDir()
	if err != nil {
		return fmt.Errorf("failed to determine user home directory: %w", err)
	}
	stagedBin, err := stageHookBinary(homeDir)
	if err != nil {
		return err
	}
	out := cmd.OutOrStdout()
	_, _ = fmt.Fprintf(out, "Staged gh-ish hook binary: %s\n", stagedBin)

	if hookInstallAgentFlag != "" {
		targets, err := parseAgentTargets(hookInstallAgentFlag)
		if err != nil {
			return err
		}
		hookCmdStr := fmt.Sprintf("%q hook pre-tool-use", stagedBin)
		for _, target := range targets {
			cfgPath, err := installAgentHookConfig(homeDir, target, hookCmdStr)
			if err != nil {
				return err
			}
			_, _ = fmt.Fprintf(out, "Installed %s PreToolUse hook: %s\n", target, cfgPath)
		}
	}

	if hookInstallGitFlag || hookInstallBlockRawPushFlag {
		cfg := GetConfig(cmd)
		if cfg == nil {
			return fmt.Errorf("internal error: command config is uninitialized")
		}
		hooksDir, err := resolveGitHooksDir(cmd)
		if err != nil {
			return err
		}
		if err := os.MkdirAll(hooksDir, 0755); err != nil {
			return fmt.Errorf("failed to create Git hooks directory %s: %w", hooksDir, err)
		}
		slashBin := filepath.ToSlash(stagedBin)

		if hookInstallGitFlag {
			commitMsgPath := filepath.Join(hooksDir, "commit-msg")
			script := fmt.Sprintf("#!/bin/sh\nexec %q hook commit-msg \"$1\"\n", slashBin)
			if err := os.WriteFile(commitMsgPath, []byte(script), 0755); err != nil {
				return fmt.Errorf("failed to write commit-msg hook: %w", err)
			}
			_, _ = fmt.Fprintf(out, "Installed Git commit-msg hook: %s\n", commitMsgPath)
		}

		if hookInstallBlockRawPushFlag {
			var stdout, stderr bytes.Buffer
			if err := cfg.Git.Run(cmd.Context(), &stdout, &stderr, "config", "ghish.blockrawpush", "true"); err != nil {
				return fmt.Errorf("failed to set git config ghish.blockrawpush: %w", err)
			}
			prePushPath := filepath.Join(hooksDir, "pre-push")
			script := fmt.Sprintf("#!/bin/sh\nexec %q hook pre-push \"$@\"\n", slashBin)
			if err := os.WriteFile(prePushPath, []byte(script), 0755); err != nil {
				return fmt.Errorf("failed to write pre-push hook: %w", err)
			}
			_, _ = fmt.Fprintf(out, "Enabled ghish.blockrawpush and installed Git pre-push hook: %s\n", prePushPath)
		}
	}

	return nil
}

func runHookUninstall(cmd *cobra.Command, args []string) error {
	if hookUninstallAgentFlag == "" && !hookUninstallGitFlag {
		return fmt.Errorf(
			"missing required target flag for 'hook uninstall': specify --agent and/or --git\n\n" +
				"Examples:\n" +
				"  • Remove agent PreToolUse hooks: ./gh hook uninstall --agent\n" +
				"  • Remove Git pre-push guard:     ./gh hook uninstall --git\n" +
				"  • Check current status:          ./gh hook status",
		)
	}

	out := cmd.OutOrStdout()
	if hookUninstallAgentFlag != "" {
		homeDir, err := HookUserHomeDir()
		if err != nil {
			return fmt.Errorf("failed to determine user home directory: %w", err)
		}
		targets, err := parseAgentTargets(hookUninstallAgentFlag)
		if err != nil {
			return err
		}
		for _, target := range targets {
			cfgPath, removed, err := uninstallAgentHookConfig(homeDir, target)
			if err != nil {
				return err
			}
			if removed {
				_, _ = fmt.Fprintf(out, "Removed %s PreToolUse hook from %s\n", target, cfgPath)
			} else {
				_, _ = fmt.Fprintf(out, "No %s PreToolUse hook present in %s\n", target, cfgPath)
			}
		}
	}

	if hookUninstallGitFlag {
		cfg := GetConfig(cmd)
		if cfg == nil {
			return fmt.Errorf("internal error: command config is uninitialized")
		}
		var stdout, stderr bytes.Buffer
		_ = cfg.Git.Run(cmd.Context(), &stdout, &stderr, "config", "--unset", "ghish.blockrawpush")
		hooksDir, err := resolveGitHooksDir(cmd)
		if err != nil {
			return err
		}
		prePushPath := filepath.Join(hooksDir, "pre-push")
		if err := os.Remove(prePushPath); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("failed to remove %s: %w", prePushPath, err)
		}
		_, _ = fmt.Fprintf(out, "Disabled ghish.blockrawpush and removed %s\n", prePushPath)
	}

	return nil
}

func runHookStatus(cmd *cobra.Command, args []string) error {
	homeDir, err := HookUserHomeDir()
	if err != nil {
		return fmt.Errorf("failed to determine user home directory: %w", err)
	}
	out := cmd.OutOrStdout()
	stagedBin := stagedHookBinaryPath(homeDir)
	binStatus := "not staged"
	if _, err := os.Stat(stagedBin); err == nil {
		binStatus = "installed"
	}
	_, _ = fmt.Fprintf(out, "Staged Binary: %s (%s)\n\n", stagedBin, binStatus)

	_, _ = fmt.Fprintln(out, "Agent PreToolUse Hooks (user-level):")
	for _, target := range []string{"jetski", "claude", "cursor"} {
		status := "disabled"
		if isAgentHookInstalled(homeDir, target) {
			status = "enabled"
		}
		_, _ = fmt.Fprintf(out, "  %-8s %-10s (%s)\n", target+":", status, agentConfigPath(homeDir, target))
	}

	_, _ = fmt.Fprintln(out, "\nGit Repository Hooks:")
	hooksDir, err := resolveGitHooksDir(cmd)
	if err != nil {
		_, _ = fmt.Fprintln(out, "  (not inside a Git repository)")
		return nil
	}
	commitMsgStatus := "missing"
	if info, err := os.Stat(filepath.Join(hooksDir, "commit-msg")); err == nil && info.Mode()&0111 != 0 {
		commitMsgStatus = "installed"
	}
	prePushStatus := "disabled"
	if info, err := os.Stat(filepath.Join(hooksDir, "pre-push")); err == nil && info.Mode()&0111 != 0 {
		prePushStatus = "installed"
	}
	_, _ = fmt.Fprintf(out, "  commit-msg: %s\n", commitMsgStatus)
	_, _ = fmt.Fprintf(out, "  pre-push:   %s\n", prePushStatus)
	return nil
}
