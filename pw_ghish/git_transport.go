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
	"cmp"
	"context"
	"fmt"
	"io"
	"net/url"
	"strings"
)

// Gerrit Git transport fallback:
//
//	initial git push / fetch (GIT_TERMINAL_PROMPT=0)
//	  |-- succeeds or non-auth failure ----------> return directly
//	  +-- transport auth failure on Gerrit remote:
//	        1. preferredCorpGitSchemes() (googler mode without GERRIT_TOKEN):
//	             - rpc://<tenant>/<project>   (LOAS Linux; 7-day gcert LOAS2)
//	             - sso://<tenant>/<project>   (Corp SSO helper; 20-hour ticket)
//	        2. https://<tenant>.googlesource.com/ + OAuth2 Bearer
//	             (skipped on corp-only hosts)

func writeGitFallbackNote(w io.Writer, failedScheme, initialStderr string, forPush bool, baseURL, credDesc string) {
	if w == nil {
		return
	}
	pastVerb, why := "fetched", "authentication failed"
	if forPush {
		pastVerb = "pushed"
	}
	if strings.Contains(strings.ToLower(initialStderr), "expired") {
		why = "credentials expired"
	}
	fmt.Fprintf(w, "Note: %s:// %s; %s via %s using %s.\n", cmp.Or(failedScheme, "git"), why, pastVerb, baseURL, credDesc)
}

func runGitWithAuthFallback(
	ctx context.Context,
	gc GitClient,
	cfg *Config,
	remote string,
	args []string,
	noteW io.Writer,
) (outBuf, errBuf bytes.Buffer, err error) {
	verb := args[0]
	forPush := verb == "push"
	baseCtx := withGitEnv(ctx, "GIT_TERMINAL_PROMPT=0")
	origErr := gc.Run(baseCtx, &outBuf, &errBuf, args...)
	if origErr == nil || ctx.Err() != nil {
		return outBuf, errBuf, origErr
	}
	initialStderr := errBuf.String()
	if _, isAuth := classifyGitTransportAuthError(initialStderr, ""); !isAuth {
		return outBuf, errBuf, origErr
	}
	info, ok := resolveGerritGitRemote(ctx, gc, cfg, remote, forPush)
	if !ok {
		return outBuf, errBuf, origErr
	}
	mode, _, authErr := ResolveAuthMode(ctx, cfg)
	if authErr != nil {
		return outBuf, errBuf, authErr
	}
	httpsBase := gerritGitURL("https", info.tenant, "")
	if mode == AuthModeNone {
		return outBuf, errBuf, NewExitCodeError(ExitCodeAuth,
			"git %s for %s failed authentication. Gerrit authentication is disabled (auth mode none), "+
				"so it was not retried with Gerrit credentials.\n"+
				"Check your git credentials for %s.\n\nUnderlying error: %w",
			verb, httpsBase, httpsBase, origErr)
	}
	explicitTok, tokErr := gerritTokenFromEnv()
	if tokErr != nil {
		return outBuf, errBuf, tokErr
	}

	failedScheme, _ := classifyGitTransportAuthError(initialStderr, info.scheme)
	allowCorpGit := mode == AuthModeGoogler && explicitTok == ""
	if hasCorpOnlyIPRestrictionText(initialStderr) {
		if !allowCorpGit {
			return outBuf, errBuf, gitIPRestrictedAuthError(info.canonHost, verb, mode, origErr)
		}
		defaultHostCache.MarkCorpOnly(info.canonHost)
	}
	if allowCorpGit {
		for _, scheme := range preferredCorpGitSchemes() {
			if scheme == failedScheme {
				continue
			}
			candCtx := withGitConfigEnv(baseCtx, remoteSchemeOverrideGitConfig(remote, scheme, info.tenant, info.project)...)
			var candOut, candErr bytes.Buffer
			runErr := gc.Run(candCtx, &candOut, &candErr, args...)
			if runErr == nil {
				cred := map[string]string{"rpc": "LOAS2", "sso": "Corp SSO"}[scheme]
				writeGitFallbackNote(noteW, failedScheme, initialStderr, forPush, gerritGitURL(scheme, info.tenant, ""), cred)
				return candOut, candErr, nil
			}
			if _, isAuth := classifyGitTransportAuthError(candErr.String(), scheme); ctx.Err() != nil || !isAuth {
				return candOut, candErr, runErr
			}
		}
		if defaultHostCache.IsCorpOnly(info.canonHost) {
			return outBuf, errBuf, gitTransportAuthError(info.canonHost, verb, failedScheme, mode, allowCorpGit, origErr)
		}
	}

	authCtx := ctx
	if authCtx.Value(configKey) != cfg {
		authCtx = context.WithValue(authCtx, configKey, cfg)
	}
	res, authErr := resolveGerritAuth(authCtx, httpsBase, mode)
	if authErr != nil {
		return outBuf, errBuf, authErr
	}
	if res.unauthed || res.token == "" {
		if res.cause != nil {
			origErr = fmt.Errorf("%w (Gerrit OAuth2 token probe: %v)", origErr, res.cause)
		}
		return outBuf, errBuf, gitTransportAuthError(info.canonHost, verb, failedScheme, mode, allowCorpGit, origErr)
	}

	kv := append(
		remoteSchemeOverrideGitConfig(remote, "https", info.tenant, info.project),
		fmt.Sprintf("http.%s.extraHeader", httpsBase), "Authorization: Bearer "+res.token,
	)
	var httpsOut, httpsBuf bytes.Buffer
	httpsErr := gc.Run(withGitConfigEnv(baseCtx, kv...), &httpsOut, &httpsBuf, args...)
	if httpsErr == nil {
		if failedScheme != "https" {
			writeGitFallbackNote(noteW, failedScheme, initialStderr, forPush, httpsBase, res.desc)
		}
		return httpsOut, httpsBuf, nil
	}
	httpsStderr := httpsBuf.String()
	if scheme, isAuth := classifyGitTransportAuthError(httpsStderr, "https"); ctx.Err() != nil || !isAuth || scheme != "https" {
		return httpsOut, httpsBuf, httpsErr
	}
	if hasCorpOnlyIPRestrictionText(httpsStderr) {
		if !allowCorpGit {
			return httpsOut, httpsBuf, gitIPRestrictedAuthError(info.canonHost, verb, mode, httpsErr)
		}
		defaultHostCache.MarkCorpOnly(info.canonHost)
		return httpsOut, httpsBuf, gitTransportAuthError(info.canonHost, verb, failedScheme, mode, allowCorpGit, httpsErr)
	}
	return httpsOut, httpsBuf, NewExitCodeError(ExitCodeAuth,
		"git %s for %s was retried with a Gerrit OAuth2 token (%s), which was also rejected.\n"+
			"Check your credentials with: gh auth status\n\nUnderlying error: %w",
		verb, httpsBase, res.desc, httpsErr)
}

