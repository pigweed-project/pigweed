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
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func newLogServer(t *testing.T, status int, content string) *httptest.Server {
	t.Helper()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(status)
		w.Write([]byte(content))
	}))
	t.Cleanup(server.Close)
	return server
}

func TestGetBuildDetails_Success(t *testing.T) {
	ctx := context.Background()

	mockResp := `)]}'
{
  "id": "8680709829694997522",
  "builder": {
    "project": "pigweed",
    "bucket": "pigweed.try",
    "builder": "pigweed-linux"
  },
  "status": "FAILURE",
  "summaryMarkdown": "Build compilation failed",
  "steps": [
    {
      "name": "setup",
      "status": "SUCCESS"
    },
    {
      "name": "compile|ninja",
      "status": "FAILURE",
      "summaryMarkdown": "ninja: error: 'foo.cc' not found",
      "logs": [
        {
          "name": "stdout",
          "viewUrl": "https://logs.chromium.org/logs/pigweed/mock/+/u/compile/ninja/stdout"
        }
      ]
    }
  ]
}`

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/prpc/buildbucket.v2.Builds/GetBuild" {
			t.Errorf("Unexpected path: %s", r.URL.Path)
		}
		body, _ := io.ReadAll(r.Body)
		if !strings.Contains(string(body), `"mask":{"fields":`) {
			t.Errorf("Expected request body to contain mask.fields, got: %s", string(body))
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(mockResp))
	}))
	defer server.Close()

	details, err := GetBuildDetails(ctx, server.URL, "8680709829694997522", server.Client())
	if err != nil {
		t.Fatalf("GetBuildDetails failed: %v", err)
	}

	if details.ID != "8680709829694997522" {
		t.Errorf("Expected ID 8680709829694997522, got %s", details.ID)
	}
	if details.Status != "FAILURE" {
		t.Errorf("Expected status FAILURE, got %s", details.Status)
	}
	if len(details.Steps) != 2 {
		t.Fatalf("Expected 2 steps, got %d", len(details.Steps))
	}
	if details.Steps[1].Name != "compile|ninja" || details.Steps[1].Status != "FAILURE" {
		t.Errorf("Unexpected step 1: %+v", details.Steps[1])
	}
	if len(details.Steps[1].Logs) != 1 || details.Steps[1].Logs[0].Name != "stdout" {
		t.Errorf("Unexpected logs: %+v", details.Steps[1].Logs)
	}
}

func TestFetchLogStream_Raw(t *testing.T) {
	ctx := context.Background()

	rawLog := `line 1
line 2
line 3
line 4
line 5
`

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasSuffix(r.URL.RawQuery, "format=raw") {
			t.Errorf("Expected query format=raw, got: %s", r.URL.RawQuery)
		}
		w.Write([]byte(rawLog))
	}))
	defer server.Close()

	// 1. Fetch full log
	full, err := FetchLogStream(ctx, server.URL+"/+/u/test/stdout", 0, server.Client())
	if err != nil {
		t.Fatalf("FetchLogStream failed: %v", err)
	}
	if strings.TrimSpace(full) != strings.TrimSpace(rawLog) {
		t.Errorf("Expected full log, got: %s", full)
	}

	// 2. Fetch tail (last 2 lines)
	tail, err := FetchLogStream(ctx, server.URL+"/+/u/test/stdout", 2, server.Client())
	if err != nil {
		t.Fatalf("FetchLogStream tail failed: %v", err)
	}
	expectedTail := "line 4\nline 5"
	if strings.TrimSpace(tail) != expectedTail {
		t.Errorf("Expected tail %q, got %q", expectedTail, tail)
	}
}

func TestExtractFailureReports(t *testing.T) {
	ctx := context.Background()
	logServer := newLogServer(t, http.StatusOK, "error: undefined reference to 'pw::Init()'\nFAILED: pw_main")

	build := LUCIBuildDetails{
		ID: "12345",
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "pigweed.try",
			Builder: "pigweed-linux",
		},
		Status: "FAILURE",
		Steps: []LUCIStep{
			{
				Name:   "setup",
				Status: "SUCCESS",
			},
			{
				Name:            "compile",
				Status:          "FAILURE",
				SummaryMarkdown: "ninja execution failed",
				Logs: []LUCILog{
					{
						Name:    "stdout",
						ViewURL: logServer.URL + "/log",
					},
				},
			},
		},
	}

	report := build.ExtractFailureReport(ctx, 10, logServer.Client())
	if report == nil {
		t.Fatal("Expected non-nil FailureReport")
	}

	if report.Builder != "pigweed-linux" {
		t.Errorf("Expected builder pigweed-linux, got %s", report.Builder)
	}
	if report.FailedStep != "compile" {
		t.Errorf("Expected failed step compile, got %s", report.FailedStep)
	}
	if report.StepSummary != "ninja execution failed" {
		t.Errorf("Expected summary 'ninja execution failed', got %s", report.StepSummary)
	}
	if !strings.Contains(report.LogSnippet, "undefined reference") {
		t.Errorf("Expected log snippet to contain error message, got: %s", report.LogSnippet)
	}
}

