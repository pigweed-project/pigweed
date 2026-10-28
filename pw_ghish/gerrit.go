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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

// GerritTimeLayout is the timestamp format returned by Gerrit REST APIs.
const GerritTimeLayout = "2006-01-02 15:04:05.000000000"

// Timestamp represents a Gerrit timestamp that serializes and deserializes
// using Gerrit's UTC timestamp format ("2006-01-02 15:04:05.000000000").
type Timestamp struct {
	time.Time
}

// UnmarshalJSON parses a Gerrit timestamp string into UTC time.
func (t *Timestamp) UnmarshalJSON(b []byte) error {
	s := strings.TrimSpace(string(b))
	if s == "" || s == "null" || s == `""` {
		t.Time = time.Time{}
		return nil
	}
	unquoted, err := strconv.Unquote(s)
	if err != nil {
		return fmt.Errorf("invalid gerrit timestamp %s: %w", s, err)
	}
	if unquoted == "" {
		t.Time = time.Time{}
		return nil
	}
	layouts := []string{
		GerritTimeLayout,
		"2006-01-02 15:04:05.999999999",
		"2006-01-02 15:04:05",
		time.RFC3339Nano,
		time.RFC3339,
	}
	for _, layout := range layouts {
		var parsed time.Time
		if layout == time.RFC3339Nano || layout == time.RFC3339 {
			parsed, err = time.Parse(layout, unquoted)
		} else {
			parsed, err = time.ParseInLocation(layout, unquoted, time.UTC)
		}
		if err == nil {
			t.Time = parsed
			return nil
		}
	}
	return fmt.Errorf("cannot parse gerrit timestamp %q", unquoted)
}

// MarshalJSON formats a Gerrit timestamp string in UTC.
func (t Timestamp) MarshalJSON() ([]byte, error) {
	if t.Time.IsZero() {
		return []byte(`""`), nil
	}
	return []byte(strconv.Quote(t.Time.UTC().Format(GerritTimeLayout))), nil
}

// AccountInfo describes a Gerrit user account.
type AccountInfo struct {
	AccountID   int      `json:"_account_id,omitempty"`
	Name        string   `json:"name,omitempty"`
	DisplayName string   `json:"display_name,omitempty"`
	Email       string   `json:"email,omitempty"`
	Username    string   `json:"username,omitempty"`
	Status      string   `json:"status,omitempty"`
	Inactive    bool     `json:"inactive,omitempty"`
	Tags        []string `json:"tags,omitempty"`
}

// GitPersonInfo contains information about the author or committer of a commit.
type GitPersonInfo struct {
	Name  string    `json:"name,omitempty"`
	Email string    `json:"email,omitempty"`
	Date  Timestamp `json:"date,omitzero"`
	TZ    int       `json:"tz,omitempty"`
}

// CommitInfo contains information about a Git commit.
type CommitInfo struct {
	Commit    string        `json:"commit,omitempty"`
	Parents   []CommitInfo  `json:"parents,omitempty"`
	Author    GitPersonInfo `json:"author,omitzero"`
	Committer GitPersonInfo `json:"committer,omitzero"`
	Subject   string        `json:"subject,omitempty"`
	Message   string        `json:"message,omitempty"`
}

// FetchInfo contains information about fetching a patchset.
type FetchInfo struct {
	URL      string            `json:"url,omitempty"`
	Ref      string            `json:"ref,omitempty"`
	Commands map[string]string `json:"commands,omitempty"`
}

// FileInfo contains information about a file in a patchset.
type FileInfo struct {
	Status        string `json:"status,omitempty"`
	Binary        bool   `json:"binary,omitempty"`
	OldPath       string `json:"old_path,omitempty"`
	LinesInserted int    `json:"lines_inserted,omitempty"`
	LinesDeleted  int    `json:"lines_deleted,omitempty"`
	SizeDelta     int    `json:"size_delta,omitempty"`
	Size          int    `json:"size,omitempty"`
}

// RevisionInfo contains information about a Gerrit patchset.
type RevisionInfo struct {
	Kind        string               `json:"kind,omitempty"`
	Number      int                  `json:"_number,omitempty"`
	Created     *Timestamp           `json:"created,omitempty"`
	Uploader    AccountInfo          `json:"uploader,omitzero"`
	Ref         string               `json:"ref,omitempty"`
	Fetch       map[string]FetchInfo `json:"fetch,omitempty"`
	Commit      CommitInfo           `json:"commit,omitzero"`
	Files       map[string]FileInfo  `json:"files,omitempty"`
	Description string               `json:"description,omitempty"`
}

// ApprovalInfo describes a vote on a label by an account.
type ApprovalInfo struct {
	AccountInfo
	Value int    `json:"value,omitempty"`
	Date  string `json:"date,omitempty"`
	Tag   string `json:"tag,omitempty"`
}

// LabelInfo contains information about a label on a change.
type LabelInfo struct {
	Approved     AccountInfo       `json:"approved,omitzero"`
	Rejected     AccountInfo       `json:"rejected,omitzero"`
	Recommended  AccountInfo       `json:"recommended,omitzero"`
	Disliked     AccountInfo       `json:"disliked,omitzero"`
	Blocking     bool              `json:"blocking,omitempty"`
	Value        int               `json:"value,omitempty"`
	DefaultValue int               `json:"default_value,omitempty"`
	Optional     bool              `json:"optional,omitempty"`
	All          []ApprovalInfo    `json:"all,omitempty"`
	Values       map[string]string `json:"values,omitempty"`
}

