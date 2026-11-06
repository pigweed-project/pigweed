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
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestProductionPersistentCacheDir(t *testing.T) {
	homeFallbackEnv := map[string]string{"HOME": "/home/alice"}
	homeFallbackWant := filepath.Join("/home/alice", ".cache", "pw_ghish")
	switch runtime.GOOS {
	case "darwin":
		homeFallbackWant = filepath.Join("/home/alice", "Library", "Caches", "pw_ghish")
	case "windows":
		winHome := filepath.Join("C:", "Users", "alice")
		homeFallbackEnv = map[string]string{"USERPROFILE": winHome}
		homeFallbackWant = filepath.Join(winHome, ".cache", "pw_ghish")
	}

	for _, tc := range []struct {
		name string
		env  map[string]string
		want string
	}{
		{
			name: "GHISH_CACHE=0 disables cache",
			env:  map[string]string{"GHISH_CACHE": "0", "GHISH_CACHE_DIR": "/override", "XDG_CACHE_HOME": "/xdg"},
			want: "",
		},
		{
			name: "GHISH_CACHE trimmed 0 disables cache",
			env:  map[string]string{"GHISH_CACHE": " 0 ", "GHISH_CACHE_DIR": "/override"},
			want: "",
		},
		{
			name: "GHISH_CACHE_DIR overrides XDG_CACHE_HOME",
			env:  map[string]string{"GHISH_CACHE_DIR": " /override ", "XDG_CACHE_HOME": "/xdg"},
			want: "/override",
		},
		{
			name: "XDG_CACHE_HOME overrides HOME",
			env:  map[string]string{"XDG_CACHE_HOME": "/xdg", "HOME": "/home/alice"},
			want: filepath.Join("/xdg", "pw_ghish"),
		},
		{
			name: "HOME fallback",
			env:  homeFallbackEnv,
			want: homeFallbackWant,
		},
		{
			name: "empty when unset",
			want: "",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			for _, k := range []string{
				"GHISH_CACHE", "GHISH_CACHE_DIR", "XDG_CACHE_HOME", "HOME",
				"LocalAppData", "USERPROFILE", "HOMEDRIVE", "HOMEPATH",
			} {
				t.Setenv(k, tc.env[k])
			}
			if got := productionPersistentCacheDir(); got != tc.want {
				t.Errorf("productionPersistentCacheDir() = %q, want %q", got, tc.want)
			}
		})
	}
}

func TestDefaultRuntimeCacheDir(t *testing.T) {
	for _, goos := range []string{"windows", "freebsd"} {
		withHostRoleForTest(t, goos, "", false)
		t.Setenv("XDG_RUNTIME_DIR", t.TempDir())
		if got := defaultRuntimeCacheDir(); got != "" {
			t.Errorf("defaultRuntimeCacheDir(%s) = %q, want \"\"", goos, got)
		}
	}
	if runtime.GOOS == "windows" {
		t.Skip("Unix 0700 mode checks do not apply on Windows")
	}

	good := t.TempDir()
	if err := os.Chmod(good, 0o700); err != nil {
		t.Fatalf("chmod: %v", err)
	}
	groupReadable := t.TempDir()
	if err := os.Chmod(groupReadable, 0o750); err != nil {
		t.Fatalf("chmod: %v", err)
	}
	notDir := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(notDir, nil, 0o600); err != nil {
		t.Fatalf("write: %v", err)
	}
	symlinkBase := filepath.Join(t.TempDir(), "runtime-link")
	if err := os.Symlink(good, symlinkBase); err != nil {
		t.Fatalf("symlink: %v", err)
	}

	for _, tc := range []struct {
		name    string
		goos    string
		env     map[string]string
		wantDir string
	}{
		{name: "linux XDG_RUNTIME_DIR 0700", goos: "linux", env: map[string]string{"XDG_RUNTIME_DIR": good}, wantDir: filepath.Join(good, "pw_ghish")},
		{name: "linux unset", goos: "linux", env: map[string]string{"XDG_RUNTIME_DIR": ""}},
		{name: "linux relative", goos: "linux", env: map[string]string{"XDG_RUNTIME_DIR": "run/user"}},
		{name: "linux missing", goos: "linux", env: map[string]string{"XDG_RUNTIME_DIR": filepath.Join(good, "missing")}},
		{name: "linux not a directory", goos: "linux", env: map[string]string{"XDG_RUNTIME_DIR": notDir}},
		{name: "linux symlink rejected", goos: "linux", env: map[string]string{"XDG_RUNTIME_DIR": symlinkBase}},
		{name: "linux not 0700", goos: "linux", env: map[string]string{"XDG_RUNTIME_DIR": groupReadable}},
		{name: "linux ignores TMPDIR", goos: "linux", env: map[string]string{"TMPDIR": good}},
		{name: "darwin TMPDIR 0700 with trailing slash", goos: "darwin", env: map[string]string{"TMPDIR": good + "/"}, wantDir: filepath.Join(good, "pw_ghish")},
		{name: "darwin TMPDIR not 0700", goos: "darwin", env: map[string]string{"TMPDIR": groupReadable}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			withHostRoleForTest(t, tc.goos, "", false)
			t.Setenv("XDG_RUNTIME_DIR", "")
			t.Setenv("TMPDIR", "")
			for k, v := range tc.env {
				t.Setenv(k, v)
			}
			if got := defaultRuntimeCacheDir(); got != tc.wantDir {
				t.Errorf("defaultRuntimeCacheDir() = %q, want %q", got, tc.wantDir)
			}
		})
	}
}