func TestExtractFailureReport_PrefersStepWithLogs(t *testing.T) {
	ctx := context.Background()
	logServer := newLogServer(t, http.StatusOK, "fatal error: header.h not found")

	// Simulate build where child step failed with stdout, and trailing sibling failed with no logs
	build := LUCIBuildDetails{
		ID:      "67890",
		Builder: bbBuilder{Builder: "pigweed-docs"},
		Status:  "FAILURE",
		Steps: []LUCIStep{
			{
				Name:   "docs|bazel",
				Status: "FAILURE",
				Logs: []LUCILog{
					{
						Name:    "stdout",
						ViewURL: logServer.URL + "/stdout",
					},
				},
			},
			{
				Name:   "docs|logs",
				Status: "FAILURE",
				// No logs attached
			},
		},
	}

	report := build.ExtractFailureReport(ctx, 10, logServer.Client())
	if report == nil {
		t.Fatal("Expected non-nil FailureReport")
	}
	if report.FailedStep != "docs|bazel" {
		t.Errorf("Expected FailedStep 'docs|bazel', got %q", report.FailedStep)
	}
	if report.LogName != "stdout" {
		t.Errorf("Expected LogName 'stdout', got %q", report.LogName)
	}
	if !strings.Contains(report.LogSnippet, "fatal error") {
		t.Errorf("Expected log snippet to contain error, got %q", report.LogSnippet)
	}
}

func TestExtractFailureReport_FallbackBuildSummary(t *testing.T) {
	ctx := context.Background()

	build := LUCIBuildDetails{
		ID:              "11223",
		Builder:         bbBuilder{Builder: "pigweed-test"},
		Status:          "FAILURE",
		SummaryMarkdown: "Build timed out after 30m",
		Steps: []LUCIStep{
			{
				Name:   "timeout",
				Status: "FAILURE",
				// No summaryMarkdown on the step
			},
		},
	}

	report := build.ExtractFailureReport(ctx, 10, http.DefaultClient)
	if report == nil {
		t.Fatal("Expected non-nil FailureReport")
	}
	if report.StepSummary != "Build timed out after 30m" {
		t.Errorf("Expected fallback to build SummaryMarkdown, got %q", report.StepSummary)
	}
}

func TestExtractFailureReport_LogFetchErrorEmbeddedInSnippet(t *testing.T) {
	ctx := context.Background()
	errServer := newLogServer(t, http.StatusForbidden, "LogDog access denied or log missing\n")

	build := LUCIBuildDetails{
		ID:      "99999",
		Builder: bbBuilder{Builder: "pigweed-linux"},
		Status:  "FAILURE",
		Steps: []LUCIStep{
			{
				Name:   "compile",
				Status: "FAILURE",
				Logs: []LUCILog{
					{
						Name:    "stdout",
						ViewURL: errServer.URL + "/log",
					},
				},
			},
		},
	}

	report := build.ExtractFailureReport(ctx, 10, errServer.Client())
	if report == nil {
		t.Fatal("Expected non-nil FailureReport")
	}
	if !strings.Contains(report.LogSnippet, "[Error fetching log stream") {
		t.Errorf("Expected LogSnippet to contain error notice, got: %q", report.LogSnippet)
	}
	if !strings.Contains(report.LogSnippet, "403") {
		t.Errorf("Expected LogSnippet to mention 403 status code, got: %q", report.LogSnippet)
	}
}

func TestNewLUCIClient(t *testing.T) {
	t.Run("default http client", func(t *testing.T) {
		client := NewLUCIClient("cr-buildbucket.appspot.com", nil)
		if client.Host != "cr-buildbucket.appspot.com" {
			t.Errorf("expected host cr-buildbucket.appspot.com, got %q", client.Host)
		}
		if client.HTTPClient != http.DefaultClient {
			t.Errorf("expected http.DefaultClient when nil passed, got %v", client.HTTPClient)
		}
	})

	t.Run("custom http client", func(t *testing.T) {
		custom := &http.Client{}
		client := NewLUCIClient("my-host", custom)
		if client.HTTPClient != custom {
			t.Errorf("expected custom client, got %v", client.HTTPClient)
		}
	})
}

func TestCallPRPC_Success(t *testing.T) {
	ctx := context.Background()

	type echoReq struct {
		Message string `json:"message"`
	}
	type echoResp struct {
		Reply string `json:"reply"`
	}

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/prpc/test.Service/Echo" {
			t.Errorf("unexpected path: %s", r.URL.Path)
		}
		if r.Header.Get("Content-Type") != "application/json" {
			t.Errorf("missing or unexpected Content-Type header: %s", r.Header.Get("Content-Type"))
		}
		if r.Header.Get("Accept") != "application/json" {
			t.Errorf("missing or unexpected Accept header: %s", r.Header.Get("Accept"))
		}
		var req echoReq
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Fatalf("failed to decode req: %v", err)
		}
		w.Header().Set("Content-Type", "application/json")
		// Include standard pRPC prefix
		w.Write([]byte(")]}'\n{\"reply\":\"" + req.Message + "\"}"))
	}))
	defer server.Close()

	client := NewLUCIClient(server.URL, server.Client())
	var resp echoResp
	err := client.CallPRPC(ctx, "test.Service", "Echo", echoReq{Message: "hello"}, &resp)
	if err != nil {
		t.Fatalf("CallPRPC failed: %v", err)
	}
	if resp.Reply != "hello" {
		t.Errorf("expected reply 'hello', got %q", resp.Reply)
	}
}