// SubmitRequirementResultInfo describes the result of evaluating a submit requirement.
type SubmitRequirementResultInfo struct {
	Name        string `json:"name,omitempty"`
	Description string `json:"description,omitempty"`
	Status      string `json:"status,omitempty"`
	IsLegacy    bool   `json:"is_legacy,omitempty"`
}

// ChangeMessageInfo contains information about a message attached to a change.
type ChangeMessageInfo struct {
	ID             string      `json:"id,omitempty"`
	Author         AccountInfo `json:"author,omitzero"`
	RealAuthor     AccountInfo `json:"real_author,omitzero"`
	Date           Timestamp   `json:"date,omitzero"`
	Message        string      `json:"message,omitempty"`
	Tag            string      `json:"tag,omitempty"`
	RevisionNumber int         `json:"_revision_number,omitempty"`
}

// AttentionSetInfo describes an entry in the attention set for a change.
type AttentionSetInfo struct {
	Account       AccountInfo `json:"account,omitzero"`
	LastUpdate    *Timestamp  `json:"last_update,omitempty"`
	Reason        string      `json:"reason,omitempty"`
	ReasonAccount AccountInfo `json:"reason_account,omitzero"`
}

// AttentionSetInput contains details for adding/removing users in the attention set.
type AttentionSetInput struct {
	User   string `json:"user,omitempty"`
	Reason string `json:"reason,omitempty"`
}

// CommentRange describes the range of an inline comment within a file.
type CommentRange struct {
	StartLine      int `json:"start_line"`
	StartCharacter int `json:"start_character"`
	EndLine        int `json:"end_line"`
	EndCharacter   int `json:"end_character"`
}

// CommentInfo contains information about an inline comment or draft.
type CommentInfo struct {
	PatchSet   int           `json:"patch_set,omitempty"`
	ID         string        `json:"id,omitempty"`
	Path       string        `json:"path,omitempty"`
	Side       string        `json:"side,omitempty"`
	Parent     int           `json:"parent,omitempty"`
	Line       int           `json:"line,omitempty"`
	Range      *CommentRange `json:"range,omitempty"`
	InReplyTo  string        `json:"in_reply_to,omitempty"`
	Message    string        `json:"message,omitempty"`
	Updated    *Timestamp    `json:"updated,omitempty"`
	Author     AccountInfo   `json:"author,omitzero"`
	Unresolved *bool         `json:"unresolved,omitempty"`
	CommitID   string        `json:"commit_id,omitempty"`
}

// CommentInput contains information for creating or updating an inline comment or draft.
type CommentInput struct {
	ID         string        `json:"id,omitempty"`
	Path       string        `json:"path,omitempty"`
	Side       string        `json:"side,omitempty"`
	Line       int           `json:"line,omitempty"`
	Range      *CommentRange `json:"range,omitempty"`
	InReplyTo  string        `json:"in_reply_to,omitempty"`
	Updated    *Timestamp    `json:"updated,omitempty"`
	Message    string        `json:"message,omitempty"`
	Unresolved *bool         `json:"unresolved,omitempty"`
}

// ReviewInput contains options for setting a review on a revision.
type ReviewInput struct {
	Message                          string                    `json:"message,omitempty"`
	Tag                              string                    `json:"tag,omitempty"`
	Labels                           map[string]int            `json:"labels,omitempty"`
	Comments                         map[string][]CommentInput `json:"comments,omitempty"`
	Drafts                           string                    `json:"drafts,omitempty"`
	Notify                           string                    `json:"notify,omitempty"`
	AddToAttentionSet                []AttentionSetInput       `json:"add_to_attention_set,omitempty"`
	RemoveFromAttentionSet           []AttentionSetInput       `json:"remove_from_attention_set,omitempty"`
	IgnoreAutomaticAttentionSetRules bool                      `json:"ignore_automatic_attention_set_rules,omitempty"`
	Ready                            bool                      `json:"ready,omitempty"`
	WorkInProgress                   bool                      `json:"work_in_progress,omitempty"`
}

// ReviewResult contains the response regarding a review that was set.
type ReviewResult struct {
	Labels map[string]int `json:"labels,omitempty"`
	Ready  bool           `json:"ready,omitempty"`
	Error  string         `json:"error,omitempty"`
}

// reviewerInput contains options for adding a reviewer to a change.
type reviewerInput struct {
	Reviewer string `json:"reviewer"`
	State    string `json:"state,omitempty"`
}

// topicInput contains options for setting a topic on a change.
type topicInput struct {
	Topic string `json:"topic,omitempty"`
}

// hashtagsInput contains options for adding or removing hashtags on a change.
type hashtagsInput struct {
	Add    []string `json:"add,omitempty"`
	Remove []string `json:"remove,omitempty"`
}

// submitInput contains options for submitting a change.
type submitInput struct {
	WaitForMerge bool `json:"wait_for_merge,omitempty"`
}

// messageInput is the request payload for Gerrit endpoints that accept an optional message.
type messageInput struct {
	Message string `json:"message,omitempty"`
}