type testPersistentItem struct {
	Label string `json:"label"`
}

func (testPersistentItem) CachePolicy() CachePolicy {
	return CachePolicy{
		Tier:       CachePersistent,
		Name:       "test_items",
		MaxTTL:     time.Hour,
		DisableEnv: "GHISH_TEST_ITEMS_CACHE",
	}
}

type testMemKey struct {
	Scope string
	Index int
}

type testMemItem struct {
	Value string
}

func (testMemItem) CachePolicy() CachePolicy {
	return CachePolicy{
		Tier:   CacheInMemory,
		Name:   "test_mem",
		MaxTTL: 10 * time.Minute,
	}
}

type cacheFixture struct {
	t        testing.TB
	dir      string
	filePath string
	now      time.Time
}

func newCacheFixture(t testing.TB, dir string) *cacheFixture {
	t.Helper()
	if dir == "" {
		dir = filepath.Join(t.TempDir(), "ghish_cache")
	}
	fx := &cacheFixture{
		t:        t,
		dir:      dir,
		filePath: filepath.Join(dir, "test_items.json"),
		now:      time.Date(2026, 10, 29, 6, 0, 0, 0, time.UTC),
	}
	WithTestCaches(t, TestCacheConfig{
		PersistentDir: fx.dir,
		Now:           func() time.Time { return fx.now },
	})
	return fx
}

func (fx *cacheFixture) newCache() *Cache[string, testPersistentItem] {
	fx.t.Helper()
	return newDetachedCache[string, testPersistentItem]()
}

func (fx *cacheFixture) assertDirNotCreated() {
	fx.t.Helper()
	if _, err := os.Stat(fx.dir); !errors.Is(err, os.ErrNotExist) {
		fx.t.Fatalf("expected %s not to exist, stat err = %v", fx.dir, err)
	}
}

// readDiskCacheFile asserts 0700 dir and 0600 regular file permissions, no
// leftover atomic .tmp files, and cacheEnvelopeVersion before returning Entries.
func readDiskCacheFile[V any](t testing.TB, path string) map[string]cacheEntry[V] {
	t.Helper()
	dir := filepath.Dir(path)
	dirInfo, err := os.Stat(dir)
	if err != nil {
		t.Fatalf("stat %s: %v", dir, err)
	}
	fileInfo, err := os.Lstat(path)
	if err != nil {
		t.Fatalf("lstat %s: %v", path, err)
	}
	if !fileInfo.Mode().IsRegular() {
		t.Fatalf("%s mode = %v, want regular file", path, fileInfo.Mode())
	}
	if runtime.GOOS != "windows" {
		if dirInfo.Mode().Perm() != 0o700 {
			t.Errorf("%s perm = %04o, want 0700", dir, dirInfo.Mode().Perm())
		}
		if fileInfo.Mode().Perm() != 0o600 {
			t.Errorf("%s perm = %04o, want 0600", path, fileInfo.Mode().Perm())
		}
	}
	dirEntries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("readdir %s: %v", dir, err)
	}
	for _, e := range dirEntries {
		if strings.HasSuffix(e.Name(), ".tmp") {
			t.Errorf("leftover temp file %q in %s", e.Name(), dir)
		}
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	var parsed cacheFile[V]
	if err := json.Unmarshal(raw, &parsed); err != nil {
		t.Fatalf("unmarshal %s: %v\n%s", path, err, raw)
	}
	if parsed.Version != cacheEnvelopeVersion {
		t.Errorf("%s version = %d, want %d", path, parsed.Version, cacheEnvelopeVersion)
	}
	return parsed.Entries
}