func TestCallPRPC_Invariants(t *testing.T) {
	ctx := context.Background()
	var resp struct{}

	t.Run("nil client", func(t *testing.T) {
		var client *LUCIClient
		err := client.CallPRPC(ctx, "service", "method", struct{}{}, &resp)
		if err == nil || !strings.Contains(err.Error(), "LUCIClient is nil") {
			t.Errorf("expected error mentioning nil client, got %v", err)
		}
	})

	t.Run("empty host", func(t *testing.T) {
		client := NewLUCIClient("", nil)
		err := client.CallPRPC(ctx, "service", "method", struct{}{}, &resp)
		if err == nil || !strings.Contains(err.Error(), "host cannot be empty") {
			t.Errorf("expected error mentioning empty host, got %v", err)
		}
	})

	t.Run("empty service or method", func(t *testing.T) {
		client := NewLUCIClient("host", nil)
		if err := client.CallPRPC(ctx, "", "method", struct{}{}, &resp); err == nil {
			t.Error("expected error for empty service")
		}
		if err := client.CallPRPC(ctx, "service", "", struct{}{}, &resp); err == nil {
			t.Error("expected error for empty method")
		}
	})

	t.Run("nil req or resp", func(t *testing.T) {
		client := NewLUCIClient("host", nil)
		if err := client.CallPRPC(ctx, "service", "method", nil, &resp); err == nil {
			t.Error("expected error for nil req")
		}
		if err := client.CallPRPC(ctx, "service", "method", struct{}{}, nil); err == nil {
			t.Error("expected error for nil resp")
		}
	})
}

func TestCallPRPC_Errors(t *testing.T) {
	ctx := context.Background()

	t.Run("http non-200", func(t *testing.T) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "permission denied", http.StatusForbidden)
		}))
		defer server.Close()

		client := NewLUCIClient(server.URL, server.Client())
		var resp struct{}
		err := client.CallPRPC(ctx, "service", "method", struct{}{}, &resp)
		if err == nil {
			t.Fatal("expected error for HTTP 403, got nil")
		}
		if !strings.Contains(err.Error(), "403") || !strings.Contains(err.Error(), "permission denied") {
			t.Errorf("expected error mentioning 403 and body, got: %v", err)
		}
	})

	t.Run("invalid json response", func(t *testing.T) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Write([]byte(")]}'\nnot valid json!"))
		}))
		defer server.Close()

		client := NewLUCIClient(server.URL, server.Client())
		var resp struct{}
		err := client.CallPRPC(ctx, "service", "method", struct{}{}, &resp)
		if err == nil {
			t.Fatal("expected error for invalid json, got nil")
		}
		if !strings.Contains(err.Error(), "failed to parse") {
			t.Errorf("expected parse error, got: %v", err)
		}
	})

	t.Run("context canceled", func(t *testing.T) {
		canceledCtx, cancel := context.WithCancel(ctx)
		cancel()

		client := NewLUCIClient("example.com", nil)
		var resp struct{}
		err := client.CallPRPC(canceledCtx, "service", "method", struct{}{}, &resp)
		if err == nil {
			t.Fatal("expected error for canceled context, got nil")
		}
	})
}

func TestSearchBuilds(t *testing.T) {
	ctx := context.Background()

	t.Run("success via mock server", func(t *testing.T) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path != "/prpc/buildbucket.v2.Builds/SearchBuilds" {
				t.Errorf("unexpected path: %s", r.URL.Path)
			}
			w.Write([]byte(")]}'\n{\"builds\":[{\"id\":\"12345\",\"status\":\"SUCCESS\",\"builder\":{\"project\":\"pigweed\",\"bucket\":\"try\",\"builder\":\"linux\"}}]}"))
		}))
		defer server.Close()

		client := NewLUCIClient(server.URL, server.Client())
		builds, err := client.SearchBuilds(ctx, "gerrit.googlesource.com", "pigweed/pigweed", 123, 1)
		if err != nil {
			t.Fatalf("SearchBuilds failed: %v", err)
		}
		if len(builds) != 1 {
			t.Fatalf("expected 1 build, got %d", len(builds))
		}
		if builds[0].ID != "12345" || builds[0].Status != "SUCCESS" || builds[0].Builder.Builder != "linux" {
			t.Errorf("unexpected build: %+v", builds[0])
		}
	})

	t.Run("invariants", func(t *testing.T) {
		client := NewLUCIClient("host", nil)
		if _, err := client.SearchBuilds(ctx, "", "proj", 1, 1); err == nil {
			t.Error("expected error on empty gerritHost")
		}
		if _, err := client.SearchBuilds(ctx, "host", "", 1, 1); err == nil {
			t.Error("expected error on empty project")
		}
		if _, err := client.SearchBuilds(ctx, "host", "proj", 0, 1); err == nil {
			t.Error("expected error on changeNum <= 0")
		}
		if _, err := client.SearchBuilds(ctx, "host", "proj", 1, 0); err == nil {
			t.Error("expected error on patchsetNum <= 0")
		}
		var nilClient *LUCIClient
		if _, err := nilClient.SearchBuilds(ctx, "host", "proj", 1, 1); err == nil {
			t.Error("expected error on nil client")
		}
	})
}