// ChangeInfo models the subset of Gerrit's REST ChangeInfo entity fields
// (https://gerrit-review.googlesource.com/Documentation/rest-api-changes.html#change-info)
// read by pw_ghish; unmapped JSON fields in Gerrit responses are ignored.
type ChangeInfo struct {
	ID                     string                        `json:"id,omitempty"`
	Project                string                        `json:"project,omitempty"`
	Branch                 string                        `json:"branch,omitempty"`
	Topic                  string                        `json:"topic,omitempty"`
	AttentionSet           map[string]AttentionSetInfo   `json:"attention_set,omitempty"`
	Assignee               AccountInfo                   `json:"assignee,omitzero"`
	Hashtags               []string                      `json:"hashtags,omitempty"`
	ChangeID               string                        `json:"change_id,omitempty"`
	Subject                string                        `json:"subject,omitempty"`
	Status                 string                        `json:"status,omitempty"`
	Created                Timestamp                     `json:"created,omitzero"`
	Updated                Timestamp                     `json:"updated,omitzero"`
	Mergeable              bool                          `json:"mergeable,omitempty"`
	Submittable            bool                          `json:"submittable,omitempty"`
	UnresolvedCommentCount int                           `json:"unresolved_comment_count,omitempty"`
	Number                 int                           `json:"_number,omitempty"`
	Owner                  AccountInfo                   `json:"owner,omitzero"`
	Labels                 map[string]LabelInfo          `json:"labels,omitempty"`
	Reviewers              map[string][]AccountInfo      `json:"reviewers,omitempty"`
	Messages               []ChangeMessageInfo           `json:"messages,omitempty"`
	CurrentRevision        string                        `json:"current_revision,omitempty"`
	Revisions              map[string]RevisionInfo       `json:"revisions,omitempty"`
	WorkInProgress         bool                          `json:"work_in_progress,omitempty"`
	SubmitRequirements     []SubmitRequirementResultInfo `json:"submit_requirements,omitempty"`
}

// GerritAPIError represents a non-2xx HTTP response from the Gerrit REST API.
type GerritAPIError struct {
	Method     string
	URL        string
	StatusCode int
	Status     string
	Body       string
}

func (e *GerritAPIError) Error() string {
	if e.Body != "" {
		return fmt.Sprintf("gerrit %s %s failed (HTTP %d %s): %s", e.Method, e.URL, e.StatusCode, e.Status, e.Body)
	}
	return fmt.Sprintf("gerrit %s %s failed (HTTP %d %s)", e.Method, e.URL, e.StatusCode, e.Status)
}

// IsGerritHTTPStatus reports whether err is a *GerritAPIError with the given HTTP status code.
func IsGerritHTTPStatus(err error, status int) bool {
	if apiErr, ok := errors.AsType[*GerritAPIError](err); ok {
		return apiErr.StatusCode == status
	}
	return false
}

// stripXSSIPrefix removes the ")]}'" XSSI protection prefix (followed by
// "\n", "\r\n", or nothing) that Gerrit and LUCI pRPC put before JSON bodies.
func stripXSSIPrefix(b []byte) []byte {
	b = bytes.TrimPrefix(b, []byte(")]}'\r\n"))
	b = bytes.TrimPrefix(b, []byte(")]}'\n"))
	b = bytes.TrimPrefix(b, []byte(")]}'"))
	return b
}

// maxGerritErrorBodyBytes caps how much of a non-2xx response body a
// GerritAPIError keeps (2 KiB).
const maxGerritErrorBodyBytes = 2 << 10

// maxGerritPatchBytes caps GetPatch responses (128 MiB, the base64 of about
// 96 MiB of diff). It is a var so tests can lower it.
var maxGerritPatchBytes int64 = 128 << 20

// gerritResponseTooLargeError reports a response that exceeded its read limit.
type gerritResponseTooLargeError struct {
	method, url string
	limit       int64
}

func (e *gerritResponseTooLargeError) Error() string {
	return fmt.Sprintf("gerrit %s %s: response exceeds %s limit", e.method, e.url, formatByteLimit(e.limit))
}

// GerritAuth selects how a GerritClient authenticates. The zero value sends
// unauthenticated requests.
type GerritAuth struct {
	// StaticToken is an explicit token (GERRIT_TOKEN). It is only sent to the
	// client's host.
	StaticToken string
	// Dynamic mints a luci-auth token per request through
	// GerritTokenResolver, and re-mints once if Gerrit rejects it.
	Dynamic bool
	// AllowAnonymousFallback retries GETs on Google-hosted Gerrit that fail
	// with 400/401 without the token and the /a/ prefix. Unauthenticated
	// clients drop /a/ up front instead.
	AllowAnonymousFallback bool
	// modeReason explains how the auth mode was selected, for googler-mode errors.
	modeReason string
}

// GerritClient is a native Gerrit REST API client. Its authentication is
// fixed when it is built; see NewGerritClientWithHTTP.
type GerritClient struct {
	baseURL string // normalized, no trailing slash
	// host is the lower-cased hostname of baseURL. Tokens are only attached
	// to requests for this host; "" never matches.
	host string
	hc   *http.Client // CheckRedirect set, no Timeout
	auth GerritAuth
	// stderr receives the anonymous-fallback warning (os.Stderr by default).
	stderr io.Writer
}

func hasAuthenticatedPrefix(u *url.URL) bool {
	return u != nil && strings.HasPrefix(u.Path, "/a/")
}

func stripAuthenticatedPrefix(u *url.URL) {
	if !hasAuthenticatedPrefix(u) {
		return
	}
	u.Path = strings.TrimPrefix(u.Path, "/a")
	if u.RawPath != "" {
		u.RawPath = strings.TrimPrefix(u.RawPath, "/a")
	}
}

