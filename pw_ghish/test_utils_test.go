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
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path"
	"strings"
	"sync"
	"testing"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
	"github.com/spf13/pflag"
)

func resetAllFlags(cmd *cobra.Command) {
	cmd.Flags().VisitAll(func(f *pflag.Flag) {
		if f.Name == "host" || f.Name == "profile" {
			return
		}
		if s, ok := f.Value.(pflag.SliceValue); ok {
			_ = s.Replace(nil)
		} else {
			_ = f.Value.Set(f.DefValue)
		}
		f.Changed = false
	})
	cmd.SetContext(context.Background())
	for _, child := range cmd.Commands() {
		resetAllFlags(child)
	}
}

func init() {
	setupRootAliases()
}

// executeCommand runs a cobra command and captures stdout.
func executeCommand(root *cobra.Command, args ...string) (string, error) {
	setupRootAliases()
	resetAllFlags(root)

	r, w, _ := os.Pipe()
	oldStdout := os.Stdout
	os.Stdout = w

	oldNewGerritClient := NewGerritClient
	NewGerritClient = func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		host, _ := cmd.Flags().GetString("host")
		if host == "" {
			host = HostFlag
		}
		if host == "" {
			host = "https://fuchsia-review.googlesource.com"
		}
		if !strings.HasPrefix(host, "http://") && !strings.HasPrefix(host, "https://") {
			host = "https://" + host
		}
		return gerrit.NewClient(ctx, host, http.DefaultClient)
	}
	defer func() { NewGerritClient = oldNewGerritClient }()

	root.SetOut(w)
	root.SetErr(w)
	root.SetArgs(NormalizeCQArgs(args))

	var buf bytes.Buffer
	readDone := make(chan struct{})
	go func() {
		_, _ = buf.ReadFrom(r)
		close(readDone)
	}()

	err := root.Execute()

	_ = w.Close()
	os.Stdout = oldStdout
	<-readDone
	_ = r.Close()

	return buf.String(), err
}

// MockGitRunner implements GitRunner for testing.
type MockGitRunner struct {
	mu    sync.Mutex
	Calls []string
	RunFn func(ctx context.Context, stdout, stderr io.Writer, args ...string) error
}

func (m *MockGitRunner) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	m.mu.Lock()
	m.Calls = append(m.Calls, strings.Join(args, " "))
	m.mu.Unlock()
	if m.RunFn != nil {
		return m.RunFn(ctx, stdout, stderr, args...)
	}
	return nil
}

func (m *MockGitRunner) LastCall() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	if len(m.Calls) == 0 {
		return ""
	}
	return m.Calls[len(m.Calls)-1]
}

func (m *MockGitRunner) HasCall(substr string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, call := range m.Calls {
		if strings.Contains(call, substr) {
			return true
		}
	}
	return false
}

// SetMockGit configures DefaultGitRunner for the duration of the test.
func SetMockGit(t *testing.T, runner *MockGitRunner) *MockGitRunner {
	t.Helper()
	if runner == nil {
		runner = &MockGitRunner{}
	}
	oldGit := DefaultGitRunner
	DefaultGitRunner = runner
	t.Cleanup(func() { DefaultGitRunner = oldGit })
	return runner
}

type defaultGitRunnerWrapper struct {
	inner GitRunner
}

func (w *defaultGitRunnerWrapper) Run(ctx context.Context, stdout, stderr io.Writer, args ...string) error {
	var buf bytes.Buffer
	outWriter := stdout
	if len(args) >= 2 && args[0] == "log" && args[1] == "-1" {
		outWriter = io.MultiWriter(stdout, &buf)
	}
	err := w.inner.Run(ctx, outWriter, stderr, args...)
	if err == nil && len(args) >= 2 && args[0] == "log" && args[1] == "-1" && buf.Len() == 0 {
		stdout.Write([]byte("Default commit subject\n\nChange-Id: I0000000000000000000000000000000000000001\n"))
	}
	return err
}

// SetupMockConfig wraps gitRunner to provide default commit info on git log -1 and configures RootCmd.PersistentPreRun, restoring everything on test cleanup.
func SetupMockConfig(t *testing.T, gitRunner GitRunner) {
	t.Helper()
	oldGit := DefaultGitRunner
	DefaultGitRunner = &defaultGitRunnerWrapper{inner: gitRunner}
	t.Cleanup(func() { DefaultGitRunner = oldGit })

	oldPreRun := RootCmd.PersistentPreRun
	RootCmd.PersistentPreRun = func(cmd *cobra.Command, args []string) {
		SetConfig(cmd, &Config{
			Host: HostFlag,
			Git:  DefaultGitRunner,
		})
	}
	t.Cleanup(func() { RootCmd.PersistentPreRun = oldPreRun })
}

// mockTransport allows tests to customize RoundTrip behavior.
type mockTransport struct {
	roundTrip func(*http.Request) (*http.Response, error)
}

func (m *mockTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	return m.roundTrip(req)
}

// SetMockGerritClient overrides NewGerritClient for the duration of the test.
func SetMockGerritClient(t *testing.T, fn func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error)) {
	t.Helper()
	orig := NewGerritClient
	NewGerritClient = fn
	t.Cleanup(func() { NewGerritClient = orig })
}

// SetTestProfile sets ProfileFlag for the duration of the test and restores it on cleanup.
func SetTestProfile(t *testing.T, profile string) {
	t.Helper()
	oldProfile := ProfileFlag
	ProfileFlag = profile
	t.Cleanup(func() { ProfileFlag = oldProfile })
}