func TestGetBuildDetails_Invariants(t *testing.T) {
	ctx := context.Background()

	t.Run("empty buildID", func(t *testing.T) {
		client := NewLUCIClient("host", nil)
		if _, err := client.GetBuildDetails(ctx, ""); err == nil {
			t.Error("expected error on empty buildID")
		}
	})

	t.Run("nil client", func(t *testing.T) {
		var client *LUCIClient
		if _, err := client.GetBuildDetails(ctx, "123"); err == nil {
			t.Error("expected error on nil client")
		}
	})
}

func TestFetchLogStream_Invariants(t *testing.T) {
	ctx := context.Background()

	t.Run("empty viewURL", func(t *testing.T) {
		client := NewLUCIClient("host", nil)
		if _, err := client.FetchLogStream(ctx, "", 10); err == nil {
			t.Error("expected error on empty viewURL")
		}
	})

	t.Run("nil client", func(t *testing.T) {
		var client *LUCIClient
		if _, err := client.FetchLogStream(ctx, "https://logs.chromium.org/log", 10); err == nil {
			t.Error("expected error on nil client")
		}
	})
}

func TestExtractFailureReport_LUCIClient(t *testing.T) {
	ctx := context.Background()
	logServer := newLogServer(t, http.StatusOK, "compile error in file.cc")

	build := &LUCIBuildDetails{
		ID:      "123",
		Builder: bbBuilder{Builder: "linux-builder"},
		Status:  "FAILURE",
		Steps: []LUCIStep{
			{
				Name:            "ninja",
				Status:          "FAILURE",
				SummaryMarkdown: "ninja failed",
				Logs: []LUCILog{
					{Name: "stdout", ViewURL: logServer.URL + "/log"},
				},
			},
		},
	}

	client := NewLUCIClient("", logServer.Client())
	report := client.ExtractFailureReport(ctx, build, 5)
	if report == nil {
		t.Fatal("expected non-nil failure report")
	}
	if report.Builder != "linux-builder" {
		t.Errorf("expected builder linux-builder, got %q", report.Builder)
	}
	if report.FailedStep != "ninja" {
		t.Errorf("expected failed step ninja, got %q", report.FailedStep)
	}
	if !strings.Contains(report.LogSnippet, "compile error in file.cc") {
		t.Errorf("expected log snippet to contain error, got: %q", report.LogSnippet)
	}

	t.Run("nil build returns nil", func(t *testing.T) {
		if r := client.ExtractFailureReport(ctx, nil, 5); r != nil {
			t.Errorf("expected nil report for nil build, got %+v", r)
		}
	})

	t.Run("non-failing build returns nil", func(t *testing.T) {
		successBuild := &LUCIBuildDetails{Status: "SUCCESS"}
		if r := client.ExtractFailureReport(ctx, successBuild, 5); r != nil {
			t.Errorf("expected nil report for successful build, got %+v", r)
		}
	})

	t.Run("nil client defaults safely", func(t *testing.T) {
		var nilClient *LUCIClient
		r := nilClient.ExtractFailureReport(ctx, build, 5)
		if r == nil {
			t.Fatal("expected non-nil report when client is nil")
		}
	})
}

func TestExtractFailureReport_NoSteps_InfraFailureSummary(t *testing.T) {
	ctx := context.Background()
	build := &LUCIBuildDetails{
		ID: "8671031638107722401",
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "pigweed.try",
			Builder: "pigweed-mac-arm-zephyr",
		},
		Status:          "INFRA_FAILURE",
		SummaryMarkdown: "Task did not start, no resource",
		Steps:           nil,
	}

	report := build.ExtractFailureReport(ctx, 10, http.DefaultClient)
	if report == nil {
		t.Fatal("expected non-nil report for INFRA_FAILURE build without steps")
	}
	if report.Builder != "pigweed-mac-arm-zephyr" {
		t.Errorf("expected builder pigweed-mac-arm-zephyr, got %q", report.Builder)
	}
	if report.Status != "INFRA_FAILURE" {
		t.Errorf("expected status INFRA_FAILURE, got %q", report.Status)
	}
	if report.StepSummary != "Task did not start, no resource" {
		t.Errorf("expected StepSummary 'Task did not start, no resource', got %q", report.StepSummary)
	}
}

func TestExtractFailureReport_NoSteps_CancellationMarkdown(t *testing.T) {
	ctx := context.Background()
	build := &LUCIBuildDetails{
		ID: "8671031638107722402",
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "pigweed.try",
			Builder: "pigweed-linux",
		},
		Status:               "FAILURE",
		CancellationMarkdown: "Build was cancelled by CQ",
		Steps:                nil,
	}

	report := build.ExtractFailureReport(ctx, 10, http.DefaultClient)
	if report == nil {
		t.Fatal("expected non-nil report")
	}
	if report.StepSummary != "Build was cancelled by CQ" {
		t.Errorf("expected StepSummary 'Build was cancelled by CQ', got %q", report.StepSummary)
	}
}