// NewGerritClientWithHTTP constructs a GerritClient for baseURL that
// authenticates as auth says. A nil hc gives an http.Client without a
// Timeout (requests are bounded by their context); any client gets
// CheckRedirect set so Authorization is stripped on cross-host redirects.
func NewGerritClientWithHTTP(baseURL string, hc *http.Client, auth GerritAuth) *GerritClient {
	u := strings.TrimSpace(baseURL)
	// Parse the URL the way hostnameOf (and so the auth policy) does, so
	// both agree on the host. Only http(s) URLs get a host that can
	// receive a token.
	if u != "" && !strings.Contains(u, "://") {
		u = "https://" + u
	}
	var host string
	if parsed, err := url.Parse(u); err == nil {
		if parsed.Scheme == "http" || parsed.Scheme == "https" {
			host = strings.ToLower(parsed.Hostname())
		}
		u = parsed.String()
	}
	u = strings.TrimRight(u, "/")
	auth.StaticToken = strings.TrimSpace(auth.StaticToken)
	return &GerritClient{
		baseURL: u,
		host:    host,
		hc:      ensureSafeHTTPClient(hc),
		auth:    auth,
		stderr:  os.Stderr,
	}
}

// BaseURL returns the client's normalized base URL (no trailing slash).
func (c *GerritClient) BaseURL() string {
	return c.baseURL
}

// Authenticated reports whether the client sends a token.
func (c *GerritClient) Authenticated() bool {
	return c.auth.StaticToken != "" || c.auth.Dynamic
}

// trusted reports whether u may receive the client's token: HTTPS (or
// loopback HTTP) to exactly the client's host, which must also be a host that
// may receive auto-minted tokens unless the token is a static GERRIT_TOKEN.
func (c *GerritClient) trusted(u *url.URL) bool {
	if c.host == "" || !urlHostIn(u, c.host) {
		return false
	}
	return c.auth.StaticToken != "" || isGerritAutoAuthHost(c.host)
}

// attachAuth sets the Authorization header on req when it is trusted and
// returns the token it sent ("" if none).
func (c *GerritClient) attachAuth(req *http.Request) (string, error) {
	if req == nil || !c.Authenticated() || !c.trusted(req.URL) {
		return "", nil
	}
	req.Header.Del("Authorization")
	if c.auth.StaticToken != "" {
		req.Header.Set("Authorization", "Bearer "+c.auth.StaticToken)
		return c.auth.StaticToken, nil
	}
	tok, _, err := GerritTokenResolver(req.Context())
	if err == nil && strings.TrimSpace(tok) == "" {
		err = errors.New("luci-auth returned no Gerrit token")
	}
	if err != nil {
		if !c.auth.AllowAnonymousFallback {
			return "", gerritGooglerAuthError(c.auth.modeReason, req.URL.Host,
				fmt.Errorf("luci-auth token refresh failed: %w", err))
		}
		return "", nil
	}
	tok = strings.TrimSpace(tok)
	req.Header.Set("Authorization", "Bearer "+tok)
	return tok, nil
}

// newGerritAPIError builds the error for a non-2xx response, keeping at most
// maxGerritErrorBodyBytes of the XSSI-stripped body.
func newGerritAPIError(method string, sent *url.URL, resp *http.Response, raw []byte) *GerritAPIError {
	u := sent
	if resp.Request != nil && resp.Request.URL != nil {
		u = resp.Request.URL
	}
	var urlStr string
	if u != nil {
		urlStr = u.Redacted()
	}
	body := bytes.TrimSpace(stripXSSIPrefix(raw))
	text := string(body)
	if len(body) > maxGerritErrorBodyBytes {
		text = strings.ToValidUTF8(string(body[:maxGerritErrorBodyBytes]), "") + "… (truncated)"
	}
	return &GerritAPIError{
		Method:     method,
		URL:        urlStr,
		StatusCode: resp.StatusCode,
		Status:     resp.Status,
		Body:       text,
	}
}

// do sends a request with a typed body (in, nil for none) and decodes the
// JSON response into out (nil to ignore it). Non-2xx responses return a
// *GerritAPIError.
func (c *GerritClient) do(ctx context.Context, method, path string, q url.Values, in, out any) error {
	raw, err := c.doRaw(ctx, method, path, q, in, maxHTTPBodyBytes)
	if err != nil {
		return err
	}
	if out == nil {
		return nil
	}
	if trimmed := bytes.TrimSpace(raw); len(trimmed) > 0 {
		return json.Unmarshal(trimmed, out)
	}
	return nil
}