func TestCache_PersistenceAndDeleteIf(t *testing.T) {
	fx := newCacheFixture(t, "")
	c1 := fx.newCache()

	// Empty-key and cold-miss operations must never materialize the cache dir.
	c1.Put("", testPersistentItem{Label: "empty"})
	c1.Delete("")
	if _, ok := c1.Get(""); ok {
		t.Fatal("Get(\"\") = ok, want miss")
	}
	if _, ok := c1.Get("alpha"); ok {
		t.Fatal("cold Get(alpha) = ok, want miss")
	}
	c1.Delete("alpha")
	fx.assertDirNotCreated()

	// Put persists atomically (0700 dir, 0600 file, no .tmp) and reloads in c2.
	c1.Put("alpha", testPersistentItem{Label: "one"})
	_ = readDiskCacheFile[testPersistentItem](t, fx.filePath)

	c2 := fx.newCache()
	if got, ok := c2.Get("alpha"); !ok || got.Label != "one" {
		t.Fatalf("c2.Get(alpha) = (%+v, %v), want Label=one", got, ok)
	}

	// DeleteIf removes only when match returns true, persisting the eviction.
	if c1.DeleteIf("alpha", func(v testPersistentItem) bool { return v.Label == "wrong" }) {
		t.Error("DeleteIf with non-matching predicate removed alpha")
	}
	if !c1.DeleteIf("alpha", func(v testPersistentItem) bool { return v.Label == "one" }) {
		t.Error("DeleteIf with matching predicate did not remove alpha")
	}
	c2.ResetMemory()
	if _, ok := c2.Get("alpha"); ok {
		t.Error("alpha remained on disk after DeleteIf")
	}
}

func TestCache_TTL(t *testing.T) {
	for _, tc := range []struct {
		name    string
		until   time.Duration // 0 calls Put; >0 calls PutUntil(now+until)
		advance time.Duration
		wantHit bool
		wantTTL time.Duration
	}{
		{name: "Put valid before MaxTTL", advance: 59 * time.Minute, wantHit: true, wantTTL: time.Hour},
		{name: "Put expires at MaxTTL", advance: time.Hour, wantHit: false, wantTTL: time.Hour},
		{name: "PutUntil honors shorter deadline", until: 15 * time.Minute, advance: 14 * time.Minute, wantHit: true, wantTTL: 15 * time.Minute},
		{name: "PutUntil expires at shorter deadline", until: 15 * time.Minute, advance: 15 * time.Minute, wantHit: false, wantTTL: 15 * time.Minute},
		{name: "PutUntil clamps longer deadline to MaxTTL", until: 5 * time.Hour, advance: 59 * time.Minute, wantHit: true, wantTTL: time.Hour},
	} {
		t.Run(tc.name, func(t *testing.T) {
			fx := newCacheFixture(t, "")
			base := fx.now
			c := fx.newCache()
			if tc.until == 0 {
				c.Put("k", testPersistentItem{Label: "v"})
			} else {
				c.PutUntil("k", testPersistentItem{Label: "v"}, base.Add(tc.until))
			}
			if gotExp := readDiskCacheFile[testPersistentItem](t, fx.filePath)["k"].ExpiresAt; !gotExp.Equal(base.Add(tc.wantTTL)) {
				t.Errorf("disk ExpiresAt = %v, want %v", gotExp, base.Add(tc.wantTTL))
			}
			fx.now = base.Add(tc.advance)
			c.ResetMemory()
			if _, ok := c.Get("k"); ok != tc.wantHit {
				t.Errorf("Get(k) after %v = %v, want %v", tc.advance, ok, tc.wantHit)
			}
		})
	}

	t.Run("expired entries pruned on write and non-future PutUntil evicts", func(t *testing.T) {
		fx := newCacheFixture(t, "")
		c := fx.newCache()
		c.PutUntil("short", testPersistentItem{Label: "ephemeral"}, fx.now.Add(10*time.Minute))
		fx.now = fx.now.Add(20 * time.Minute)

		c.Put("fresh", testPersistentItem{Label: "kept"})
		if _, still := readDiskCacheFile[testPersistentItem](t, fx.filePath)["short"]; still {
			t.Error("expired entry 'short' was not pruned from disk on write")
		}
		c.PutUntil("fresh", testPersistentItem{Label: "stale"}, fx.now)
		if _, ok := c.Get("fresh"); ok {
			t.Error("PutUntil(now) kept existing entry 'fresh'")
		}
	})
}