func TestExtractFailureReport_NoSteps_FallbackGeneric(t *testing.T) {
	ctx := context.Background()
	build := &LUCIBuildDetails{
		ID: "8671031638107722403",
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "pigweed.try",
			Builder: "pigweed-linux",
		},
		Status: "INFRA_FAILURE",
		Steps:  nil,
	}

	report := build.ExtractFailureReport(ctx, 10, http.DefaultClient)
	if report == nil {
		t.Fatal("expected non-nil report")
	}
	expected := "Build ended with status INFRA_FAILURE, but no step details were reported."
	if report.StepSummary != expected {
		t.Errorf("expected StepSummary %q, got %q", expected, report.StepSummary)
	}
}

func TestExtractFailureReport_NoSteps_ResourceExhaustion(t *testing.T) {
	ctx := context.Background()
	build := &LUCIBuildDetails{
		ID: "8671031638107722404",
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "pigweed.try",
			Builder: "pigweed-mac-arm-vscode",
		},
		Status: "INFRA_FAILURE",
		StatusDetails: &LUCIStatusDetails{
			ResourceExhaustion: &struct{}{},
		},
		Steps: nil,
	}

	report := build.ExtractFailureReport(ctx, 10, http.DefaultClient)
	if report == nil {
		t.Fatal("expected non-nil report")
	}
	expected := "Task did not start: resource exhaustion (no available bots in pool)"
	if report.StepSummary != expected {
		t.Errorf("expected StepSummary %q, got %q", expected, report.StepSummary)
	}
}

func TestExtractFailureReport_NoSteps_Timeout(t *testing.T) {
	ctx := context.Background()
	build := &LUCIBuildDetails{
		ID: "8671031638107722405",
		Builder: bbBuilder{
			Project: "pigweed",
			Bucket:  "pigweed.try",
			Builder: "pigweed-linux-bazel-host",
		},
		Status: "FAILURE",
		StatusDetails: &LUCIStatusDetails{
			Timeout: &struct{}{},
		},
		Steps: nil,
	}

	report := build.ExtractFailureReport(ctx, 10, http.DefaultClient)
	if report == nil {
		t.Fatal("expected non-nil report")
	}
	expected := "Task timed out before completion"
	if report.StepSummary != expected {
		t.Errorf("expected StepSummary %q, got %q", expected, report.StepSummary)
	}
}

func TestGetBuildDetails_MaskUsesSnakeCase(t *testing.T) {
	ctx := context.Background()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		if !strings.Contains(string(body), "summary_markdown") {
			t.Errorf("Expected request body to contain 'summary_markdown', got: %s", string(body))
		}
		if !strings.Contains(string(body), "cancellation_markdown") {
			t.Errorf("Expected request body to contain 'cancellation_markdown', got: %s", string(body))
		}
		if !strings.Contains(string(body), "status_details") {
			t.Errorf("Expected request body to contain 'status_details', got: %s", string(body))
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`)]}'` + "\n" + `{"id":"123","status":"SUCCESS"}`))
	}))
	defer server.Close()

	client := NewLUCIClient(server.URL, server.Client())
	_, err := client.GetBuildDetails(ctx, "123")
	if err != nil {
		t.Fatalf("GetBuildDetails failed: %v", err)
	}
}

func TestSearchBuilds_MaskIncludesSummary(t *testing.T) {
	ctx := context.Background()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		if !strings.Contains(string(body), "summary_markdown") {
			t.Errorf("Expected SearchBuilds request body to contain 'summary_markdown' mask, got: %s", string(body))
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`)]}'` + "\n" + `{"builds":[{"id":"123","status":"SUCCESS","summaryMarkdown":"ok"}]}`))
	}))
	defer server.Close()

	client := NewLUCIClient(server.URL, server.Client())
	builds, err := client.SearchBuilds(ctx, "pigweed-review.googlesource.com", "pigweed/pigweed", 472267, 37)
	if err != nil {
		t.Fatalf("SearchBuilds failed: %v", err)
	}
	if len(builds) != 1 || builds[0].SummaryMarkdown != "ok" {
		t.Errorf("Unexpected builds result: %+v", builds)
	}
}