// doRaw sends a request with a typed body (in, nil for none) and returns the
// XSSI-stripped response body, read up to limit bytes. Non-2xx responses
// return a *GerritAPIError.
func (c *GerritClient) doRaw(ctx context.Context, method, path string, q url.Values, in any, limit int64) ([]byte, error) {
	if c == nil {
		return nil, fmt.Errorf("internal error: gerrit client is nil")
	}
	trimmedPath := strings.TrimPrefix(path, "/")
	if strings.Contains(trimmedPath, "//") || (trimmedPath == "changes/" && len(q) == 0) {
		return nil, fmt.Errorf("gerrit %s: changeID is required", method)
	}
	reqURL := c.baseURL + "/" + trimmedPath
	if len(q) > 0 {
		sep := "?"
		if strings.Contains(reqURL, "?") {
			sep = "&"
		}
		reqURL += sep + q.Encode()
	}
	var payload []byte
	if in != nil {
		var err error
		if payload, err = json.Marshal(in); err != nil {
			return nil, fmt.Errorf("marshaling gerrit request body: %w", err)
		}
	}

	// newReq builds a request; anonymous requests carry no token and drop /a/.
	newReq := func(anonymous bool) (*http.Request, string, error) {
		var body io.Reader
		if payload != nil {
			body = bytes.NewReader(payload)
		}
		req, err := http.NewRequestWithContext(ctx, method, reqURL, body)
		if err != nil {
			return nil, "", err
		}
		req.Header.Set("Accept", "application/json")
		if payload != nil {
			req.Header.Set("Content-Type", "application/json; charset=utf-8")
		}
		if anonymous {
			stripAuthenticatedPrefix(req.URL)
			return req, "", nil
		}
		tok, err := c.attachAuth(req)
		if err != nil {
			return nil, "", err
		}
		return req, tok, nil
	}
	canFallBack := func(req *http.Request) bool {
		return c.auth.AllowAnonymousFallback && req.Method == http.MethodGet &&
			isGoogleGerritHost(req.URL.Hostname()) && hasAuthenticatedPrefix(req.URL)
	}

	req, sentTok, err := newReq(false)
	if err != nil {
		return nil, err
	}
	if sentTok == "" && canFallBack(req) {
		stripAuthenticatedPrefix(req.URL)
	}
	resp, err := c.hc.Do(req)
	if err != nil {
		return nil, err
	}

	// A rejected luci-auth token: drop it from the cache and retry once
	// with a freshly minted one.
	if c.auth.Dynamic && sentTok != "" && isGerritTokenAuthRejection(resp) &&
		(resp.Request == nil || c.trusted(resp.Request.URL)) {
		invalidateCachedGerritToken(sentTok)
		retry, newTok, rErr := newReq(false)
		if rErr != nil {
			resp.Body.Close()
			return nil, rErr
		}
		if newTok != "" && newTok != sentTok {
			resp.Body.Close()
			req = retry
			if resp, err = c.hc.Do(req); err != nil {
				return nil, err
			}
		}
	}

	if (resp.StatusCode == http.StatusBadRequest || resp.StatusCode == http.StatusUnauthorized) &&
		canFallBack(req) && !strings.HasSuffix(req.URL.Path, "/accounts/self") {
		fmt.Fprintf(c.stderr, "Warning: authenticated request to %s returned %d; falling back to anonymous\n", req.URL.Path, resp.StatusCode)
		resp.Body.Close()
		if req, _, err = newReq(true); err != nil {
			return nil, err
		}
		if resp, err = c.hc.Do(req); err != nil {
			return nil, err
		}
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		raw, _ := io.ReadAll(io.LimitReader(resp.Body, maxGerritErrorBodyBytes+512))
		return nil, newGerritAPIError(method, req.URL, resp, raw)
	}
	raw, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return nil, fmt.Errorf("gerrit %s %s: reading response: %w", method, req.URL.Redacted(), err)
	}
	if int64(len(raw)) > limit {
		return nil, &gerritResponseTooLargeError{method: method, url: req.URL.Redacted(), limit: limit}
	}
	return stripXSSIPrefix(raw), nil
}

// formatByteLimit formats a read limit for error messages.
func formatByteLimit(n int64) string {
	if n >= 1<<20 && n%(1<<20) == 0 {
		return fmt.Sprintf("%d MiB", n>>20)
	}
	return fmt.Sprintf("%d bytes", n)
}

// GetSelfAccount fetches the authenticated user's AccountInfo ("GET /accounts/self").
func (c *GerritClient) GetSelfAccount(ctx context.Context) (*AccountInfo, error) {
	var account AccountInfo
	if err := c.do(ctx, http.MethodGet, "accounts/self", nil, nil, &account); err != nil {
		return nil, err
	}
	return &account, nil
}

// GetChange fetches a single ChangeInfo ("GET /changes/{change-id}").
func (c *GerritClient) GetChange(ctx context.Context, changeID string, additionalFields ...string) (*ChangeInfo, error) {
	var q url.Values
	if len(additionalFields) > 0 {
		q = url.Values{"o": additionalFields}
	}
	var change ChangeInfo
	endpoint := fmt.Sprintf("changes/%s", url.PathEscape(changeID))
	if err := c.do(ctx, http.MethodGet, endpoint, q, nil, &change); err != nil {
		return nil, err
	}
	return &change, nil
}

// GetChangeDetail fetches detailed ChangeInfo ("GET /changes/{change-id}/detail").
func (c *GerritClient) GetChangeDetail(ctx context.Context, changeID string, additionalFields ...string) (*ChangeInfo, error) {
	var q url.Values
	if len(additionalFields) > 0 {
		q = url.Values{"o": additionalFields}
	}
	var change ChangeInfo
	endpoint := fmt.Sprintf("changes/%s/detail", url.PathEscape(changeID))
	if err := c.do(ctx, http.MethodGet, endpoint, q, nil, &change); err != nil {
		return nil, err
	}
	return &change, nil
}

// QueryChanges executes a single Gerrit change query ("GET /changes/?q=...").
func (c *GerritClient) QueryChanges(ctx context.Context, query string, limit int, additionalFields ...string) ([]ChangeInfo, error) {
	q := url.Values{"q": []string{query}}
	if limit > 0 {
		q.Set("n", strconv.Itoa(limit))
	}
	if len(additionalFields) > 0 {
		q["o"] = additionalFields
	}
	var changes []ChangeInfo
	if err := c.do(ctx, http.MethodGet, "changes/", q, nil, &changes); err != nil {
		return nil, err
	}
	return changes, nil
}

