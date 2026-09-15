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
	"encoding/json"
	"fmt"
	"io"
	"strings"
	"text/template"
)

// Renderer handles output rendering using Go templates or JSON formatting.
type Renderer struct {
	Out             io.Writer
	JSONFields      string
	Template        string
	DefaultTemplate string
	FuncMap         template.FuncMap
}

// Render executes the rendering process.
func (r *Renderer) Render(data any) error {
	if r.Out == nil {
		return fmt.Errorf("renderer output writer is nil")
	}

	if r.JSONFields != "" {
		return r.renderJSON(data)
	}

	tmplStr := r.DefaultTemplate
	if r.Template != "" {
		tmplStr = r.Template
	}
	if strings.TrimSpace(tmplStr) == "" {
		return fmt.Errorf("renderer template is empty: neither Template nor DefaultTemplate was specified")
	}

	tmpl, err := template.New("output").Funcs(r.FuncMap).Parse(tmplStr)
	if err != nil {
		return fmt.Errorf("error parsing template: %w", err)
	}

	return tmpl.Execute(r.Out, data)
}

func (r *Renderer) renderJSON(data any) error {
	filtered, err := filterData(data, SplitJSONFields(r.JSONFields))
	if err != nil {
		return err
	}

	encoded, err := json.MarshalIndent(filtered, "", "  ")
	if err != nil {
		return fmt.Errorf("error marshaling JSON: %w", err)
	}

	_, err = fmt.Fprintln(r.Out, string(encoded))
	return err
}

// SplitJSONFields returns the field names in a --json specification, ignoring
// empty entries. It is exported so that a command can tell which fields the
// caller asked for -- and refuse, before rendering, to answer one it cannot
// answer honestly.
func SplitJSONFields(spec string) []string {
	var fields []string
	for _, f := range strings.Split(spec, ",") {
		if f = strings.TrimSpace(f); f != "" {
			fields = append(fields, f)
		}
	}
	return fields
}

// filterData narrows data to fields, which must already be clean (see
// SplitJSONFields). An empty list means "everything".
func filterData(data any, fields []string) (any, error) {
	cleanFields := fields
	if len(cleanFields) == 0 {
		return data, nil
	}

	switch v := data.(type) {
	case map[string]any:
		return filterMap(v, cleanFields)
	case []map[string]any:
		if len(v) == 0 {
			return []any{}, nil
		}
		var result []any
		for _, item := range v {
			filteredItem, err := filterMap(item, cleanFields)
			if err != nil {
				return nil, err
			}
			result = append(result, filteredItem)
		}
		return result, nil
	case []any:
		if len(v) == 0 {
			return []any{}, nil
		}
		var result []any
		for _, item := range v {
			filteredItem, err := filterData(item, cleanFields)
			if err != nil {
				return nil, err
			}
			result = append(result, filteredItem)
		}
		return result, nil
	default:
		return data, nil
	}
}

func filterMap(m map[string]any, fields []string) (map[string]any, error) {
	result := make(map[string]any)
	var unknown []string
	for _, f := range fields {
		if val, ok := m[f]; ok {
			result[f] = val
		} else {
			unknown = append(unknown, f)
		}
	}
	if len(unknown) > 0 {
		var available []string
		for k := range m {
			available = append(available, k)
		}
		return nil, fmt.Errorf("unknown JSON field(s): %s (available: %s)",
			strings.Join(unknown, ", "), strings.Join(available, ", "))
	}
	return result, nil
}