func TestFormatBuildSteps(t *testing.T) {
	build := &LUCIBuildDetails{
		ID: "8671031638107722737",
		Builder: bbBuilder{
			Builder: "pigweed-linux-static-analysis",
		},
		Status:          "STARTED",
		SummaryMarkdown: "running clang-tidy",
		Steps: []LUCIStep{
			{Name: "setup", Status: "SUCCESS"},
			{Name: "environment|doctor", Status: "SUCCESS", SummaryMarkdown: "all healthy"},
			{Name: "static_analysis", Status: "STARTED"},
		},
	}

	formatted := FormatBuildSteps(build)
	if !strings.Contains(formatted, "Steps for pigweed-linux-static-analysis (Build 8671031638107722737)") {
		t.Errorf("missing header in formatted output:\n%s", formatted)
	}
	if !strings.Contains(formatted, "Status: STARTED *") {
		t.Errorf("missing status symbol in formatted output:\n%s", formatted)
	}
	if !strings.Contains(formatted, "✓  setup") {
		t.Errorf("missing step setup in formatted output:\n%s", formatted)
	}
	if !strings.Contains(formatted, "*  static_analysis") {
		t.Errorf("missing running step static_analysis in formatted output:\n%s", formatted)
	}
	if !strings.Contains(formatted, "Summary: running clang-tidy") {
		t.Errorf("missing summary in formatted output:\n%s", formatted)
	}

	t.Run("empty steps", func(t *testing.T) {
		emptyBuild := &LUCIBuildDetails{
			ID:      "123",
			Builder: bbBuilder{Builder: "empty"},
			Status:  "INFRA_FAILURE",
		}
		out := FormatBuildSteps(emptyBuild)
		if !strings.Contains(out, "(no steps recorded for this build)") {
			t.Errorf("expected empty steps note, got:\n%s", out)
		}
	})

	t.Run("nil build", func(t *testing.T) {
		if out := FormatBuildSteps(nil); out != "" {
			t.Errorf("expected empty string for nil build, got %q", out)
		}
	})
}

func TestFormatBuildSteps_HierarchicalAndSanitization(t *testing.T) {
	build := &LUCIBuildDetails{
		ID: "8671017385591991937",
		Builder: bbBuilder{
			Builder: "pigweed-linux-bazel-rust",
		},
		Status: "SUCCESS",
		Steps: []LUCIStep{
			{Name: "setup_build", Status: "SUCCESS", SummaryMarkdown: "running recipe: \"workflows\" with Python 3.11.9"},
			{Name: "checkout pigweed", Status: "SUCCESS"},
			{Name: "checkout pigweed|cache", Status: "SUCCESS", SummaryMarkdown: "hit"},
			{Name: "checkout pigweed|cache|makedirs", Status: "SUCCESS"},
			{Name: "checkout pigweed|change data|changes|pigweed:472267", Status: "SUCCESS", SummaryMarkdown: "Change(number=472267, remote='https://pigweed.googlesource.com/pigweed/pigweed', ref='refs/changes/67/472267/46', rebase=True, project='pigweed/pigweed', branch='main', gerrit_name='pigweed')"},
			{Name: "checkout pigweed|status", Status: "SUCCESS", SummaryMarkdown: "applied [Change(number=472267, remote='https://...')]"},
			{Name: "build rust_nightly", Status: "SUCCESS"},
			{Name: "build rust_nightly|before disk usage", Status: "SUCCESS", SummaryMarkdown: "77.36/590.33 GB used (13.1%)"},
			{Name: "build rust_nightly|run", Status: "SUCCESS"},
		},
	}

	out := FormatBuildSteps(build)

	// Top-level steps must be present
	if !strings.Contains(out, "✓  setup_build") {
		t.Errorf("expected top-level setup_build, got:\n%s", out)
	}
	if !strings.Contains(out, "✓  checkout pigweed") {
		t.Errorf("expected top-level checkout pigweed, got:\n%s", out)
	}
	if !strings.Contains(out, "✓  build rust_nightly") {
		t.Errorf("expected top-level build rust_nightly, got:\n%s", out)
	}

	// Internal sub-steps should NOT be dumped in default view
	if strings.Contains(out, "checkout pigweed|cache|makedirs") {
		t.Errorf("did not expect internal child step in default view, got:\n%s", out)
	}
	if strings.Contains(out, "build rust_nightly|before disk usage") {
		t.Errorf("did not expect disk usage child step in default view, got:\n%s", out)
	}

	// Python object dumps must be suppressed
	if strings.Contains(out, "Change(number=472267") {
		t.Errorf("Python object dump must be suppressed, but was found in output:\n%s", out)
	}
	if strings.Contains(out, "applied [Change") {
		t.Errorf("Python list dump must be suppressed, but was found in output:\n%s", out)
	}
}

func TestFormatBuildSteps_HighlightsFailingSubSteps(t *testing.T) {
	build := &LUCIBuildDetails{
		ID: "8671182706745774001",
		Builder: bbBuilder{
			Builder: "pigweed-lintformat",
		},
		Status: "FAILURE",
		Steps: []LUCIStep{
			{Name: "setup_build", Status: "SUCCESS"},
			{Name: "python_format", Status: "FAILURE"},
			{Name: "python_format|logs|glob|*.bat", Status: "SUCCESS"},
			{Name: "python_format|failure summary", Status: "FAILURE", SummaryMarkdown: "python_format failed"},
			{Name: "bazel_lint", Status: "SUCCESS"},
		},
	}

	out := FormatBuildSteps(build)

	if !strings.Contains(out, "✗  python_format") {
		t.Errorf("expected failing top-level step, got:\n%s", out)
	}
	if !strings.Contains(out, "└── ✗  failure summary") {
		t.Errorf("expected indented failing child step, got:\n%s", out)
	}
	if strings.Contains(out, "python_format|logs|glob|*.bat") {
		t.Errorf("passing child step of failing parent should not be dumped, got:\n%s", out)
	}
}

