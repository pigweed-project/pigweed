# `pw_ghish/internal/astcheck`

- **Fail loud:** Silent zero returns (`""`, `0`, `nil`, `false`) or `continue` on invalid inputs disguise bugs as 0 findings (false passes). Panic—or call `tb.Fatalf` when `testing.TB` is in scope—on nil `*File`/`AST`, invalid or cross-file `token.Pos`, empty patterns/prefixes, or malformed AST literals.
- **Small, reusable library (mechanism only):** Keep domain policy, contextual heuristics, and one-off traversals in `pw_ghish/*_ast_test.go`. Compose existing primitives (`FindCalls`, `FindCompositeLits`, `Directives`); hoist or extend a domain-agnostic helper when ≥2 checks share it, and migrate existing callers to it.
- **Syntactic only:** Use `go/parser` (`parser.SkipObjectResolution`) + `go/ast`, never `go/types`, so checks stay millisecond-fast.
- **Prefer `//ghish:wraps`:** Enforce wrapper choke-points by annotating the exported wrapper (`//ghish:wraps <pkg.Symbol> -- <guidance>`) for `AssertWraps`, not bespoke AST walkers.