func TestCache_InvalidDiskFiles(t *testing.T) {
	for _, tc := range []struct {
		name string
		raw  string
	}{
		{name: "corrupt JSON", raw: `{not-json`},
		{name: "legacy unversioned schema", raw: `{"legacy":"schema"}`},
		{name: "unsupported future version", raw: `{"version":99,"entries":{"k":{"value":{"label":"v99"},"expires_at":"2026-10-29T06:30:00Z"}}}`},
		{name: "clock-skewed expiry beyond MaxTTL", raw: `{"version":1,"entries":{"k":{"value":{"label":"skew"},"expires_at":"2999-01-01T00:00:00Z"}}}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			fx := newCacheFixture(t, "")
			if err := os.MkdirAll(fx.dir, 0o700); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(fx.filePath, []byte(tc.raw), 0o600); err != nil {
				t.Fatal(err)
			}
			c := fx.newCache()
			if _, ok := c.Get("k"); ok {
				t.Fatal("Get(k) = ok on invalid disk payload, want benign miss")
			}
			c.Put("k", testPersistentItem{Label: "healed"})
			c.ResetMemory()
			if got, ok := c.Get("k"); !ok || got.Label != "healed" {
				t.Errorf("Get(k) after heal = (%+v, %v), want Label=healed", got, ok)
			}
		})
	}
}

func TestCache_ConfinedPathsRejectUnsafeTargets(t *testing.T) {
	const outsideBody = `{"version":1,"entries":{"bad":{"value":{"label":"leaked"},"expires_at":"2026-10-29T06:30:00Z"}}}`

	for _, tc := range []struct {
		name  string
		setup func(t *testing.T, root string) (cacheDir string, checkUntouched func(t *testing.T))
	}{
		{
			name: "symlinked cache directory",
			setup: func(t *testing.T, root string) (string, func(*testing.T)) {
				targetDir := filepath.Join(root, "target")
				if err := os.MkdirAll(targetDir, 0o700); err != nil {
					t.Fatal(err)
				}
				targetFile := filepath.Join(targetDir, "test_items.json")
				if err := os.WriteFile(targetFile, []byte(outsideBody), 0o600); err != nil {
					t.Fatal(err)
				}
				linkDir := filepath.Join(root, "link-dir")
				if err := os.Symlink(targetDir, linkDir); err != nil {
					t.Fatal(err)
				}
				return linkDir, func(t *testing.T) {
					if got, _ := os.ReadFile(targetFile); string(got) != outsideBody {
						t.Error("Put modified file through symlinked cache directory")
					}
				}
			},
		},
		{
			name: "symlinked cache file",
			setup: func(t *testing.T, root string) (string, func(*testing.T)) {
				cacheDir := filepath.Join(root, "cache")
				if err := os.MkdirAll(cacheDir, 0o700); err != nil {
					t.Fatal(err)
				}
				outside := filepath.Join(root, "outside.json")
				if err := os.WriteFile(outside, []byte(outsideBody), 0o600); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(outside, filepath.Join(cacheDir, "test_items.json")); err != nil {
					t.Fatal(err)
				}
				return cacheDir, func(t *testing.T) {
					if got, _ := os.ReadFile(outside); string(got) != outsideBody {
						t.Error("Put modified target of symlinked cache file")
					}
				}
			},
		},
		{
			name: "directory at cache file path",
			setup: func(t *testing.T, root string) (string, func(*testing.T)) {
				cacheDir := filepath.Join(root, "cache")
				subDir := filepath.Join(cacheDir, "test_items.json")
				if err := os.MkdirAll(subDir, 0o700); err != nil {
					t.Fatal(err)
				}
				return cacheDir, func(t *testing.T) {
					if info, err := os.Lstat(subDir); err != nil || !info.IsDir() {
						t.Errorf("subdirectory at cache file path altered: info=%v err=%v", info, err)
					}
				}
			},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cacheDir, checkUntouched := tc.setup(t, t.TempDir())
			fx := newCacheFixture(t, cacheDir)
			c := fx.newCache()

			if _, ok := c.Get("bad"); ok {
				t.Error("Get read through unsafe cache path")
			}
			c.Put("written", testPersistentItem{Label: "mem-only"})
			if got, ok := c.Get("written"); !ok || got.Label != "mem-only" {
				t.Errorf("in-process Get(written) = (%+v, %v), want mem-only", got, ok)
			}
			c.ResetMemory()
			if _, ok := c.Get("written"); ok {
				t.Error("Put persisted entry across ResetMemory on unsafe path")
			}
			checkUntouched(t)
		})
	}
}

func TestCache_DegradationAndDisableEnv(t *testing.T) {
	t.Run("read-only cache directory degrades cleanly in memory", func(t *testing.T) {
		if runtime.GOOS == "windows" || os.Getuid() == 0 {
			t.Skip("POSIX non-root directory permission check")
		}
		fx := newCacheFixture(t, "")
		fx.newCache().Put("alpha", testPersistentItem{Label: "seeded"})

		if err := os.Chmod(fx.dir, 0o500); err != nil {
			t.Fatalf("Chmod(0500): %v", err)
		}
		t.Cleanup(func() { _ = os.Chmod(fx.dir, 0o700) })

		c := fx.newCache()
		if got, ok := c.Get("alpha"); !ok || got.Label != "seeded" {
			t.Fatalf("cold Get(alpha) on 0500 dir = (%+v, %v), want seeded", got, ok)
		}
		c.Put("beta", testPersistentItem{Label: "mem-1"})
		c.Put("gamma", testPersistentItem{Label: "mem-2"})
		c.Delete("alpha")

		if _, ok := c.Get("alpha"); ok {
			t.Error("deleted key alpha was resurrected from read-only disk")
		}
		for k, want := range map[string]string{"beta": "mem-1", "gamma": "mem-2"} {
			if got, ok := c.Get(k); !ok || got.Label != want {
				t.Errorf("Get(%s) on read-only dir = (%+v, %v), want %q", k, got, ok, want)
			}
		}
	})

	t.Run("DisableEnv degrades persistent cache to memory and non-string keys work in memory", func(t *testing.T) {
		fx := newCacheFixture(t, "")
		t.Setenv("GHISH_TEST_ITEMS_CACHE", "0")

		pc := fx.newCache()
		pc.Put("k", testPersistentItem{Label: "mem-only"})
		if got, ok := pc.Get("k"); !ok || got.Label != "mem-only" {
			t.Errorf("persistent cache with DisableEnv=0 Get(k) = (%+v, %v), want mem-only", got, ok)
		}
		fx.assertDirNotCreated()

		mc := newDetachedCache[testMemKey, testMemItem]()
		key := testMemKey{Scope: "gerrit", Index: 1}
		mc.Put(key, testMemItem{Value: "tok"})
		if got, ok := mc.Get(key); !ok || got.Value != "tok" {
			t.Errorf("memory cache Get = (%+v, %v), want tok", got, ok)
		}
	})
}

func TestCache_ValidationPanics(t *testing.T) {
	for _, tc := range []struct {
		name string
		fn   func()
	}{
		{name: "non-string key on disk tier", fn: func() { _ = newDetachedCache[testMemKey, testPersistentItem]() }},
		{name: "empty policy Name", fn: func() { validateCachePolicy(CachePolicy{Tier: CacheInMemory}) }},
		{name: "path traversal Name", fn: func() { validateCachePolicy(CachePolicy{Tier: CacheInMemory, Name: "../escape"}) }},
		{name: "negative MaxTTL", fn: func() { validateCachePolicy(CachePolicy{Tier: CacheInMemory, Name: "neg", MaxTTL: -time.Second}) }},
		{name: "CacheInMemory with DisableEnv", fn: func() { validateCachePolicy(CachePolicy{Tier: CacheInMemory, Name: "mem", DisableEnv: "GHISH_X"}) }},
		{name: "CacheRuntime without MaxTTL", fn: func() { validateCachePolicy(CachePolicy{Tier: CacheRuntime, Name: "rt"}) }},
		{name: "unknown Tier", fn: func() { validateCachePolicy(CachePolicy{Name: "bad_tier"}) }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			defer func() {
				if recover() == nil {
					t.Fatalf("expected panic for %s", tc.name)
				}
			}()
			tc.fn()
		})
	}
}

// TestCachePolicies_SecurityInvariants audits every package-level Cache
// registered via NewCache so persistent caches never store credentials.
func TestCachePolicies_SecurityInvariants(t *testing.T) {
	cacheRegistryMu.Lock()
	registered := slices.Clone(cacheRegistry)
	cacheRegistryMu.Unlock()
	if len(registered) == 0 {
		t.Fatal("cacheRegistry is empty")
	}

	forbidden := []string{"token", "cookie", "secret", "password", "passwd", "ticket", "auth", "bearer"}
	checkIdent := func(where, ident string) {
		lower := strings.ToLower(ident)
		for _, bad := range forbidden {
			if strings.Contains(lower, bad) {
				t.Errorf("CachePersistent %s (%q) contains forbidden credential term %q", where, ident, bad)
			}
		}
	}
	visited := make(map[reflect.Type]bool)
	var inspectPersistentType func(rt reflect.Type, path string)
	inspectPersistentType = func(rt reflect.Type, path string) {
		for rt.Kind() == reflect.Pointer || rt.Kind() == reflect.Slice || rt.Kind() == reflect.Array {
			rt = rt.Elem()
		}
		if rt.Name() != "" && rt.PkgPath() != "time" {
			checkIdent("type "+path, rt.Name())
		}
		if rt.Kind() == reflect.Map {
			inspectPersistentType(rt.Key(), path+"[key]")
			inspectPersistentType(rt.Elem(), path+"[val]")
			return
		}
		if rt.Kind() != reflect.Struct || rt.PkgPath() == "time" || visited[rt] {
			return
		}
		visited[rt] = true
		for i := range rt.NumField() {
			f := rt.Field(i)
			rawTag := f.Tag.Get("json")
			if rawTag == "-" {
				continue
			}
			tagName, _, _ := strings.Cut(rawTag, ",")
			checkIdent("field "+path+"."+f.Name, f.Name)
			if tagName != "" {
				checkIdent("json tag on "+path+"."+f.Name, tagName)
			}
			inspectPersistentType(f.Type, path+"."+f.Name)
		}
	}

	seenNames := make(map[string]bool)
	seenDisableEnvs := make(map[string]string)
	for _, rc := range registered {
		p := rc.Policy
		if seenNames[p.Name] {
			t.Errorf("duplicate cache Name %q", p.Name)
		}
		seenNames[p.Name] = true
		if p.DisableEnv != "" {
			if prev, dup := seenDisableEnvs[p.DisableEnv]; dup {
				t.Errorf("duplicate DisableEnv %q on %q and %q", p.DisableEnv, prev, p.Name)
			}
			seenDisableEnvs[p.DisableEnv] = p.Name
		}
		switch p.Tier {
		case CachePersistent:
			if p.DisableEnv == "" {
				t.Errorf("CachePersistent %q must declare a per-cache DisableEnv", p.Name)
			}
			clear(visited)
			inspectPersistentType(rc.ValueType, p.Name)
		case CacheRuntime:
			if p.MaxTTL <= 0 || p.MaxTTL > 4*time.Hour {
				t.Errorf("CacheRuntime %q MaxTTL = %v, want within (0, 4h]", p.Name, p.MaxTTL)
			}
			if p.DisableEnv == "" {
				t.Errorf("CacheRuntime %q must declare a per-cache DisableEnv", p.Name)
			}
		case CacheInMemory:
		default:
			t.Errorf("cache %q has unexpected tier %v", p.Name, p.Tier)
		}
	}
}

func TestCache_GetOrLoadAndPutIfAbsent(t *testing.T) {
	fx := newCacheFixture(t, "")
	c := fx.newCache()

	if c.PutIfAbsent("", testPersistentItem{Label: "ignored"}) {
		t.Error("PutIfAbsent(\"\") = true, want false")
	}
	if !c.PutIfAbsent("k1", testPersistentItem{Label: "first"}) {
		t.Error("first PutIfAbsent(\"k1\") = false, want true")
	}
	if c.PutIfAbsent("k1", testPersistentItem{Label: "second"}) {
		t.Error("duplicate PutIfAbsent(\"k1\") = true, want false")
	}
	if got, ok := c.Get("k1"); !ok || got.Label != "first" {
		t.Errorf("Get(\"k1\") = (%+v, %v), want first", got, ok)
	}
	fx.now = fx.now.Add(2 * time.Hour)
	if !c.PutIfAbsent("k1", testPersistentItem{Label: "after-expiry"}) {
		t.Error("PutIfAbsent(\"k1\") after expiry = false, want true")
	}

	// GetOrLoad: errors and cancelled contexts do not populate the cache.
	loadErr := errors.New("load failed")
	if _, err := c.GetOrLoad(t.Context(), "k2", func(context.Context) (testPersistentItem, time.Time, error) {
		return testPersistentItem{Label: "bad"}, time.Time{}, loadErr
	}); !errors.Is(err, loadErr) {
		t.Errorf("GetOrLoad error = %v, want %v", err, loadErr)
	}
	if _, ok := c.Get("k2"); ok {
		t.Error("Get(\"k2\") = hit after failed GetOrLoad, want miss")
	}

	cancelCtx, cancel := context.WithCancel(t.Context())
	if _, err := c.GetOrLoad(cancelCtx, "k2", func(context.Context) (testPersistentItem, time.Time, error) {
		cancel()
		return testPersistentItem{Label: "cancelled"}, time.Time{}, nil
	}); !errors.Is(err, context.Canceled) {
		t.Errorf("GetOrLoad mid-flight cancel = %v, want context.Canceled", err)
	}
	if _, ok := c.Get("k2"); ok {
		t.Error("Get(\"k2\") = hit after cancelled GetOrLoad, want miss")
	}

	// Concurrent callers share a single load invocation even when DeleteIf
	// contends with an advancing clock during load.
	var clockNanos atomic.Int64
	clockNanos.Store(fx.now.UnixNano())
	WithTestCaches(t, TestCacheConfig{
		PersistentDir: fx.dir,
		Now:           func() time.Time { return time.Unix(0, clockNanos.Load()).UTC() },
	})
	c.Put("shared", testPersistentItem{Label: "stale"})

	var loads atomic.Int32
	release := make(chan struct{})
	started := make(chan struct{}, 1)
	var wg sync.WaitGroup
	for range 8 {
		wg.Go(func() {
			c.DeleteIf("shared", func(v testPersistentItem) bool { return v.Label == "stale" })
			got, err := c.GetOrLoad(t.Context(), "shared", func(context.Context) (testPersistentItem, time.Time, error) {
				loads.Add(1)
				select {
				case started <- struct{}{}:
				default:
				}
				<-release
				now := time.Unix(0, clockNanos.Add(int64(5*time.Millisecond))).UTC()
				return testPersistentItem{Label: "minted"}, now.Add(2 * time.Hour), nil
			})
			if err != nil || got.Label != "minted" {
				t.Errorf("GetOrLoad(\"shared\") = (%+v, %v), want minted", got, err)
			}
		})
	}
	<-started
	close(release)
	wg.Wait()
	if n := loads.Load(); n != 1 {
		t.Errorf("concurrent GetOrLoad runs = %d, want 1", n)
	}
}