func TestFormatBuildSteps_CleanBuildSummaryExperiments(t *testing.T) {
	build := &LUCIBuildDetails{
		ID: "123",
		Builder: bbBuilder{
			Builder: "openprot-earlgrey",
		},
		Status:          "SUCCESS",
		SummaryMarkdown: "**Experiments**:\n* pigweed.disable_rbe",
		Steps: []LUCIStep{
			{Name: "setup", Status: "SUCCESS"},
		},
	}

	out := FormatBuildSteps(build)
	if strings.Contains(out, "**Experiments**") {
		t.Errorf("internal experiment markdown should be filtered out, got:\n%s", out)
	}
}

func TestBBBuild_IsExperimental(t *testing.T) {
	tests := []struct {
		name     string
		build    bbBuild
		expected bool
	}{
		{
			name: "critical NO",
			build: bbBuild{
				Critical: "NO",
			},
			expected: true,
		},
		{
			name: "critical lowercase no",
			build: bbBuild{
				Critical: "no",
			},
			expected: true,
		},
		{
			name: "cq_experimental tag true",
			build: bbBuild{
				Tags: []bbTag{
					{Key: "cq_experimental", Value: "true"},
				},
			},
			expected: true,
		},
		{
			name: "cq_experimental tag 1",
			build: bbBuild{
				Tags: []bbTag{
					{Key: "cq_experimental", Value: "1"},
				},
			},
			expected: true,
		},
		{
			name: "input experiment pigweed.non_production",
			build: bbBuild{
				Input: &bbInput{
					Experiments: []string{"pigweed.disable_github", "pigweed.non_production"},
				},
			},
			expected: true,
		},
		{
			name: "input experiment luci.non_production",
			build: bbBuild{
				Input: &bbInput{
					Experiments: []string{"luci.non_production"},
				},
			},
			expected: true,
		},
		{
			name: "summary markdown non_production fallback",
			build: bbBuild{
				SummaryMarkdown: "**Experiments**:\n* pigweed.non_production",
			},
			expected: true,
		},
		{
			name: "summary markdown cq_experimental fallback",
			build: bbBuild{
				SummaryMarkdown: "cq_experimental: true",
			},
			expected: true,
		},
		{
			name: "production build",
			build: bbBuild{
				Critical: "YES",
				Input: &bbInput{
					Experiments: []string{"pigweed.disable_github"},
				},
				Tags: []bbTag{
					{Key: "user_agent", Value: "cq"},
				},
				SummaryMarkdown: "All checks passed",
			},
			expected: false,
		},
		{
			name:     "empty build",
			build:    bbBuild{},
			expected: false,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			actual := tc.build.IsExperimental()
			if actual != tc.expected {
				t.Errorf("IsExperimental() = %v, expected %v for %+v", actual, tc.expected, tc.build)
			}
		})
	}
}

func TestSearchBuilds_MaskIncludesExperimentalFields(t *testing.T) {
	ctx := context.Background()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		bodyStr := string(body)
		if !strings.Contains(bodyStr, "critical") {
			t.Errorf("Expected SearchBuilds request mask to include 'critical', got: %s", bodyStr)
		}
		if !strings.Contains(bodyStr, "input.experiments") {
			t.Errorf("Expected SearchBuilds request mask to include 'input.experiments', got: %s", bodyStr)
		}
		if !strings.Contains(bodyStr, "tags") {
			t.Errorf("Expected SearchBuilds request mask to include 'tags', got: %s", bodyStr)
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`)]}'` + "\n" + `{"builds":[]}`))
	}))
	defer server.Close()

	client := NewLUCIClient(server.URL, server.Client())
	_, err := client.SearchBuilds(ctx, "pigweed-review.googlesource.com", "pigweed/pigweed", 472267, 37)
	if err != nil {
		t.Fatalf("SearchBuilds failed: %v", err)
	}
}

func TestExtractStepDiagnostic(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		expected string
	}{
		{
			name:     "empty",
			input:    "",
			expected: "",
		},
		{
			name:     "single file diff",
			input:    "````\n--- /b/s/w/ir/x/w/co/docs/common/header.js  (original)\n+++ /b/s/w/ir/x/w/co/docs/common/header.js  (reformatted)\n@@ -67,7 +67,11 @@\n````",
			expected: "formatting diff in docs/common/header.js",
		},
		{
			name:     "multi file diff",
			input:    "````\n--- /b/s/w/ir/x/w/co/docs/common/header.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/common/header.py  (reformatted)\n--- /b/s/w/ir/x/w/co/docs/common/nav.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/common/nav.py  (reformatted)\n--- /b/s/w/ir/x/w/co/docs/tests/header_test.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/tests/header_test.py  (reformatted)\n--- /b/s/w/ir/x/w/co/docs/tests/search_test.py  (original)\n+++ /b/s/w/ir/x/w/co/docs/tests/search_test.py  (reformatted)\n````",
			expected: "formatting diff in 4 files: docs/common/header.py, docs/common/nav.py (+2 more)",
		},
		{
			name:     "compiler error with line number",
			input:    "````\n[ACTION //pw_build/py:workflows.lint.mypy(//pw_build/python_toolchain:python)]\nFAILED: [code=1] python/gen/pw_build/py/workflows.lint.mypy.pw_pystamp\npython [...]/py/pw_build/python_runner.py ...\n[...]/pw_build/workflows/build_driver.py:66: error: Argument 1 to \"ParseDict\" has incompatible type \"str\"\nFound 1 error in 1 file (checked 13 source files)\n````",
			expected: "build_driver.py:66: error: Argument 1 to \"ParseDict\" has incompatible type \"str\"",
		},
		{
			name:     "split line compiler error where error message is on next line",
			input:    "````\n[ACTION //pw_build/py:workflows.lint.mypy(//pw_build/python_toolchain:python)]\nFAILED: [code=1] python/gen/pw_build/py/workflows.lint.mypy.pw_pystamp\n[...]/pw_build/workflows/build_driver.py:66: error:\nArgument 1 to \"ParseDict\" has incompatible type \"str\"; expected \"dict[str, Any]\"\nFound 1 error in 1 file\n````",
			expected: "build_driver.py:66: error: Argument 1 to \"ParseDict\" has incompatible type \"s...",
		},
		{
			name:     "single line failure message",
			input:    "ninja execution failed",
			expected: "ninja execution failed",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			actual := extractStepDiagnostic(tc.input)
			if actual != tc.expected {
				t.Errorf("extractStepDiagnostic() =\n%q\nwant:\n%q", actual, tc.expected)
			}
		})
	}
}

