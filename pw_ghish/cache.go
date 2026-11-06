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
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// cacheEnvelopeVersion versions the outer cacheFile JSON wrapper (not the
// per-cache value schema V). Pre-existing unversioned files unmarshal as 0 and
// are treated as a benign miss that heals on the next write.
const cacheEnvelopeVersion = 1

// CacheTier classifies where cached values are permitted to live.
// Moving a type to a broader tier changes its security boundary.
type CacheTier uint8

const (
	// CacheInMemory keeps entries strictly in process memory and never
	// serializes them to disk.
	CacheInMemory CacheTier = iota + 1
	// CacheRuntime stores short-lived session state across processes in the
	// per-user 0700 runtime directory ($XDG_RUNTIME_DIR/pw_ghish on Linux,
	// $TMPDIR/pw_ghish on macOS), degrading to CacheInMemory when
	// unavailable or disabled. Requires MaxTTL > 0.
	CacheRuntime
	// CachePersistent stores non-secret metadata in the user cache directory
	// ($GHISH_CACHE_DIR or $XDG_CACHE_HOME/pw_ghish / os.UserCacheDir),
	// degrading to in-memory when unavailable or disabled.
	CachePersistent
)

// CachePolicy declares the storage tier, unique name, TTL ceiling, and optional
// per-cache disable environment variable for a Cacheable value type.
type CachePolicy struct {
	Tier   CacheTier
	Name   string        // Unique cache name; "<Name>.json" on disk for persistent tiers.
	MaxTTL time.Duration // 0 means no time-based expiry.
	// DisableEnv names an optional env var that disables disk persistence for
	// disk-backed tiers when set to "0".
	DisableEnv string
}

// Cacheable is implemented on a value receiver by every type stored in a Cache,
// binding each data type to its storage tier and TTL ceiling at its definition.
type Cacheable interface {
	CachePolicy() CachePolicy
}

type registeredCache struct {
	Policy    CachePolicy
	ValueType reflect.Type
	reset     func()
}

var (
	cacheRegistryMu sync.Mutex
	cacheRegistry   []registeredCache
)

func validateCachePolicy(p CachePolicy) {
	if p.Name == "" || p.Name != filepath.Base(p.Name) || strings.ContainsAny(p.Name, "/\\ \t\r\n") {
		panic(fmt.Sprintf("invalid CachePolicy.Name %q", p.Name))
	}
	if p.MaxTTL < 0 {
		panic(fmt.Sprintf("cache %q: negative MaxTTL %v", p.Name, p.MaxTTL))
	}
	switch p.Tier {
	case CacheInMemory:
		if p.DisableEnv != "" {
			panic(fmt.Sprintf("cache %q: CacheInMemory does not support DisableEnv (%q)", p.Name, p.DisableEnv))
		}
	case CachePersistent:
	case CacheRuntime:
		if p.MaxTTL <= 0 {
			panic(fmt.Sprintf("cache %q: CacheRuntime requires MaxTTL > 0", p.Name))
		}
	default:
		panic(fmt.Sprintf("cache %q: unknown CacheTier %d", p.Name, p.Tier))
	}
}

func registerCache(entry registeredCache) {
	cacheRegistryMu.Lock()
	defer cacheRegistryMu.Unlock()
	for _, existing := range cacheRegistry {
		if existing.Policy.Name == entry.Policy.Name {
			panic(fmt.Sprintf("duplicate cache registration for name %q", entry.Policy.Name))
		}
		if entry.Policy.DisableEnv != "" && existing.Policy.DisableEnv == entry.Policy.DisableEnv {
			panic(fmt.Sprintf("duplicate cache DisableEnv %q (%s and %s)",
				entry.Policy.DisableEnv, existing.Policy.Name, entry.Policy.Name))
		}
	}
	cacheRegistry = append(cacheRegistry, entry)
}

func resetAllCaches() {
	cacheRegistryMu.Lock()
	resets := make([]func(), len(cacheRegistry))
	for i, entry := range cacheRegistry {
		resets[i] = entry.reset
	}
	cacheRegistryMu.Unlock()
	for _, reset := range resets {
		reset()
	}
}

// cacheEnv supplies the clock and root directory resolvers for disk-backed
// caches (<dir>/<Policy.Name>.json). Production initializes activeCacheEnv in
// init(); hermetic tests swap it in TestMain or per test via WithTestCaches.
type cacheEnv struct {
	Now func() time.Time
	// RuntimeDir returns the root directory for CacheRuntime JSON files
	// (e.g. $XDG_RUNTIME_DIR/pw_ghish), or "" to keep CacheRuntime entries
	// strictly in memory without touching disk.
	RuntimeDir func() string
	// PersistentDir returns the root directory for CachePersistent JSON files
	// (e.g. ~/.cache/pw_ghish), or "" to keep CachePersistent entries strictly
	// in memory without touching disk.
	PersistentDir func() string
}