func findGitRemoteSSO() string {
	return lookPathFirst(
		"git-remote-sso",
		"/usr/lib/git-core/git-remote-sso",
		"/usr/local/git/git-google/bin/git-remote-sso",
		"/usr/local/bin/git-remote-sso",
		"/google/bin/releases/git-remote-sso/git-remote-sso",
	)
}

// preferredCorpGitSchemes returns the ordered corp Git remote-helper schemes
// available on the current host: "rpc" on LOAS Linux with git-remote-rpc,
// followed by "sso" when git-remote-sso is installed.
func preferredCorpGitSchemes() []string {
	var schemes []string
	if isLOASLinux() {
		if p, err := LookPathFn("git-remote-rpc"); err == nil && p != "" {
			schemes = append(schemes, "rpc")
		}
	}
	if findGitRemoteSSO() != "" {
		schemes = append(schemes, "sso")
	}
	return schemes
}

type gerritGitRemote struct {
	scheme    string
	tenant    string
	project   string
	canonHost string
}

func parseGerritGitRemote(raw string) (gerritGitRemote, bool) {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || u.Hostname() == "" {
		return gerritGitRemote{}, false
	}
	switch u.Scheme {
	case "sso", "rpc", "https":
	default:
		return gerritGitRemote{}, false
	}
	canonHost := CanonicalGerritHost(raw)
	tenant, ok := strings.CutSuffix(canonHost, "-review.googlesource.com")
	if !ok || tenant == "" || strings.Contains(tenant, ".") {
		return gerritGitRemote{}, false
	}
	return gerritGitRemote{
		scheme:    u.Scheme,
		tenant:    tenant,
		project:   GerritProjectFromRemote(raw),
		canonHost: canonHost,
	}, true
}

