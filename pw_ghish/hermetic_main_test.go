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

//go:build !live

package pw_ghish

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

// TestMain runs every unit test with fail-closed credentials and network so a
// test that forgets to mock auth can never run luci-auth, gcloud, sso_client,
// gob-curl, bb, or the Busytown accelerators, read the developer's tokens, or
// reach a real server. Tests that need credentials install mocks with
// WithHermeticAuth. The live suite (-tags=live) has no TestMain and uses real
// credentials.
func TestMain(m *testing.M) {
	for _, v := range hermeticAuthEnvVars {
		os.Unsetenv(v)
	}
	// Clear coding-agent variables so that --agent's auto-detection behaves
	// the same whether or not tests run under an agent.
	for _, v := range agentEnvVars {
		os.Unsetenv(v)
	}
	// Ignore the developer's global and system git config so auth-mode
	// auto-detection never sees their user.email.
	os.Setenv("GIT_CONFIG_GLOBAL", os.DevNull)
	os.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	allowLoopbackGerritAuthForTests = true
	failClosed := failClosedAuthResolvers()
	SetAuthResolvers(failClosed)
	// `bb` and the Busytown accelerators (fetch_artifact, ants_cli) carry the
	// developer's LUCI and LOAS credentials, so tests must stub them.
	SetRerunExecForTesting(failClosed.LookPath, func(ctx context.Context, name string, args []string, stdout, stderr io.Writer) error {
		return fmt.Errorf("hermetic test: unexpected command %s", name)
	})
	BusytownFileExistsFn = func(string) bool { return false }
	BusytownExecCommandFn = func(ctx context.Context, name string, args ...string) ([]byte, error) {
		return failClosed.CommandRunner(ctx, name, args...)
	}
	http.DefaultTransport = &loopbackOnlyTransport{
		inner:       http.DefaultTransport,
		logRefusals: true,
	}
	os.Exit(m.Run())
}

func TestHermeticDefaults_FailClosed(t *testing.T) {
	prev := currentAuthResolvers()
	t.Cleanup(func() { SetAuthResolvers(prev) })
	ResetAuthTokenCaches()
	t.Cleanup(ResetAuthTokenCaches)
	ctx := t.Context()

	for _, tc := range []struct {
		name string
		fn   func() error
	}{
		{"LookPathFn", func() error { _, err := LookPathFn("luci-auth"); return err }},
		{"OAuthTokenCommandRunner", func() error { _, err := OAuthTokenCommandRunner(ctx, "luci-auth", "token"); return err }},
		{"DefaultGerritToken", func() error { _, _, err := DefaultGerritToken(ctx); return err }},
		{"LUCITokenResolver", func() error { _, _, err := LUCITokenResolver(ctx); return err }},
		{"AndroidBuildTokenResolver", func() error { _, err := AndroidBuildTokenResolver(ctx); return err }},
		{"IssueTrackerTokenResolver", func() error { _, _, err := IssueTrackerTokenResolver(ctx); return err }},
		{"DefaultRerunCheck", func() error { return DefaultRerunCheck(ctx, "bb add foo", "", io.Discard, io.Discard) }},
		{"BusytownExecCommandFn", func() error { _, err := BusytownExecCommandFn(ctx, AntsCLIBinPath); return err }},
	} {
		if err := tc.fn(); err == nil {
			t.Errorf("%s succeeded, want fail-closed error", tc.name)
		}
	}
	if BusytownFileExistsFn(FetchArtifactBinPath) {
		t.Errorf("BusytownFileExistsFn(%q) = true, want false", FetchArtifactBinPath)
	}

	t.Setenv("GERRIT_TOKEN", "x")
	t.Setenv("LUCI_TOKEN", "l")
	t.Setenv("GHISH_ANDROID_BUILD_TOKEN", "a")
	t.Setenv("BUGANIZER_TOKEN", "b")
	ResetAuthTokenCaches()
	if tok, src, err := DefaultGerritToken(ctx); err != nil || tok != "x" || src != "GERRIT_TOKEN" {
		t.Errorf("DefaultGerritToken() = (%q, %q, %v), want (\"x\", \"GERRIT_TOKEN\", nil)", tok, src, err)
	}
	if tok, src, err := LUCITokenResolver(ctx); err != nil || tok != "l" || src != "LUCI_TOKEN" {
		t.Errorf("LUCITokenResolver() = (%q, %q, %v), want (\"l\", \"LUCI_TOKEN\", nil)", tok, src, err)
	}
	if tok, err := AndroidBuildTokenResolver(ctx); err != nil || tok != "a" {
		t.Errorf("AndroidBuildTokenResolver() = (%q, %v), want (\"a\", nil)", tok, err)
	}
	if tok, src, err := IssueTrackerTokenResolver(ctx); err != nil || tok != "b" || src != "BUGANIZER_TOKEN" {
		t.Errorf("IssueTrackerTokenResolver() = (%q, %q, %v), want (\"b\", \"BUGANIZER_TOKEN\", nil)", tok, src, err)
	}

	guard, _ := http.DefaultTransport.(*loopbackOnlyTransport)
	if guard != nil {
		_ = guard.takeRefused()
	}
	if resp, err := http.Post("https://pigweed-review.googlesource.com/", "text/plain", strings.NewReader("body")); err == nil || !strings.Contains(err.Error(), "refusing non-loopback") {
		if resp != nil {
			resp.Body.Close()
		}
		t.Errorf("non-loopback request error = %v, want refusal", err)
	}
	if guard != nil {
		if refused := guard.takeRefused(); len(refused) != 1 || refused[0] != "POST https://pigweed-review.googlesource.com/" {
			t.Errorf("guard.takeRefused() = %v, want [POST https://pigweed-review.googlesource.com/]", refused)
		}
	}

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/redirect-external" {
			http.Redirect(w, r, "https://example.com/leaked", http.StatusFound)
			return
		}
		io.WriteString(w, "ok")
	}))
	defer srv.Close()
	if resp, err := http.DefaultClient.Get(srv.URL + "/redirect-external"); err == nil || !strings.Contains(err.Error(), "refusing non-loopback") {
		if resp != nil {
			resp.Body.Close()
		}
		t.Errorf("redirect to external host error = %v, want refusal", err)
	}
	localhostURL := strings.Replace(srv.URL, "127.0.0.1", "localhost", 1)
	for _, targetURL := range []string{srv.URL, localhostURL} {
		resp, err := http.DefaultClient.Get(targetURL)
		if err != nil {
			t.Fatalf("loopback request to %s failed: %v", targetURL, err)
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if string(body) != "ok" {
			t.Errorf("loopback response from %s = %q, want %q", targetURL, body, "ok")
		}
	}
}