var activeCacheEnv atomic.Pointer[cacheEnv]

func init() {
	env := cacheEnv{
		Now:           time.Now,
		RuntimeDir:    defaultRuntimeCacheDir,
		PersistentDir: productionPersistentCacheDir,
	}
	activeCacheEnv.Store(&env)
}

func currentCacheEnv() cacheEnv {
	return *activeCacheEnv.Load()
}

func setCacheEnv(env cacheEnv) cacheEnv {
	if env.Now == nil {
		env.Now = time.Now
	}
	if env.RuntimeDir == nil {
		env.RuntimeDir = func() string { return "" }
	}
	if env.PersistentDir == nil {
		env.PersistentDir = func() string { return "" }
	}
	prev := activeCacheEnv.Swap(&env)
	return *prev
}

// defaultRuntimeCacheDir returns <runtime_base>/pw_ghish when the platform
// runtime directory ($XDG_RUNTIME_DIR on Linux, $TMPDIR on macOS) is an
// absolute regular directory with mode 0700, or "" when CacheRuntime entries
// must stay in memory.
func defaultRuntimeCacheDir() string {
	var envVar string
	switch runtimeGOOS {
	case "linux":
		envVar = "XDG_RUNTIME_DIR"
	case "darwin":
		envVar = "TMPDIR"
	default:
		return ""
	}
	base := os.Getenv(envVar)
	if base == "" {
		slog.Debug("runtime cache disabled: runtime directory variable is unset", "var", envVar)
		return ""
	}
	if !filepath.IsAbs(base) {
		slog.Debug("runtime cache disabled: runtime directory is not absolute", "var", envVar, "dir", base)
		return ""
	}
	cleanBase := filepath.Clean(base)
	info, err := os.Lstat(cleanBase)
	if err != nil {
		// OK: runtime cache is best-effort; callers degrade to in-memory.
		slog.Debug("runtime cache disabled: cannot stat runtime directory", "dir", cleanBase, "err", err)
		return ""
	}
	if !info.IsDir() {
		slog.Debug("runtime cache disabled: runtime directory is not a regular directory", "dir", cleanBase, "mode", info.Mode())
		return ""
	}
	if perm := info.Mode().Perm(); perm != 0o700 {
		slog.Debug("runtime cache disabled: runtime directory is not mode 0700", "dir", cleanBase, "perm", perm)
		return ""
	}
	return filepath.Join(cleanBase, "pw_ghish")
}

func productionPersistentCacheDir() string {
	if strings.TrimSpace(os.Getenv("GHISH_CACHE")) == "0" {
		return ""
	}
	if dir := strings.TrimSpace(os.Getenv("GHISH_CACHE_DIR")); dir != "" {
		return dir
	}
	if xdg := strings.TrimSpace(os.Getenv("XDG_CACHE_HOME")); xdg != "" {
		return filepath.Join(xdg, "pw_ghish")
	}
	if ucd, err := os.UserCacheDir(); err == nil && ucd != "" {
		return filepath.Join(ucd, "pw_ghish")
	}
	if home, err := os.UserHomeDir(); err == nil && home != "" {
		return filepath.Join(home, ".cache", "pw_ghish")
	}
	return ""
}

type cacheEntry[V any] struct {
	Value     V         `json:"value"`
	ExpiresAt time.Time `json:"expires_at,omitzero"`
}

// cacheStore persists entries behind Cache's in-memory map. Concrete
// implementations (noopStore and diskStore) are comparable value types and
// absorb I/O failures internally as best-effort operations.
type cacheStore[K comparable, V any] interface {
	load() map[K]cacheEntry[V]
	save(entries map[K]cacheEntry[V])
}

type noopStore[K comparable, V any] struct{}

func (noopStore[K, V]) load() map[K]cacheEntry[V] { return nil }
func (noopStore[K, V]) save(map[K]cacheEntry[V])  {}

// Cache is a thread-safe, TTL-aware key-value cache backed by the storage tier
// declared on V.CachePolicy(). String-keyed caches treat "" as a no-op miss.
type Cache[K comparable, V Cacheable] struct {
	policy CachePolicy // immutable after construction

	mu    sync.Mutex
	store cacheStore[K, V] // nil until first access or after ResetMemory
	mem   map[K]cacheEntry[V]
}

