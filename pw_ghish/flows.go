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
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/andygrunwald/go-gerrit"
	"github.com/spf13/cobra"
)

// DefaultScheduleTimezone is the default timezone used for scheduled Gerrit flows.
const DefaultScheduleTimezone = "America/Los_Angeles"

var (
	scheduleTimeRegex = regexp.MustCompile(`^([01]?[0-9]|2[0-3]):[0-5][0-9]$`)
	scheduleNowFn     = time.Now
)

// FlowAction represents the action to execute when a Gerrit flow stage condition is met.
type FlowAction struct {
	Name       string   `json:"name"`
	Parameters []string `json:"parameters"`
}

// FlowStageExpression represents a single condition and action stage in a Gerrit flow.
type FlowStageExpression struct {
	Condition string     `json:"condition"`
	Action    FlowAction `json:"action"`
}

// FlowInput represents the request payload for POST /changes/<id>/flows.
type FlowInput struct {
	StageExpressions []FlowStageExpression `json:"stage_expressions"`
}

// FlowInfo is a flow as returned by GET /changes/<id>/flows.
type FlowInfo struct {
	UUID   string          `json:"uuid"`
	Stages []FlowStageInfo `json:"stages"`
}

// FlowStageInfo is one stage of a FlowInfo.
type FlowStageInfo struct {
	Expression FlowExpressionInfo `json:"expression"`
	// State is PENDING, DONE, FAILED or TERMINATED.
	State string `json:"state"`
}

// FlowExpressionInfo is the condition and action of a FlowStageInfo.
type FlowExpressionInfo struct {
	Condition string         `json:"condition"`
	Action    FlowActionInfo `json:"action"`
}

// FlowActionInfo is the action of a FlowExpressionInfo. Parameters are kept
// raw because their shape (list or map) depends on the flow service.
type FlowActionInfo struct {
	Name       string          `json:"name"`
	Parameters json.RawMessage `json:"parameters,omitempty"`
}