func configRemoteURL(ctx context.Context, gc GitClient, key string) string {
	val, err := gc.ConfigGet(ctx, key)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(val)
}

func resolveGerritGitRemote(ctx context.Context, gc GitClient, cfg *Config, remote string, forPush bool) (gerritGitRemote, bool) {
	remote = strings.TrimSpace(remote)
	if remote == "" {
		return gerritGitRemote{}, false
	}
	info, ok := parseGerritGitRemote(remote)
	if isValidRemoteName(remote) {
		keys := []string{"remote." + remote + ".url", "remote." + remote + ".pushurl"}
		if forPush {
			keys = []string{keys[1], keys[0]}
		}
		for _, key := range keys {
			cand, candOK := parseGerritGitRemote(configRemoteURL(ctx, gc, key))
			if !candOK {
				continue
			}
			if !ok {
				info, ok = cand, true
			} else if info.project == "" && cand.tenant == info.tenant {
				// Inherit project when pushurl is bare-tenant (e.g. "sso://pigweed/").
				info.project = cand.project
			}
			if info.project != "" {
				break
			}
		}
	}
	if ok && info.project == "" {
		if projCfg := cfg.projectConfigIfPresent(ctx); projCfg != nil {
			info.project = strings.Trim(strings.TrimSpace(projCfg.Gerrit.Project), "/")
		}
	}
	return info, ok && info.project != ""
}

func gerritGitURL(scheme, tenant, project string) string {
	var host string
	switch scheme {
	case "rpc", "sso":
		host = tenant
	case "https":
		host = tenant + ".googlesource.com"
	default:
		return ""
	}
	return (&url.URL{Scheme: scheme, Host: host, Path: "/" + strings.TrimPrefix(project, "/")}).String()
}

// remoteSchemeOverrideGitConfig returns per-invocation GIT_CONFIG_COUNT pairs
// that steer remote to targetScheme ("rpc", "sso", or "https") and outrank
// tenant-level insteadOf/pushInsteadOf rules in ~/.gitconfig via longer
// full-repository prefix rules.
func remoteSchemeOverrideGitConfig(remote, targetScheme, tenant, project string) []string {
	targetFullURL := gerritGitURL(targetScheme, tenant, project)
	if !isValidRemoteName(remote) {
		return []string{
			fmt.Sprintf("url.%s.insteadOf", targetFullURL), remote,
			fmt.Sprintf("url.%s.pushInsteadOf", targetFullURL), remote,
		}
	}
	// Empty values clear multi-valued url/pushurl entries from .git/config (Git 2.29+),
	// and mapping targetFullURL to itself wins Git's longest-prefix match over shorter
	// tenant-level insteadOf/pushInsteadOf rules in ~/.gitconfig.
	return []string{
		fmt.Sprintf("remote.%s.url", remote), "",
		fmt.Sprintf("remote.%s.url", remote), targetFullURL,
		fmt.Sprintf("remote.%s.pushurl", remote), "",
		fmt.Sprintf("remote.%s.pushurl", remote), targetFullURL,
		fmt.Sprintf("url.%s.insteadOf", targetFullURL), targetFullURL,
		fmt.Sprintf("url.%s.pushInsteadOf", targetFullURL), targetFullURL,
	}
}

// isTerminalPushRejection reports whether git push stderr shows Gerrit
// rejecting the push after transport authentication succeeded (e.g., missing
// Change-Id, no new changes, unsigned CLA, or ref/permission rejection).
func isTerminalPushRejection(stderr string) bool {
	lower := strings.ToLower(stderr)
	return strings.Contains(stderr, "missing Change-Id") ||
		strings.Contains(stderr, "no new changes") ||
		strings.Contains(stderr, "! [remote rejected]") ||
		strings.Contains(stderr, "! [rejected]") ||
		strings.Contains(lower, "prohibited by gerrit") ||
		strings.Contains(lower, "contributor license agreement") ||
		strings.Contains(stderr, "cla.developers.google.com")
}