func isEmptyStringKey[K comparable](key K) bool {
	s, ok := any(key).(string)
	return ok && s == ""
}

// policyFor extracts and validates V's static CachePolicy. Go interfaces
// dispatch on values rather than types, so we call CachePolicy on a
// default-initialized V.
func policyFor[V Cacheable]() CachePolicy {
	var v V
	p := v.CachePolicy()
	validateCachePolicy(p)
	return p
}

func newDetachedCache[K comparable, V Cacheable]() *Cache[K, V] {
	policy := policyFor[V]()
	if policy.Tier != CacheInMemory && reflect.TypeFor[K]() != reflect.TypeFor[string]() {
		panic(fmt.Sprintf("cache %q: disk-backed tier requires string keys, got %v", policy.Name, reflect.TypeFor[K]()))
	}
	return &Cache[K, V]{
		policy: policy,
		mem:    make(map[K]cacheEntry[V]),
	}
}

// NewCache constructs and registers a Cache[K, V] whose backing tier and TTL
// rules come from V.CachePolicy(). Disk-backed tiers require K to be string;
// CacheInMemory accepts any comparable key type.
func NewCache[K comparable, V Cacheable]() *Cache[K, V] {
	c := newDetachedCache[K, V]()
	registerCache(registeredCache{
		Policy:    c.policy,
		ValueType: reflect.TypeFor[V](),
		reset:     c.ResetMemory,
	})
	return c
}

func (c *Cache[K, V]) disabledByEnv() bool {
	return c.policy.DisableEnv != "" && strings.TrimSpace(os.Getenv(c.policy.DisableEnv)) == "0"
}

func (c *Cache[K, V]) resolveStore(env cacheEnv) cacheStore[K, V] {
	if c.disabledByEnv() {
		return noopStore[K, V]{}
	}
	var dir string
	switch c.policy.Tier {
	case CacheRuntime:
		dir = env.RuntimeDir()
	case CachePersistent:
		dir = env.PersistentDir()
	}
	if dir == "" {
		return noopStore[K, V]{}
	}
	return diskStore[K, V]{dir: dir, name: c.policy.Name + ".json"}
}

// bindStoreLocked lazily binds c.store on first use (or re-binds when test
// overrides change the active cache directory) and hydrates c.mem once.
func (c *Cache[K, V]) bindStoreLocked(env cacheEnv, now time.Time) cacheStore[K, V] {
	store := c.resolveStore(env)
	if c.store != store {
		c.store = store
		clear(c.mem)
		for k, e := range store.load() {
			if c.entryValidAt(e, now) {
				c.mem[k] = e
			}
		}
	}
	return store
}

func (c *Cache[K, V]) entryValidAt(e cacheEntry[V], now time.Time) bool {
	if c.policy.MaxTTL > 0 {
		if e.ExpiresAt.IsZero() || !e.ExpiresAt.After(now) {
			return false
		}
		if e.ExpiresAt.After(now.Add(c.policy.MaxTTL)) {
			return false
		}
		return true
	}
	if !e.ExpiresAt.IsZero() && !e.ExpiresAt.After(now) {
		return false
	}
	return true
}

// computeExpiry clamps a caller-supplied expiresAt (zero when unspecified)
// against Policy.MaxTTL, returning ok=false if expiresAt is already past now.
func (c *Cache[K, V]) computeExpiry(expiresAt, now time.Time) (time.Time, bool) {
	if !expiresAt.IsZero() && !expiresAt.After(now) {
		return time.Time{}, false
	}
	if c.policy.MaxTTL <= 0 {
		return expiresAt, true
	}
	maxExp := now.Add(c.policy.MaxTTL)
	if !expiresAt.IsZero() && expiresAt.Before(maxExp) {
		return expiresAt, true
	}
	return maxExp, true
}

func (c *Cache[K, V]) pruneInvalidLocked(now time.Time) {
	for k, e := range c.mem {
		if !c.entryValidAt(e, now) {
			delete(c.mem, k)
		}
	}
}