// parameterValues returns the action's parameters as strings, accepting
// either a JSON list or a JSON object. An object's values are returned in key
// order, since Go map iteration order is random.
func (a FlowActionInfo) parameterValues() []string {
	if len(a.Parameters) == 0 {
		return nil
	}
	var list []string
	if err := json.Unmarshal(a.Parameters, &list); err == nil {
		return list
	}
	var obj map[string]string
	if err := json.Unmarshal(a.Parameters, &obj); err == nil {
		keys := make([]string, 0, len(obj))
		for k := range obj {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		vals := make([]string, 0, len(obj))
		for _, k := range keys {
			vals = append(vals, obj[k])
		}
		return vals
	}
	return nil
}

// ScheduledSubmitFlow describes the flows CreateScheduledSubmitFlow created.
type ScheduledSubmitFlow struct {
	// ChangeNum is the numeric change the flows were created on.
	ChangeNum int
	// When is the schedule, e.g. "tomorrow at 02:36".
	When string
	// TZ is the timezone When is in.
	TZ string
	// Replaced is the number of pending scheduled submit flows that were
	// deleted so they would not fire alongside the new ones.
	Replaced int
	// MarkedReady is true if the change was work in progress and was marked
	// ready for review before the flow was created.
	MarkedReady bool
}

// Summary returns the line printed after the flows are created.
func (s ScheduledSubmitFlow) Summary() string {
	msg := fmt.Sprintf("Created submit flow for Change %d (scheduled for %s in %s).", s.ChangeNum, s.When, s.TZ)
	if s.MarkedReady {
		msg += " Marked it ready for review."
	}
	if s.Replaced > 0 {
		msg += fmt.Sprintf(" Replaced %d previously scheduled submit flow(s).", s.Replaced)
	}
	return msg
}

// ValidateScheduleTime validates that customTime is in 24-hour "HH:MM" format
// and returns it zero-padded to "HH:MM": a single-digit hour such as "9:05" is
// accepted and returned as "09:05", so every later use (the flow condition,
// stack offsets) sees one format.
func ValidateScheduleTime(customTime string) (string, error) {
	trimmed := strings.TrimSpace(customTime)
	if !scheduleTimeRegex.MatchString(trimmed) {
		return "", fmt.Errorf("invalid time %q: expected 24-hour format \"HH:MM\" in PT / MTV time (e.g. \"21:45\")", customTime)
	}
	if len(trimmed) == len("9:05") {
		trimmed = "0" + trimmed
	}
	return trimmed, nil
}

// ComputeScheduleTime returns the target "HH:MM" schedule time.
// If customTime is non-empty, it is validated against 24-hour "HH:MM" format.
// Otherwise, a deterministic time between 21:00 and 02:59 is computed from changeNum.
func ComputeScheduleTime(changeNum int, customTime string) (string, error) {
	if customTime != "" {
		return ValidateScheduleTime(customTime)
	}
	if changeNum < 0 {
		changeNum = 0
	}
	// 6-hour window (21:00 to 03:00) = 360 minutes.
	offsetMinutes := changeNum % 360
	targetHour := (21 + (offsetMinutes / 60)) % 24
	targetMin := offsetMinutes % 60
	return fmt.Sprintf("%02d:%02d", targetHour, targetMin), nil
}

// ScheduleDay returns "today" if targetTime ("HH:MM") is still in the future
// today in tz, or "tomorrow" if targetTime has already passed today in tz.
func ScheduleDay(targetTime string, tz string) string {
	parts := strings.SplitN(strings.TrimSpace(targetTime), ":", 2)
	if len(parts) != 2 {
		return "today"
	}
	hour, errH := strconv.Atoi(parts[0])
	min, errM := strconv.Atoi(parts[1])
	if errH != nil || errM != nil {
		return "today"
	}

	if tz == "" {
		tz = DefaultScheduleTimezone
	}
	loc, err := time.LoadLocation(tz)
	if err != nil {
		if hour < 12 {
			return "tomorrow"
		}
		return "today"
	}

	nowInTz := scheduleNowFn().In(loc)
	targetToday := time.Date(nowInTz.Year(), nowInTz.Month(), nowInTz.Day(), hour, min, 0, 0, loc)
	if !targetToday.After(nowInTz) {
		return "tomorrow"
	}
	return "today"
}

// flowVotes are the two votes a scheduled submit flow casts, in order, each
// formatted as "<Label>+<N>".
type flowVotes struct {
	// DryRun starts presubmit verification without submitting, e.g.
	// "Commit-Queue+1" or "Presubmit-Ready+1".
	DryRun string
	// AutoSubmit asks for the change to be submitted once it is
	// submittable, e.g. "Pigweed-Auto-Submit+1".
	AutoSubmit string
}

// resolveFlowVotes returns the votes scheduled Gerrit Flows cast, read from
// the labels the change itself reports: the host's dry-run vote (see
// commitQueueDryRunVote) and its auto-submit vote.
//
// The labels are looked up rather than guessed from the host or profile. A
// flow is accepted when it is created but only acts hours later, so voting a
// label the host does not define fails when nobody is watching, and the change
// silently never submits. Refusing up front, before any flow exists, is the
// only point at which the user can still be told.
func resolveFlowVotes(change *gerrit.ChangeInfo) (flowVotes, error) {
	autoSubmit, ok, err := FindAutoSubmitLabel(change.Labels)
	if err != nil {
		return flowVotes{}, err
	}
	if !ok {
		return flowVotes{}, noFlowAutoSubmitError(change)
	}
	dryRun, ok := commitQueueDryRunVote(change.Labels)
	if !ok {
		return flowVotes{}, noFlowDryRunError(change)
	}
	return flowVotes{
		DryRun:     fmt.Sprintf("%s%+d", dryRun.Name, dryRun.Value),
		AutoSubmit: fmt.Sprintf("%s%+d", autoSubmit.Name, autoSubmit.Value),
	}, nil
}

// noFlowAutoSubmitError reports that a change's host has no auto-submit label,
// so a scheduled flow cannot arrange for the change to be submitted.
func noFlowAutoSubmitError(change *gerrit.ChangeInfo) error {
	root := RootCmd.CommandPath()
	return fmt.Errorf("--nightly/--submit-at: change %d has no auto-submit label, so a scheduled flow cannot arrange for it to be submitted.\n\n"+
		"Labels here: %s\n\n"+
		"To submit via the Commit Queue now instead:\n"+
		"  %s pr merge %d --cq",
		change.Number, FormatLabelNames(change.Labels), root, change.Number)
}

// noFlowDryRunError reports that a change's host has no single Commit-Queue
// or Presubmit-Ready label permitting +1, so a scheduled flow has no way to
// start presubmit verification.
func noFlowDryRunError(change *gerrit.ChangeInfo) error {
	root := RootCmd.CommandPath()
	return fmt.Errorf("--nightly/--submit-at: change %d has no Commit-Queue or Presubmit-Ready label that accepts +1, so a scheduled flow cannot start its presubmits.\n\n"+
		"Labels here: %s\n\n"+
		"To have it submitted once it is ready instead:\n"+
		"  %s pr merge %d --auto",
		change.Number, FormatLabelNames(change.Labels), root, change.Number)
}

// CreateScheduledSubmitFlow creates a Gerrit Flow on the change that, at
// targetTime in tz, casts the host's dry-run vote (Commit-Queue+1 or
// Presubmit-Ready+1) and then the auto-submit label. Both are read from the
// change's labels; see resolveFlowVotes.
//
// It does not branch on whether the change is submittable. The auto-submit
// label already does that: once the change is submittable, whether at the
// scheduled time or later, the auto-submit bot submits it, which can reuse the
// dry run's tryjobs. A single unconditional flow also leaves nothing
// armed after it fires, unlike a pair of is:submittable / -is:submittable
// flows, where the one that did not fire would vote later on its own.
//
// Any scheduled submit flows on the change that have not finished yet are
// deleted first, so the new schedule replaces the old one. See
// isScheduledSubmitFlow.
//
// If the change is work in progress, it is marked ready for review first: a
// scheduled submit says the change is done.
//
// Before anything on the change is modified, it checks that Gerrit Flows are
// enabled for the change and returns a *FlowsNotEnabledError if not. See
// checkFlowsEnabled.
//
// If changeNum <= 0, the numeric change number is taken from Gerrit. The change
// is always fetched, because its labels name the labels to vote; if it lacks
// either, an error is returned before any flow is created.
func (c *ChangeContext) CreateScheduledSubmitFlow(changeNum int, customTime string, tz string) (ScheduledSubmitFlow, error) {
	return c.createScheduledSubmitFlow(changeNum, customTime, "", tz)
}

// createScheduledSubmitFlow is CreateScheduledSubmitFlow with an explicit day
// for the flow condition ("today", "tomorrow", or a weekday name). An empty
// day means the next occurrence of the time, as ScheduleDay picks it.
func (c *ChangeContext) createScheduledSubmitFlow(changeNum int, customTime string, day string, tz string) (ScheduledSubmitFlow, error) {
	if c == nil {
		return ScheduledSubmitFlow{}, fmt.Errorf("internal error: ChangeContext is nil")
	}
	if c.Client == nil {
		return ScheduledSubmitFlow{}, fmt.Errorf("internal error: gerrit client is nil")
	}
	if tz == "" {
		tz = DefaultScheduleTimezone
	}

	// The change's labels are required, not optional: they name the
	// labels the flows vote (see resolveFlowVotes).
	change, err := c.GetChange(&gerrit.ChangeOptions{
		AdditionalFields: []string{"DETAILED_LABELS"},
	})
	if err != nil {
		return ScheduledSubmitFlow{}, err
	}
	if changeNum <= 0 {
		changeNum = change.Number
	}
	if changeNum <= 0 {
		return ScheduledSubmitFlow{}, fmt.Errorf("could not resolve numeric change number for %s", c.ChangeID)
	}
	if change.Number <= 0 {
		change.Number = changeNum
	}

	votes, err := resolveFlowVotes(change)
	if err != nil {
		return ScheduledSubmitFlow{}, err
	}

	targetTime, err := ComputeScheduleTime(changeNum, customTime)
	if err != nil {
		return ScheduledSubmitFlow{}, err
	}
	dayWord := day
	if dayWord == "" {
		dayWord = ScheduleDay(targetTime, tz)
	}
	scheduledWhen := fmt.Sprintf("%s at %s", dayWord, targetTime)

	var cleanHost string
	if c.Config != nil {
		cleanHost = c.Config.GerritHost(c.Context)
	}
	if cleanHost == "" && c.Client != nil {
		baseURL := c.Client.BaseURL()
		cleanHost = CleanGerritHost(baseURL.String())
	}
	if cleanHost == "" {
		return ScheduledSubmitFlow{}, fmt.Errorf("could not determine Gerrit host for flow condition")
	}

	// Checked before anything on the change is modified, so a host without
	// flows is not left with a change marked ready and no schedule.
	if err := checkFlowsEnabled(c.Context, c.Client, cleanHost, changeNum); err != nil {
		return ScheduledSubmitFlow{}, err
	}

	// Scheduling a submit says the change is done, so take it out of work in
	// progress. This happens before any flow is touched, so a failure here
	// leaves the old schedule, if any, in place.
	markedReady := false
	if change.WorkInProgress {
		if _, err := c.Client.Changes.SetReadyForReview(c.Context, strconv.Itoa(changeNum), &gerrit.ReadyForReviewInput{}); err != nil {
			return ScheduledSubmitFlow{}, c.FormatError(err, "marking ready for review")
		}
		markedReady = true
	}

	replaced, err := c.deleteScheduledSubmitFlows(changeNum, votes)
	if err != nil {
		return ScheduledSubmitFlow{}, err
	}

	// Each vote is its own stage: a flow's vote action takes exactly one
	// label vote.
	condition := fmt.Sprintf(
		"https://%s/c/%d is is:open, wait until %s in %s",
		cleanHost, changeNum, scheduledWhen, tz,
	)
	payload := FlowInput{
		StageExpressions: []FlowStageExpression{
			{
				Condition: condition,
				Action: FlowAction{
					Name:       "vote",
					Parameters: []string{votes.DryRun},
				},
			},
			{
				Condition: condition,
				Action: FlowAction{
					Name:       "vote",
					Parameters: []string{votes.AutoSubmit},
				},
			},
		},
	}

	endpoint := fmt.Sprintf("changes/%d/flows", changeNum)
	req, err := c.Client.NewRequest(c.Context, "POST", endpoint, payload)
	if err != nil {
		return ScheduledSubmitFlow{}, c.FormatError(err, "creating submit flow on")
	}
	if _, err := c.Client.Do(req, nil); err != nil {
		return ScheduledSubmitFlow{}, c.FormatError(err, "creating submit flow on")
	}

	return ScheduledSubmitFlow{ChangeNum: changeNum, When: scheduledWhen, TZ: tz, Replaced: replaced, MarkedReady: markedReady}, nil
}

// isScheduledSubmitFlow reports whether flow is one CreateScheduledSubmitFlow
// created and has not finished: two "wait until" stages that cast votes.DryRun
// and then votes.AutoSubmit, with at least one still PENDING.
//
// Flows are matched by shape because Gerrit stores nothing else that ties a
// flow to the command that made it. The shape is specific enough that a
// hand-made flow matching it would do the same thing anyway. Ownership is not
// checked: only a change's owner can create flows on it, so every flow on the
// change belongs to the caller, who must be the owner to create new ones.
func isScheduledSubmitFlow(flow FlowInfo, votes flowVotes) bool {
	if len(flow.Stages) != 2 {
		return false
	}
	wantVotes := []string{votes.DryRun, votes.AutoSubmit}
	pending := false
	for i, stage := range flow.Stages {
		if stage.State == "PENDING" {
			pending = true
		}
		if !strings.Contains(stage.Expression.Condition, "wait until") {
			return false
		}
		if stage.Expression.Action.Name != "vote" {
			return false
		}
		params := stage.Expression.Action.parameterValues()
		if len(params) != 1 || params[0] != wantVotes[i] {
			return false
		}
	}
	return pending
}

// deleteScheduledSubmitFlows deletes the unfinished scheduled submit flows on
// changeNum and returns how many it deleted.
//
// Without this, every `pr push --nightly` while iterating on a change, and
// every retry after a partial failure, would add two more flows that all fire.
// It runs before any new flow is created, so a failure here can leave fewer
// flows than before but never more.
func (c *ChangeContext) deleteScheduledSubmitFlows(changeNum int, votes flowVotes) (int, error) {
	listEndpoint := fmt.Sprintf("changes/%d/flows", changeNum)
	req, err := c.Client.NewRequest(c.Context, "GET", listEndpoint, nil)
	if err != nil {
		return 0, c.FormatError(err, "listing existing flows on")
	}
	var existing []FlowInfo
	if _, err := c.Client.Do(req, &existing); err != nil {
		return 0, c.FormatError(err, "listing existing flows on")
	}

	deleted := 0
	for _, flow := range existing {
		if flow.UUID == "" || !isScheduledSubmitFlow(flow, votes) {
			continue
		}
		delEndpoint := fmt.Sprintf("changes/%d/flows/%s", changeNum, url.PathEscape(flow.UUID))
		req, err := c.Client.NewRequest(c.Context, "DELETE", delEndpoint, nil)
		if err != nil {
			return deleted, c.FormatError(err, "deleting previously scheduled submit flow on")
		}
		if _, err := c.Client.Do(req, nil); err != nil {
			return deleted, c.FormatError(err, "deleting previously scheduled submit flow on")
		}
		deleted++
	}
	return deleted, nil
}

// FlowsNotEnabledError reports that Gerrit Flows are not available for a
// change, so --nightly / --submit-at cannot schedule its submission.
type FlowsNotEnabledError struct {
	Host      string
	ChangeNum int
	// Reason says how Gerrit indicated that flows are unavailable.
	Reason string
}

func (e *FlowsNotEnabledError) Error() string {
	root := RootCmd.CommandPath()
	return fmt.Sprintf("--nightly/--submit-at: Gerrit Flows are not enabled on %s (%s), so a submit cannot be scheduled for change %d.\n\n"+
		"To have it submitted once it is ready instead:\n"+
		"  %s pr merge %d --auto\n"+
		"Or to submit via the Commit Queue now:\n"+
		"  %s pr merge %d --cq",
		e.Host, e.Reason, e.ChangeNum, root, e.ChangeNum, root, e.ChangeNum)
}

// isFlowsEnabledInfo is the response of GET /changes/<id>/is-flows-enabled.
type isFlowsEnabledInfo struct {
	Enabled bool `json:"enabled"`
}

// checkFlowsEnabled returns a *FlowsNotEnabledError if Gerrit Flows are not
// available for changeNum on host, and nil if they are.
//
// A flow is accepted when it is created but only acts hours later, so this is
// asked up front rather than inferred from a later failure. Gerrit reports
// flows as unavailable in three ways: {"enabled": false}; 405, when no flow
// service is bound; and 404, from servers older than Gerrit 3.13, which do not
// have the endpoint. 404 is only read that way because callers have already
// fetched the change, so it cannot mean the change is missing. Any other
// failure is returned as is rather than guessed to mean flows are disabled.
func checkFlowsEnabled(ctx context.Context, client *gerrit.Client, host string, changeNum int) error {
	endpoint := fmt.Sprintf("changes/%d/is-flows-enabled", changeNum)
	const action = "checking whether Gerrit Flows are enabled for"
	req, err := client.NewRequest(ctx, "GET", endpoint, nil)
	if err != nil {
		return FormatGerritError(err, action, strconv.Itoa(changeNum), host)
	}
	var info isFlowsEnabledInfo
	resp, err := client.Do(req, &info)
	if err != nil {
		if resp != nil {
			switch resp.StatusCode {
			case http.StatusMethodNotAllowed:
				return &FlowsNotEnabledError{Host: host, ChangeNum: changeNum, Reason: "no flow service is installed"}
			case http.StatusNotFound:
				return &FlowsNotEnabledError{Host: host, ChangeNum: changeNum, Reason: "the server predates Gerrit 3.13, which added flows"}
			}
		}
		return FormatGerritError(err, action, strconv.Itoa(changeNum), host)
	}
	if !info.Enabled {
		return &FlowsNotEnabledError{Host: host, ChangeNum: changeNum, Reason: "Gerrit reports flows as not enabled for this change"}
	}
	return nil
}

// ScheduleFlowError reports that the push (or metadata update) a scheduled
// submission was attached to succeeded, but creating its Gerrit Flows failed.
//
// It is its own type so callers do not report it as a failed push. Saying
// "error pushing patchset" when the patchset is already on Gerrit sends the
// user to push again, which creates nothing new and, if one flow was created
// before the failure, schedules a duplicate of it.
type ScheduleFlowError struct {
	// Done describes what already succeeded, e.g. "patchset pushed".
	Done string
	// Scheduled and Total count the stack changes scheduled before the
	// failure, when scheduling a stack. Total is 0 for a single change.
	Scheduled, Total int
	// Retry is the command that schedules the flows without pushing again.
	Retry string
	Err   error
}

func (e *ScheduleFlowError) Error() string {
	done := e.Done
	if e.Scheduled > 0 {
		done = fmt.Sprintf("%s and %d of %d changes in the stack were scheduled", done, e.Scheduled, e.Total)
	}
	return fmt.Sprintf("%s, but scheduling the submit flow failed: %v\n\n"+
		"The push succeeded; only the schedule is missing. To retry just the schedule\n"+
		"without pushing again:\n"+
		"  %s", done, e.Err, e.Retry)
}

func (e *ScheduleFlowError) Unwrap() error { return e.Err }

// scheduleRetryCommand returns the `pr merge` invocation that creates the same
// flows as a push with opts, for use in ScheduleFlowError. changeID may be
// empty, in which case `pr merge` resolves the change from the current branch.
func scheduleRetryCommand(opts PushOptions, changeID string) string {
	cmd := RootCmd.CommandPath() + " pr merge"
	if opts.ChangeNumber > 0 {
		cmd += fmt.Sprintf(" %d", opts.ChangeNumber)
	} else if changeID != "" {
		cmd += " " + changeID
	}
	if opts.SubmitAt != "" {
		return cmd + " --submit-at " + opts.SubmitAt
	}
	return cmd + " --nightly"
}

// scheduleSubmitFlowForPush creates the scheduled submit flows requested by
// --nightly or --submit-at once the push or metadata update has succeeded.
// done describes that success, for the error message if scheduling fails.
// chCtx may be nil, in which case the change is resolved from HEAD. If
// opts.StackChangeIDs is set, every change in the stack is scheduled instead
// (see scheduleSubmitFlowForStack).
func scheduleSubmitFlowForPush(cmd *cobra.Command, chCtx *ChangeContext, opts PushOptions, done string) error {
	if !opts.Nightly && opts.SubmitAt == "" {
		return nil
	}
	if len(opts.StackChangeIDs) > 1 {
		return scheduleSubmitFlowForStack(cmd, opts, done)
	}
	if chCtx == nil {
		var err error
		chCtx, err = ResolveChangeContext(cmd, nil)
		if err != nil {
			return &ScheduleFlowError{Done: done, Retry: scheduleRetryCommand(opts, ""), Err: err}
		}
	}
	scheduled, err := chCtx.CreateScheduledSubmitFlow(opts.ChangeNumber, opts.SubmitAt, DefaultScheduleTimezone)
	if err != nil {
		return &ScheduleFlowError{Done: done, Retry: scheduleRetryCommand(opts, chCtx.ChangeID), Err: err}
	}
	fmt.Fprintln(cmd.OutOrStdout(), scheduled.Summary())
	return nil
}

// stackScheduleSpacing is how far apart --nightly and --submit-at schedule
// consecutive changes in a stack.
const stackScheduleSpacing = 30 * time.Minute

// scheduleSubmitFlowForStack schedules a submit flow on every change in
// opts.StackChangeIDs (bottom first), spread out rather than all at once.
//
// Each change needs its own flow, because the votes a flow casts apply only to
// the change it is on. The bottom change gets the requested --submit-at time,
// or with --nightly its own nightly slot, and each change above it
// stackScheduleSpacing later. With --nightly, the slots are not taken from
// each change's own number: consecutive change numbers would land a minute
// apart, in no particular order. Times past the end of the nightly window are
// kept rather than squeezed into it.
func scheduleSubmitFlowForStack(cmd *cobra.Command, opts PushOptions, done string) error {
	ids := opts.StackChangeIDs
	// With --nightly, slots stays empty ("--nightly" in retry commands) until
	// the bottom change's slot is known.
	slots := make([]scheduleSlot, len(ids))
	if opts.SubmitAt != "" {
		var err error
		if slots, err = stackScheduleSlots(nil, opts.SubmitAt, len(ids), DefaultScheduleTimezone); err != nil {
			// Keep --submit-at in the retry commands rather than falling
			// back to --nightly, even though the time needs fixing.
			fallback := make([]scheduleSlot, len(ids))
			for i := range fallback {
				fallback[i].Time = opts.SubmitAt
			}
			return &ScheduleFlowError{Done: done, Total: len(ids), Retry: stackRetryCommands(ids, fallback), Err: err}
		}
	}

	for i, id := range ids {
		chCtx, err := ResolveChangeContext(cmd, []string{id})
		if err == nil && i == 0 && opts.SubmitAt == "" {
			var nightly []scheduleSlot
			if nightly, err = stackScheduleSlots(chCtx, "", len(ids), DefaultScheduleTimezone); err == nil {
				slots = nightly
			}
		}
		if err == nil {
			var scheduled ScheduledSubmitFlow
			if scheduled, err = chCtx.createScheduledSubmitFlow(0, slots[i].Time, slots[i].Day, DefaultScheduleTimezone); err == nil {
				fmt.Fprintln(cmd.OutOrStdout(), scheduled.Summary())
				continue
			}
		}
		return &ScheduleFlowError{Done: done, Scheduled: i, Total: len(ids), Retry: stackRetryCommands(ids[i:], slots[i:]), Err: err}
	}
	return nil
}

// scheduleSlot is when one change in a stack is scheduled.
type scheduleSlot struct {
	// Time is "HH:MM", or "" for the change's own --nightly slot.
	Time string
	// Day is "today", "tomorrow", or a weekday name, or "" to let
	// ScheduleDay pick the next occurrence of Time.
	Day string
}

// stackScheduleSlots returns when each of n stacked changes is scheduled,
// bottom first: at submitAt for the bottom change, or if submitAt is empty, at
// the nightly slot of bottom (the bottom change), then each
// stackScheduleSpacing later.
//
// The bottom change gets the next occurrence of its time in tz, and every
// change above it is counted from that moment, so the days follow the stack
// order. Picking "today" or "tomorrow" for each time on its own would not: at
// 21:45 with the bottom change at 21:30, the bottom change would go to
// tomorrow 21:30 but the next one to today 22:00, almost a day early. A
// change two or more days out is named by its weekday.
//
// If tz cannot be loaded, Day is left empty and each change falls back to
// ScheduleDay.
func stackScheduleSlots(bottom *ChangeContext, submitAt string, n int, tz string) ([]scheduleSlot, error) {
	base := submitAt
	if base != "" {
		// Same validation and zero-padding as a single change's --submit-at.
		var err error
		if base, err = ValidateScheduleTime(base); err != nil {
			return nil, err
		}
	} else {
		change, err := bottom.GetChange(nil)
		if err != nil {
			return nil, err
		}
		if base, err = ComputeScheduleTime(change.Number, ""); err != nil {
			return nil, err
		}
	}
	hhmm, err := time.Parse("15:04", base)
	if err != nil {
		return nil, fmt.Errorf("invalid time %q: %w", base, err)
	}

	slots := make([]scheduleSlot, n)
	loc, err := time.LoadLocation(tz)
	if err != nil {
		// OK: without time zone data, fall back to each change's own
		// ScheduleDay, as before stacks were counted from the bottom change.
		slog.Debug("stack schedule: cannot load time zone", "tz", tz, "err", err)
		for i := range slots {
			t, err := addScheduleMinutes(base, time.Duration(i)*stackScheduleSpacing)
			if err != nil {
				return nil, err
			}
			slots[i] = scheduleSlot{Time: t}
		}
		return slots, nil
	}

	now := scheduleNowFn().In(loc)
	first := time.Date(now.Year(), now.Month(), now.Day(), hhmm.Hour(), hhmm.Minute(), 0, 0, loc)
	if !first.After(now) {
		first = first.AddDate(0, 0, 1)
	}
	for i := range slots {
		at := first.Add(time.Duration(i) * stackScheduleSpacing)
		slots[i] = scheduleSlot{Time: at.Format("15:04"), Day: scheduleDayWord(now, at)}
	}
	return slots, nil
}

// scheduleDayWord names the day of at relative to now, both in the same
// location: "today", "tomorrow", or the weekday name for later days.
func scheduleDayWord(now, at time.Time) string {
	y, m, d := now.Date()
	today := time.Date(y, m, d, 0, 0, 0, 0, time.UTC)
	y, m, d = at.Date()
	days := int(time.Date(y, m, d, 0, 0, 0, 0, time.UTC).Sub(today).Hours() / 24)
	switch days {
	case 0:
		return "today"
	case 1:
		return "tomorrow"
	}
	return at.Weekday().String()
}

// addScheduleMinutes returns the "HH:MM" time d after hhmm, wrapping past
// midnight.
func addScheduleMinutes(hhmm string, d time.Duration) (string, error) {
	t, err := time.Parse("15:04", hhmm)
	if err != nil {
		return "", fmt.Errorf("invalid time %q: %w", hhmm, err)
	}
	return t.Add(d).Format("15:04"), nil
}

// stackRetryCommands returns one `pr merge` invocation per change in ids,
// joined for display in ScheduleFlowError. slots[i].Time is the --submit-at
// time for ids[i], or "" for --nightly.
func stackRetryCommands(ids []string, slots []scheduleSlot) string {
	cmds := make([]string, len(ids))
	for i, id := range ids {
		sched := " --nightly"
		if slots[i].Time != "" {
			sched = " --submit-at " + slots[i].Time
		}
		cmds[i] = RootCmd.CommandPath() + " pr merge " + id + sched
	}
	return strings.Join(cmds, "\n  ")
}
