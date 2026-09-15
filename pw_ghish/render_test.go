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
	"encoding/json"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestRender_Template(t *testing.T) {
	data := map[string]any{
		"number": 123,
		"title":  "Test Title",
	}

	tests := []struct {
		name            string
		template        string
		defaultTemplate string
		want            string
	}{
		{
			name:            "default template",
			defaultTemplate: "Number: {{.number}}, Title: {{.title}}",
			want:            "Number: 123, Title: Test Title",
		},
		{
			name:            "user template override",
			template:        "Title: {{.title}}",
			defaultTemplate: "Number: {{.number}}",
			want:            "Title: Test Title",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var buf bytes.Buffer
			r := &Renderer{
				Out:             &buf,
				Template:        tt.template,
				DefaultTemplate: tt.defaultTemplate,
			}

			if err := r.Render(data); err != nil {
				t.Fatalf("Render failed: %v", err)
			}

			if buf.String() != tt.want {
				t.Errorf("Render() = %q, want %q", buf.String(), tt.want)
			}
		})
	}
}

func TestRender_JSON(t *testing.T) {
	data := map[string]any{
		"number": 123,
		"title":  "Test Title",
		"state":  "OPEN",
	}

	tests := []struct {
		name       string
		jsonFields string
		want       map[string]any
	}{
		{
			name:       "single field",
			jsonFields: "number",
			want:       map[string]any{"number": float64(123)},
		},
		{
			name:       "multiple fields",
			jsonFields: "number,title",
			want:       map[string]any{"number": float64(123), "title": "Test Title"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var buf bytes.Buffer
			r := &Renderer{
				Out:        &buf,
				JSONFields: tt.jsonFields,
			}

			if err := r.Render(data); err != nil {
				t.Fatalf("Render failed: %v", err)
			}

			var got map[string]any
			if err := json.Unmarshal(buf.Bytes(), &got); err != nil {
				t.Fatalf("failed to unmarshal result: %v", err)
			}

			if !cmp.Equal(got, tt.want) {
				t.Errorf("Render() JSON mismatch (-got +want):\n%s", cmp.Diff(got, tt.want))
			}
		})
	}
}

func TestRender_NilOutError(t *testing.T) {
	r := &Renderer{
		Out: nil,
	}
	err := r.Render(map[string]any{"key": "val"})
	if err == nil {
		t.Fatal("expected error when Out is nil, got nil")
	}
	if !strings.Contains(err.Error(), "output writer is nil") {
		t.Errorf("expected error about nil output writer, got: %v", err)
	}
}

func TestRender_JSON_UnknownFieldError(t *testing.T) {
	data := map[string]any{
		"number": 123,
		"title":  "Test Title",
	}

	var buf bytes.Buffer
	r := &Renderer{
		Out:        &buf,
		JSONFields: "number,nonexistent_field",
	}

	err := r.Render(data)
	if err == nil {
		t.Fatal("expected error when unknown field requested in JSONFields, got nil")
	}
	if !strings.Contains(err.Error(), "unknown JSON field") {
		t.Errorf("expected error about unknown JSON field, got: %v", err)
	}
	if !strings.Contains(err.Error(), "nonexistent_field") {
		t.Errorf("expected error to mention the bad field, got: %v", err)
	}
}

func TestRender_JSON_List_UnknownFieldError(t *testing.T) {
	data := []map[string]any{
		{"number": 123, "title": "Change 1"},
		{"number": 456, "title": "Change 2"},
	}

	var buf bytes.Buffer
	r := &Renderer{
		Out:        &buf,
		JSONFields: "typo_field",
	}

	err := r.Render(data)
	if err == nil {
		t.Fatal("expected error when unknown field requested in list JSONFields, got nil")
	}
	if !strings.Contains(err.Error(), "unknown JSON field") {
		t.Errorf("expected error about unknown JSON field, got: %v", err)
	}
}

func TestRender_EmptyTemplateError(t *testing.T) {
	var buf bytes.Buffer
	r := &Renderer{
		Out: &buf,
	}

	err := r.Render(map[string]any{"number": 123})
	if err == nil {
		t.Fatal("expected error when template and default template are empty, got nil")
	}
	if !strings.Contains(err.Error(), "empty") {
		t.Errorf("expected error mentioning empty template, got: %v", err)
	}
}