// QueryChangesMulti executes multiple Gerrit change queries in a single HTTP
// request. When queries is non-empty, the returned slice always has length
// len(queries) (including on error) so callers can safely index by query
// position.
func (c *GerritClient) QueryChangesMulti(ctx context.Context, queries []string, additionalFields ...string) ([][]ChangeInfo, error) {
	if len(queries) == 0 {
		return nil, nil
	}
	q := url.Values{
		"q": queries,
	}
	if len(additionalFields) > 0 {
		q["o"] = additionalFields
	}

	rawBody, err := c.doRaw(ctx, http.MethodGet, "changes/", q, nil, maxHTTPBodyBytes)
	if err != nil {
		return make([][]ChangeInfo, len(queries)), err
	}

	trimmed := bytes.TrimSpace(rawBody)
	var results [][]ChangeInfo
	if len(trimmed) > 0 {
		if err := json.Unmarshal(trimmed, &results); err != nil {
			// Fallback if the server returned a 1D slice (e.g., when len(queries) == 1).
			var single []ChangeInfo
			if singleErr := json.Unmarshal(trimmed, &single); singleErr == nil {
				results = [][]ChangeInfo{single}
			} else {
				return make([][]ChangeInfo, len(queries)), fmt.Errorf("decoding multi-query changes response: %w", err)
			}
		}
	}
	for len(results) < len(queries) {
		results = append(results, nil)
	}
	return results, nil
}

func revisionEndpoint(changeID, revisionID, suffix string) string {
	return fmt.Sprintf(
		"changes/%s/revisions/%s/%s",
		url.PathEscape(changeID),
		url.PathEscape(cmp.Or(revisionID, "current")),
		strings.TrimPrefix(suffix, "/"),
	)
}

// SetReview posts a review on a change revision ("POST /changes/{change-id}/revisions/{revision-id}/review").
// It safely ignores unmarshaling errors on the "labels" field of ReviewResult, which can vary across Gerrit versions.
func (c *GerritClient) SetReview(ctx context.Context, changeID, revision string, input *ReviewInput) (*ReviewResult, error) {
	if input == nil {
		return nil, fmt.Errorf("internal error: review input is nil")
	}
	if input.Drafts == "" {
		cp := *input
		cp.Drafts = "KEEP"
		input = &cp
	}
	endpoint := revisionEndpoint(changeID, revision, "review")
	var result ReviewResult
	if err := c.do(ctx, http.MethodPost, endpoint, nil, input, &result); err != nil {
		if unmarshalErr, ok := errors.AsType[*json.UnmarshalTypeError](err); ok && strings.Contains(unmarshalErr.Field, "labels") {
			return &result, nil
		}
		return nil, err
	}
	return &result, nil
}

// SubmitChange submits a change ("POST /changes/{change-id}/submit").
func (c *GerritClient) SubmitChange(ctx context.Context, changeID string) (*ChangeInfo, error) {
	endpoint := fmt.Sprintf("changes/%s/submit", url.PathEscape(changeID))
	var change ChangeInfo
	if err := c.do(ctx, http.MethodPost, endpoint, nil, &submitInput{WaitForMerge: true}, &change); err != nil {
		return nil, err
	}
	return &change, nil
}

// AbandonChange abandons a change ("POST /changes/{change-id}/abandon").
func (c *GerritClient) AbandonChange(ctx context.Context, changeID, message string) (*ChangeInfo, error) {
	endpoint := fmt.Sprintf("changes/%s/abandon", url.PathEscape(changeID))
	var change ChangeInfo
	if err := c.do(ctx, http.MethodPost, endpoint, nil, &messageInput{Message: message}, &change); err != nil {
		return nil, err
	}
	return &change, nil
}

// RestoreChange restores an abandoned change ("POST /changes/{change-id}/restore").
func (c *GerritClient) RestoreChange(ctx context.Context, changeID, message string) (*ChangeInfo, error) {
	endpoint := fmt.Sprintf("changes/%s/restore", url.PathEscape(changeID))
	var change ChangeInfo
	if err := c.do(ctx, http.MethodPost, endpoint, nil, &messageInput{Message: message}, &change); err != nil {
		return nil, err
	}
	return &change, nil
}

// SetReadyForReview marks a WIP change as ready for review ("POST /changes/{change-id}/ready").
func (c *GerritClient) SetReadyForReview(ctx context.Context, changeID, message string) error {
	endpoint := fmt.Sprintf("changes/%s/ready", url.PathEscape(changeID))
	return c.do(ctx, http.MethodPost, endpoint, nil, &messageInput{Message: message}, nil)
}

// SetWorkInProgress marks a change as work in progress ("POST /changes/{change-id}/wip").
func (c *GerritClient) SetWorkInProgress(ctx context.Context, changeID, message string) error {
	endpoint := fmt.Sprintf("changes/%s/wip", url.PathEscape(changeID))
	return c.do(ctx, http.MethodPost, endpoint, nil, &messageInput{Message: message}, nil)
}

// AddReviewer adds a reviewer or CC to a change ("POST /changes/{change-id}/reviewers").
func (c *GerritClient) AddReviewer(ctx context.Context, changeID, reviewer, state string) error {
	endpoint := fmt.Sprintf("changes/%s/reviewers", url.PathEscape(changeID))
	return c.do(ctx, http.MethodPost, endpoint, nil, &reviewerInput{
		Reviewer: reviewer,
		State:    state,
	}, nil)
}