func TestFormatBuildSteps_FailureSummaryWithDiagnostic(t *testing.T) {
	build := &LUCIBuildDetails{
		ID: "8671182706745774001",
		Builder: bbBuilder{
			Builder: "pigweed-lintformat",
		},
		Status: "FAILURE",
		Steps: []LUCIStep{
			{Name: "setup_build", Status: "SUCCESS"},
			{Name: "javascript_format", Status: "FAILURE"},
			{
				Name:            "javascript_format|failure summary",
				Status:          "FAILURE",
				SummaryMarkdown: "````\n--- /b/s/w/ir/x/w/co/docs/common/header.js  (original)\n+++ /b/s/w/ir/x/w/co/docs/common/header.js  (reformatted)\n@@ -67,7 +67,11 @@\n````",
			},
			{Name: "gn_python_build_check", Status: "FAILURE"},
			{Name: "gn_python_build_check|easy rerun cmd (2)", Status: "FAILURE"},
			{
				Name:            "gn_python_build_check|failure summary",
				Status:          "FAILURE",
				SummaryMarkdown: "FAILED: ninja python.tests failed",
			},
			{Name: "custom_check", Status: "FAILURE"},
			{
				Name:   "custom_check|failure summary",
				Status: "FAILURE",
				// No summary markdown
			},
		},
	}

	out := FormatBuildSteps(build)

	// 1. Unified diff diagnostic rendered
	if !strings.Contains(out, "└── ✗  failure summary: formatting diff in docs/common/header.js") {
		t.Errorf("expected failure summary with diff diagnostic, got:\n%s", out)
	}

	// 2. Compiler/linter error rendered
	if !strings.Contains(out, "└── ✗  failure summary: FAILED: ninja python.tests failed") {
		t.Errorf("expected failure summary with failure message, got:\n%s", out)
	}

	// 3. Fallback when no summary markdown available
	if !strings.Contains(out, "└── ✗  failure summary (run 'gh run view -j pigweed-lintformat --log-failed' to inspect)") {
		t.Errorf("expected failure summary fallback hint, got:\n%s", out)
	}

	// 4. easy rerun cmd variants filtered out
	if strings.Contains(out, "easy rerun cmd") {
		t.Errorf("expected 'easy rerun cmd (2)' to be omitted, got:\n%s", out)
	}
}

func TestCleanStepSummary(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		expected string
	}{
		{
			name:     "empty",
			input:    "",
			expected: "",
		},
		{
			name:     "short error",
			input:    "ninja: error: 'foo.cc' not found",
			expected: "ninja: error: 'foo.cc' not found",
		},
		{
			name:     "longer useful error between 60 and 120 chars",
			input:    "clang: error: unable to execute command: Segmentation fault (core dumped) in /workspace/pigweed/foo/bar.cc:42",
			expected: "clang: error: unable to execute command: Segmentation fault (core dumped) in /workspace/pigweed/foo/bar.cc:42",
		},
		{
			name:     "error exceeding 120 chars truncated",
			input:    "this is a very long error message that exceeds one hundred and twenty characters in length and therefore should be truncated cleanly with an ellipsis",
			expected: "this is a very long error message that exceeds one hundred and twenty characters in length and therefore should be tr...",
		},
		{
			name:     "json dump suppressed",
			input:    "{\"error\": \"details\", \"status\": 500}",
			expected: "",
		},
		{
			name:     "embedded json data dump suppressed",
			input:    "returned unexpected payload: {\"key\": \"value\"}",
			expected: "",
		},
		{
			name:     "python repr suppressed",
			input:    "Change(number=123, project='pw')",
			expected: "",
		},
		{
			name:     "multiline suppressed",
			input:    "line 1\nline 2",
			expected: "",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := cleanStepSummary(tc.input)
			if got != tc.expected {
				t.Errorf("cleanStepSummary(%q) = %q, want %q", tc.input, got, tc.expected)
			}
		})
	}
}