// Get returns key's unexpired value, if present.
func (c *Cache[K, V]) Get(key K) (V, bool) {
	var zero V
	if isEmptyStringKey(key) {
		return zero, false
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	env := currentCacheEnv()
	now := env.Now()
	c.bindStoreLocked(env, now)
	entry, ok := c.mem[key]
	if !ok {
		return zero, false
	}
	if !c.entryValidAt(entry, now) {
		delete(c.mem, key)
		return zero, false
	}
	return entry.Value, true
}

// Put stores val under key using Policy.MaxTTL (or no time-based expiry when
// MaxTTL == 0).
func (c *Cache[K, V]) Put(key K, val V) {
	c.PutUntil(key, val, time.Time{})
}

// PutUntil stores val under key expiring at min(expiresAt, now+MaxTTL) (or
// now+MaxTTL when expiresAt is zero). If expiresAt is non-zero and already at
// or before now, any existing entry for key is removed instead.
func (c *Cache[K, V]) PutUntil(key K, val V, expiresAt time.Time) {
	if isEmptyStringKey(key) {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	env := currentCacheEnv()
	now := env.Now()
	store := c.bindStoreLocked(env, now)
	c.pruneInvalidLocked(now)
	exp, valid := c.computeExpiry(expiresAt, now)
	if !valid {
		if _, ok := c.mem[key]; ok {
			delete(c.mem, key)
			store.save(c.mem)
		}
		return
	}
	c.mem[key] = cacheEntry[V]{Value: val, ExpiresAt: exp}
	store.save(c.mem)
}

// PutIfAbsent stores val under key (using Policy.MaxTTL) only when no
// unexpired entry exists for key, returning true if val was inserted.
func (c *Cache[K, V]) PutIfAbsent(key K, val V) bool {
	if isEmptyStringKey(key) {
		return false
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	env := currentCacheEnv()
	now := env.Now()
	store := c.bindStoreLocked(env, now)
	c.pruneInvalidLocked(now)
	if _, exists := c.mem[key]; exists {
		return false
	}
	exp, _ := c.computeExpiry(time.Time{}, now)
	c.mem[key] = cacheEntry[V]{Value: val, ExpiresAt: exp}
	store.save(c.mem)
	return true
}

// GetOrLoad returns key's unexpired value if present, or runs load under c.mu
// to compute and cache a new value (expiring at min(expiresAt, now+MaxTTL)).
// Concurrent callers share a single load call; load must not re-enter c.
func (c *Cache[K, V]) GetOrLoad(
	ctx context.Context,
	key K,
	load func(context.Context) (val V, expiresAt time.Time, err error),
) (V, error) {
	var zero V
	if isEmptyStringKey(key) {
		return zero, nil
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	if err := ctx.Err(); err != nil {
		return zero, err
	}
	env := currentCacheEnv()
	now := env.Now()
	c.bindStoreLocked(env, now)
	if entry, ok := c.mem[key]; ok {
		if c.entryValidAt(entry, now) {
			return entry.Value, nil
		}
		delete(c.mem, key)
	}

	val, expiresAt, err := load(ctx)
	if err != nil {
		return zero, err
	}
	if err := ctx.Err(); err != nil {
		return zero, err
	}
	// Re-read the clock after load, which may run an external command.
	now = env.Now()
	if exp, valid := c.computeExpiry(expiresAt, now); valid {
		store := c.bindStoreLocked(env, now)
		c.pruneInvalidLocked(now)
		c.mem[key] = cacheEntry[V]{Value: val, ExpiresAt: exp}
		store.save(c.mem)
	}
	return val, nil
}

// Delete removes key from the cache if present.
func (c *Cache[K, V]) Delete(key K) {
	c.DeleteIf(key, func(V) bool { return true })
}

// DeleteIf removes key from the cache if match returns true for its value,
// returning whether it was removed.
func (c *Cache[K, V]) DeleteIf(key K, match func(V) bool) bool {
	if isEmptyStringKey(key) {
		return false
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	env := currentCacheEnv()
	now := env.Now()
	store := c.bindStoreLocked(env, now)
	c.pruneInvalidLocked(now)
	cur, ok := c.mem[key]
	if !ok || !match(cur.Value) {
		return false
	}
	delete(c.mem, key)
	store.save(c.mem)
	return true
}

// ResetMemory clears the in-memory entries and detaches the bound store so the
// next operation reloads from the active cache environment.
func (c *Cache[K, V]) ResetMemory() {
	c.mu.Lock()
	c.store = nil
	clear(c.mem)
	c.mu.Unlock()
}

// cacheFile is the on-disk JSON envelope written to <dir>/<Policy.Name>.json.
type cacheFile[V any] struct {
	Version int                      `json:"version"` // cacheEnvelopeVersion
	Entries map[string]cacheEntry[V] `json:"entries"`
}

type diskStore[K comparable, V any] struct {
	dir  string
	name string
}

// load best-effort reads and unmarshals <dir>/<name>, returning nil when the
// file does not exist yet or fails validation.
func (s diskStore[K, V]) load() map[K]cacheEntry[V] {
	data, err := readConfinedFile(s.dir, s.name)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			// OK: best-effort disk cache degrades to a cold miss
			slog.Debug("ignoring unreadable cache file", "dir", s.dir, "file", s.name, "err", err)
		}
		return nil
	}
	var parsed cacheFile[V]
	if err := json.Unmarshal(data, &parsed); err != nil || parsed.Version != cacheEnvelopeVersion {
		// OK: best-effort disk cache treats corrupt or legacy envelopes as a miss
		slog.Debug("ignoring invalid cache file", "dir", s.dir, "file", s.name, "version", parsed.Version, "err", err)
		return nil
	}
	delete(parsed.Entries, "")
	return any(parsed.Entries).(map[K]cacheEntry[V])
}

func (s diskStore[K, V]) save(entries map[K]cacheEntry[V]) {
	data, err := json.Marshal(cacheFile[V]{
		Version: cacheEnvelopeVersion,
		Entries: any(entries).(map[string]cacheEntry[V]),
	})
	if err != nil {
		// OK: best-effort disk cache keeps in-memory state when marshaling fails
		slog.Debug("failed to marshal cache", "file", s.name, "err", err)
		return
	}
	if err := writeConfinedFileAtomic(s.dir, s.name, data); err != nil {
		// OK: best-effort disk cache keeps in-memory state on read-only or unsafe dirs
		slog.Debug("failed to persist cache file", "dir", s.dir, "file", s.name, "err", err)
	}
}

// openConfinedDir opens dir as an *os.Root without following a symlink on the
// final path component.
func openConfinedDir(dir string) (*os.Root, error) {
	cleanDir := filepath.Clean(dir)
	parentDir, baseName := filepath.Dir(cleanDir), filepath.Base(cleanDir)
	if cleanDir == "" || cleanDir == "." || cleanDir == parentDir {
		return nil, fmt.Errorf("invalid cache directory %q", dir)
	}
	parent, err := os.OpenRoot(parentDir)
	if err != nil {
		return nil, err
	}
	defer func() { _ = parent.Close() }()
	return parent.OpenRoot(baseName)
}

func ensureConfinedDir(dir string) (*os.Root, error) {
	cleanDir := filepath.Clean(dir)
	if info, err := os.Lstat(cleanDir); err == nil && !info.IsDir() {
		return nil, fmt.Errorf("cache path %q is not a regular directory", cleanDir)
	}
	if err := os.MkdirAll(cleanDir, 0o700); err != nil {
		return nil, err
	}
	root, err := openConfinedDir(cleanDir)
	if err != nil {
		return nil, err
	}
	// Windows uses NTFS ACLs rather than POSIX mode bits: Go's Stat always
	// synthesizes 0777 (or 0555) on directories and Chmod only toggles the
	// read-only attribute without modifying ACLs. Persistent caching still
	// works on Windows via *os.Root, relying on inherited per-user ACLs under
	// %LocalAppData% (os.UserCacheDir) rather than enforcing 0700 on custom
	// GHISH_CACHE_DIR paths.
	if runtime.GOOS != "windows" {
		info, err := root.Stat(".")
		if err == nil && info.Mode().Perm()&0o077 != 0 {
			err = root.Chmod(".", 0o700)
		}
		if err != nil {
			_ = root.Close()
			return nil, err
		}
	}
	return root, nil
}

func readConfinedFile(dir, name string) ([]byte, error) {
	root, err := openConfinedDir(dir)
	if err != nil {
		return nil, err
	}
	defer func() { _ = root.Close() }()

	if info, err := root.Lstat(name); err != nil {
		return nil, err
	} else if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%s is not a regular file", name)
	}
	return root.ReadFile(name)
}

func writeConfinedFileAtomic(dir, name string, data []byte) error {
	root, err := ensureConfinedDir(dir)
	if err != nil {
		return err
	}
	defer func() { _ = root.Close() }()

	if info, err := root.Lstat(name); err == nil && !info.Mode().IsRegular() {
		return fmt.Errorf("%s is not a regular file", name)
	}
	tmpName := fmt.Sprintf(".%s-%s.tmp", name, rand.Text())
	if err := root.WriteFile(tmpName, data, 0o600); err != nil {
		_ = root.Remove(tmpName)
		return err
	}
	if err := root.Rename(tmpName, name); err != nil {
		_ = root.Remove(tmpName)
		return err
	}
	return nil
}