// DeleteReviewer removes a reviewer from a change ("DELETE /changes/{change-id}/reviewers/{account-id}").
func (c *GerritClient) DeleteReviewer(ctx context.Context, changeID, accountID string) error {
	endpoint := fmt.Sprintf("changes/%s/reviewers/%s", url.PathEscape(changeID), url.PathEscape(accountID))
	return c.do(ctx, http.MethodDelete, endpoint, nil, nil, nil)
}

// AddToAttentionSet adds a user to the attention set ("POST /changes/{change-id}/attention").
func (c *GerritClient) AddToAttentionSet(ctx context.Context, changeID, user, reason string) error {
	endpoint := fmt.Sprintf("changes/%s/attention", url.PathEscape(changeID))
	return c.do(ctx, http.MethodPost, endpoint, nil, &AttentionSetInput{
		User:   user,
		Reason: reason,
	}, nil)
}

// RemoveFromAttentionSet removes a user from the attention set ("DELETE /changes/{change-id}/attention/{account-id}").
func (c *GerritClient) RemoveFromAttentionSet(ctx context.Context, changeID, accountID, reason string) error {
	endpoint := fmt.Sprintf("changes/%s/attention/%s", url.PathEscape(changeID), url.PathEscape(accountID))
	return c.do(ctx, http.MethodDelete, endpoint, nil, &AttentionSetInput{
		Reason: reason,
	}, nil)
}

// SetTopic sets the topic on a change ("PUT /changes/{change-id}/topic").
func (c *GerritClient) SetTopic(ctx context.Context, changeID, topic string) error {
	endpoint := fmt.Sprintf("changes/%s/topic", url.PathEscape(changeID))
	return c.do(ctx, http.MethodPut, endpoint, nil, &topicInput{Topic: topic}, nil)
}

// DeleteTopic deletes the topic on a change ("DELETE /changes/{change-id}/topic").
func (c *GerritClient) DeleteTopic(ctx context.Context, changeID string) error {
	endpoint := fmt.Sprintf("changes/%s/topic", url.PathEscape(changeID))
	return c.do(ctx, http.MethodDelete, endpoint, nil, nil, nil)
}

// SetHashtags adds and/or removes hashtags on a change ("POST /changes/{change-id}/hashtags").
func (c *GerritClient) SetHashtags(ctx context.Context, changeID string, add, remove []string) error {
	endpoint := fmt.Sprintf("changes/%s/hashtags", url.PathEscape(changeID))
	return c.do(ctx, http.MethodPost, endpoint, nil, &hashtagsInput{
		Add:    add,
		Remove: remove,
	}, nil)
}

// ListChangeComments lists published inline comments on a change ("GET /changes/{change-id}/comments").
func (c *GerritClient) ListChangeComments(ctx context.Context, changeID string) (map[string][]CommentInfo, error) {
	endpoint := fmt.Sprintf("changes/%s/comments", url.PathEscape(changeID))
	var comments map[string][]CommentInfo
	if err := c.do(ctx, http.MethodGet, endpoint, nil, nil, &comments); err != nil {
		return nil, err
	}
	return comments, nil
}

// ListChangeDrafts lists draft inline comments on a change ("GET /changes/{change-id}/drafts").
func (c *GerritClient) ListChangeDrafts(ctx context.Context, changeID string) (map[string][]CommentInfo, error) {
	endpoint := fmt.Sprintf("changes/%s/drafts", url.PathEscape(changeID))
	var drafts map[string][]CommentInfo
	if err := c.do(ctx, http.MethodGet, endpoint, nil, nil, &drafts); err != nil {
		return nil, err
	}
	return drafts, nil
}

// CreateDraft creates a draft comment on a revision ("PUT /changes/{change-id}/revisions/{revision-id}/drafts").
func (c *GerritClient) CreateDraft(ctx context.Context, changeID, revision string, input *CommentInput) (*CommentInfo, error) {
	endpoint := revisionEndpoint(changeID, revision, "drafts")
	var created CommentInfo
	if err := c.do(ctx, http.MethodPut, endpoint, nil, input, &created); err != nil {
		return nil, err
	}
	return &created, nil
}

// UpdateDraft updates an existing draft comment ("PUT /changes/{change-id}/revisions/{revision-id}/drafts/{draft-id}").
func (c *GerritClient) UpdateDraft(ctx context.Context, changeID, revision, draftID string, input *CommentInput) (*CommentInfo, error) {
	endpoint := revisionEndpoint(changeID, revision, "drafts/"+url.PathEscape(draftID))
	var updated CommentInfo
	if err := c.do(ctx, http.MethodPut, endpoint, nil, input, &updated); err != nil {
		return nil, err
	}
	return &updated, nil
}

// DeleteDraft deletes a draft comment ("DELETE /changes/{change-id}/revisions/{revision-id}/drafts/{draft-id}").
func (c *GerritClient) DeleteDraft(ctx context.Context, changeID, revision, draftID string) error {
	endpoint := revisionEndpoint(changeID, revision, "drafts/"+url.PathEscape(draftID))
	return c.do(ctx, http.MethodDelete, endpoint, nil, nil, nil)
}