func classifyGitTransportAuthError(stderr, hintScheme string) (string, bool) {
	if isTerminalPushRejection(stderr) {
		return "", false
	}
	if hasCorpOnlyIPRestrictionText(stderr) {
		return "https", true
	}
	lower := strings.ToLower(stderr)
	switch {
	case isGitRPCAuthError(lower):
		return "rpc", true
	case isGitSSOAuthError(lower):
		return "sso", true
	case strings.Contains(lower, "try running gcert"):
		if hintScheme == "rpc" {
			return "rpc", true
		}
		return "sso", true
	case isGitHTTPSAuthError(lower):
		return "https", true
	default:
		return "", false
	}
}

func isGitSSOAuthError(lower string) bool {
	return strings.Contains(lower, "sso: credentials expired") ||
		strings.Contains(lower, "remote helper 'sso' aborted session") ||
		strings.Contains(lower, "unable to get sso ticket") ||
		strings.Contains(lower, "'remote-sso' is not a git command") ||
		strings.Contains(lower, "unable to find remote helper for 'sso'")
}

func isGitRPCAuthError(lower string) bool {
	if strings.Contains(lower, "remote helper 'rpc' aborted session") ||
		strings.Contains(lower, "'remote-rpc' is not a git command") ||
		strings.Contains(lower, "unable to find remote helper for 'rpc'") ||
		strings.Contains(lower, "no loas") {
		return true
	}
	s := strings.ReplaceAll(lower, "rpc://", "")
	hasRPC := strings.Contains(s, "rpc:") || strings.Contains(s, "git-remote-rpc") || strings.Contains(s, "loas")
	hasAuth := strings.Contains(s, "expired") || strings.Contains(s, "gcert") ||
		strings.Contains(s, "unauthenticated") || strings.Contains(s, "credentials")
	return hasRPC && hasAuth
}

func isGitHTTPSAuthError(lower string) bool {
	return strings.Contains(lower, "could not read username") ||
		strings.Contains(lower, "authentication failed") ||
		strings.Contains(lower, "terminal prompts disabled") ||
		strings.Contains(lower, "http basic: access denied") ||
		strings.Contains(lower, "the requested url returned error: 401") ||
		strings.Contains(lower, "the requested url returned error: 403")
}

func gitIPRestrictedAuthError(canonHost, verb string, mode AuthMode, cause error) error {
	steps := []string{
		"Corporate network and SSO access are required to access " + canonHost + ".",
		"Switch to Googler mode on a managed corp machine:\n     export GH_ISH_AUTH_MODE=googler",
	}
	if mode == AuthModeGoogler {
		steps = []string{"unset GERRIT_TOKEN", "gcert"}
	}
	return formatGitTransportAuthError(
		fmt.Sprintf("git %s for %s failed authentication due to corporate IP/SSO restrictions.", verb, canonHost),
		steps, canonHost, cause)
}

func gitTransportAuthError(canonHost, verb, failedScheme string, mode AuthMode, allowCorpGit bool, cause error) error {
	if allowCorpGit && defaultHostCache.IsCorpOnly(canonHost) {
		return formatGitTransportAuthError(
			fmt.Sprintf("git %s for corp-only Gerrit host %s failed authentication (requires sso:// or rpc://).", verb, canonHost),
			[]string{"gcert"}, canonHost, cause)
	}
	var steps []string
	if allowCorpGit && (hasUsableSSOCredHelper() || len(preferredCorpGitSchemes()) > 0) {
		steps = append(steps, "gcert")
		if !hasUsableSSOCredHelper() {
			steps = append(steps, luciAuthLoginCommand(mode))
		}
	} else {
		steps = append(steps, luciAuthLoginCommand(mode))
	}
	steps = append(steps, `export GERRIT_TOKEN="<token>"`)
	return formatGitTransportAuthError(
		fmt.Sprintf("git %s for %s (%s://) failed authentication, and no fallback Gerrit credentials succeeded.",
			verb, canonHost, cmp.Or(failedScheme, "https")),
		steps, canonHost, cause)
}

func formatGitTransportAuthError(headline string, steps []string, canonHost string, cause error) error {
	steps = append(steps, "gh auth status --host "+canonHost)
	return NewExitCodeError(ExitCodeAuth, "%s\n\nRemediation:\n%s\n\nUnderlying error: %w",
		headline, formatGerritRemediationSteps(steps...), cause)
}