// MockRequest records an incoming HTTP request for test assertions.
type MockRequest struct {
	Method string
	URL    *url.URL
	Path   string
	Body   []byte
	Header http.Header
}

// MockRoute defines matching criteria and handler for a route.
type MockRoute struct {
	Method  string // empty matches any method
	Path    string // exact path or prefix (if ends in *)
	Handler http.HandlerFunc
}

// MockGerritServer manages an httptest.Server configured to simulate Gerrit REST responses.
type MockGerritServer struct {
	Server         *httptest.Server
	URL            string
	fallbackStatus int
	fallbackBody   []byte
	mu             sync.Mutex
	routes         []MockRoute
	requests       []MockRequest
	t              *testing.T
}

// NewMockGerritServer creates and starts a mock Gerrit server.
// It automatically configures HostFlag and NewGerritClient to point to this server
// and restores them on test cleanup.
func NewMockGerritServer(t *testing.T) *MockGerritServer {
	t.Helper()
	s := &MockGerritServer{
		t:              t,
		fallbackStatus: http.StatusNotFound,
	}
	s.Server = httptest.NewServer(http.HandlerFunc(s.handleHTTP))
	s.URL = s.Server.URL
	t.Cleanup(s.Server.Close)

	oldHost := HostFlag
	HostFlag = s.URL
	t.Cleanup(func() { HostFlag = oldHost })

	origClient := NewGerritClient
	NewGerritClient = func(ctx context.Context, cmd *cobra.Command) (*gerrit.Client, error) {
		return gerrit.NewClient(ctx, s.URL, s.Server.Client())
	}
	t.Cleanup(func() { NewGerritClient = origClient })

	return s
}

// Client returns a gerrit.Client pointed to this mock server.
func (s *MockGerritServer) Client() *gerrit.Client {
	c, _ := gerrit.NewClient(context.Background(), s.URL, s.Server.Client())
	return c
}

func (s *MockGerritServer) handleHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	body, _ := io.ReadAll(r.Body)
	s.requests = append(s.requests, MockRequest{
		Method: r.Method,
		URL:    r.URL,
		Path:   r.URL.Path,
		Body:   body,
		Header: r.Header.Clone(),
	})

	// Match route (in registered order)
	var matchedHandler http.HandlerFunc
	for _, route := range s.routes {
		if route.Method != "" && route.Method != r.Method {
			continue
		}
		matchGlob, _ := path.Match(route.Path, r.URL.Path)
		if route.Path == r.URL.Path || matchGlob || (strings.HasSuffix(route.Path, "*") && strings.HasPrefix(r.URL.Path, strings.TrimSuffix(route.Path, "*"))) {
			matchedHandler = route.Handler
			break
		}
	}
	fallbackStatus := s.fallbackStatus
	fallbackBody := s.fallbackBody
	s.mu.Unlock()

	if matchedHandler != nil {
		r.Body = io.NopCloser(bytes.NewReader(body))
		matchedHandler(w, r)
		return
	}

	w.WriteHeader(fallbackStatus)
	if len(fallbackBody) > 0 {
		w.Write(fallbackBody)
	}
}

// On registers a custom handler for a method and path.
func (s *MockGerritServer) On(method, path string, handler http.HandlerFunc) *MockGerritServer {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.routes = append(s.routes, MockRoute{
		Method:  method,
		Path:    path,
		Handler: handler,
	})
	return s
}

// RespondJSON writes Gerrit's magic prefix and JSON payload to w.
func (s *MockGerritServer) RespondJSON(w http.ResponseWriter, status int, payload any) {
	var jsonBytes []byte
	switch v := payload.(type) {
	case string:
		jsonBytes = []byte(v)
	case []byte:
		jsonBytes = v
	default:
		var err error
		jsonBytes, err = json.Marshal(payload)
		if err != nil {
			s.t.Fatalf("MockGerritServer.RespondJSON: failed to marshal JSON: %v", err)
		}
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write([]byte(")]}'\n"))
	w.Write(jsonBytes)
}

// OnJSON registers a route returning a JSON payload with Gerrit's mandatory ")]}'\n" prefix.
func (s *MockGerritServer) OnJSON(method, path string, status int, payload any) *MockGerritServer {
	return s.On(method, path, func(w http.ResponseWriter, r *http.Request) {
		s.RespondJSON(w, status, payload)
	})
}

// OnString registers a route returning plain string content with the given content type.
func (s *MockGerritServer) OnString(method, path string, status int, contentType, body string) *MockGerritServer {
	return s.On(method, path, func(w http.ResponseWriter, r *http.Request) {
		if contentType != "" {
			w.Header().Set("Content-Type", contentType)
		}
		w.WriteHeader(status)
		w.Write([]byte(body))
	})
}

// OnStatus sets the default fallback status for unmatched requests.
func (s *MockGerritServer) OnStatus(status int) *MockGerritServer {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.fallbackStatus = status
	return s
}

// CallCount returns the number of times a method and path was requested.
func (s *MockGerritServer) CallCount(method, path string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	count := 0
	for _, req := range s.requests {
		if (method == "" || req.Method == method) && (path == "" || req.Path == path) {
			count++
		}
	}
	return count
}

// Requests returns a copy of all recorded requests.
func (s *MockGerritServer) Requests() []MockRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	copied := make([]MockRequest, len(s.requests))
	copy(copied, s.requests)
	return copied
}

// LastRequest returns the most recent request, or nil if no requests were received.
func (s *MockGerritServer) LastRequest() *MockRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.requests) == 0 {
		return nil
	}
	req := s.requests[len(s.requests)-1]
	return &req
}