// ListFiles lists the files modified in a revision ("GET /changes/{change-id}/revisions/{revision-id}/files").
func (c *GerritClient) ListFiles(ctx context.Context, changeID, revision string) (map[string]FileInfo, error) {
	endpoint := revisionEndpoint(changeID, revision, "files")
	var files map[string]FileInfo
	if err := c.do(ctx, http.MethodGet, endpoint, nil, nil, &files); err != nil {
		return nil, err
	}
	return files, nil
}

// GetPatch fetches the unified diff patch text for a revision ("GET /changes/{change-id}/revisions/{revision-id}/patch").
func (c *GerritClient) GetPatch(ctx context.Context, changeID, revision string) (string, error) {
	endpoint := revisionEndpoint(changeID, revision, "patch")
	raw, err := c.doRaw(ctx, http.MethodGet, endpoint, nil, nil, maxGerritPatchBytes)
	if _, tooLarge := errors.AsType[*gerritResponseTooLargeError](err); tooLarge {
		return "", fmt.Errorf("%w; fetch the change with 'gh pr checkout' and use 'git diff' instead", err)
	}
	if err != nil {
		return "", err
	}
	cleaned := bytes.TrimSpace(raw)
	if len(cleaned) >= 2 && cleaned[0] == '"' && cleaned[len(cleaned)-1] == '"' {
		var unquoted string
		if err := json.Unmarshal(cleaned, &unquoted); err == nil {
			return unquoted, nil
		}
	}
	return string(cleaned), nil
}

// ListProjectLabels lists label definitions for a Gerrit project ("GET /projects/{project-name}/labels/").
func (c *GerritClient) ListProjectLabels(ctx context.Context, project string) ([]LabelDefinition, error) {
	if c == nil || project == "" {
		return nil, fmt.Errorf("client and project are required")
	}
	endpoint := fmt.Sprintf("projects/%s/labels/", url.PathEscape(project))
	var labels []LabelDefinition
	if err := c.do(ctx, http.MethodGet, endpoint, nil, nil, &labels); err != nil {
		return nil, err
	}
	return labels, nil
}

// QueryChangeCodeOwners queries the Gerrit code-owners plugin for a file in a change revision.
func (c *GerritClient) QueryChangeCodeOwners(ctx context.Context, changeID, revision, filePath string) ([]CodeOwnerInfo, error) {
	if c == nil || changeID == "" || filePath == "" {
		return nil, fmt.Errorf("client, changeID, and filePath are required")
	}
	endpoint := revisionEndpoint(changeID, revision, "code_owners/"+url.PathEscape(filePath))
	q := url.Values{
		"limit": []string{"10"},
		"o":     []string{"DETAILS"},
	}
	var resp codeOwnersResponse
	if err := c.do(ctx, http.MethodGet, endpoint, q, nil, &resp); err != nil {
		return nil, err
	}
	return resp.CodeOwners, nil
}

// QueryBranchCodeOwners queries the Gerrit code-owners plugin for a file on a project branch.
func (c *GerritClient) QueryBranchCodeOwners(ctx context.Context, project, branch, filePath string) ([]CodeOwnerInfo, error) {
	if c == nil || project == "" || branch == "" || filePath == "" {
		return nil, fmt.Errorf("client, project, branch, and filePath are required")
	}
	endpoint := fmt.Sprintf(
		"projects/%s/branches/%s/code_owners/%s",
		url.PathEscape(project),
		url.PathEscape(branch),
		url.PathEscape(filePath),
	)
	q := url.Values{
		"limit": []string{"10"},
		"o":     []string{"DETAILS"},
	}
	var resp codeOwnersResponse
	if err := c.do(ctx, http.MethodGet, endpoint, q, nil, &resp); err != nil {
		return nil, err
	}
	return resp.CodeOwners, nil
}

// IsFlowsEnabled checks whether Gerrit flows are enabled for a change ("GET /changes/{change-num}/is-flows-enabled").
func (c *GerritClient) IsFlowsEnabled(ctx context.Context, changeNum int) (*isFlowsEnabledInfo, error) {
	endpoint := fmt.Sprintf("changes/%d/is-flows-enabled", changeNum)
	var info isFlowsEnabledInfo
	if err := c.do(ctx, http.MethodGet, endpoint, nil, nil, &info); err != nil {
		return nil, err
	}
	return &info, nil
}

// ListFlows lists the flows on a change ("GET /changes/{change-num}/flows").
func (c *GerritClient) ListFlows(ctx context.Context, changeNum int) ([]FlowInfo, error) {
	endpoint := fmt.Sprintf("changes/%d/flows", changeNum)
	var flows []FlowInfo
	if err := c.do(ctx, http.MethodGet, endpoint, nil, nil, &flows); err != nil {
		return nil, err
	}
	return flows, nil
}

// CreateFlow creates a flow on a change ("POST /changes/{change-num}/flows").
func (c *GerritClient) CreateFlow(ctx context.Context, changeNum int, payload FlowInput) error {
	endpoint := fmt.Sprintf("changes/%d/flows", changeNum)
	return c.do(ctx, http.MethodPost, endpoint, nil, payload, nil)
}

// DeleteFlow deletes a flow on a change ("DELETE /changes/{change-num}/flows/{flow-id}").
func (c *GerritClient) DeleteFlow(ctx context.Context, changeNum int, flowUUID string) error {
	endpoint := fmt.Sprintf("changes/%d/flows/%s", changeNum, url.PathEscape(flowUUID))
	return c.do(ctx, http.MethodDelete, endpoint, nil, nil, nil)
}
