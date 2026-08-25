---
name: c-audit
description: Audit C code for security, safety, and memory management. Use when reviewing or hardening C modules.
user-invocable: true
---

# C Code Audit Skill

Perform comprehensive security, safety, and quality audits on KEEL C code.

**Target:** $ARGUMENTS (default: all `src/` files, recursively, including `src/protocols/`)

## Usage

```
/c-audit                    # Audit all source files
/c-audit src/router.c       # Audit a specific file
/c-audit --fix              # Audit and apply fixes
```

## Audit Categories

### 1. Memory Safety (Critical)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Buffer overflow | `strcpy`, `strcat`, `sprintf`, `gets`, unbounded loops | Critical |
| Unbounded string ops | `strlen`, `strcmp` on untrusted input | Critical |
| Unsafe integer parsing | `atoi`, `atol`, `atof` (no error detection, no bounds) | High |
| Integer overflow | `malloc(a * b)` without overflow check | Critical |
| Use-after-free | Pointer used after `free()` | Critical |
| Double-free | `free()` called twice on same pointer | Critical |
| Null dereference | Pointer used without NULL check | High |
| Uninitialized memory | Variables used before assignment | High |
| Missing null terminator | String buffer not explicitly terminated | High |
| Memory leak | `kl_malloc` without corresponding `kl_free` | Medium |
| Stack buffer overflow | Large stack arrays, VLAs | Medium |

**Safe Replacements:**
```c
// Copying
strcpy(dst, src)           -> strncpy(dst, src, sizeof(dst)-1); dst[sizeof(dst)-1] = '\0';
strcat(dst, src)           -> strncat(dst, src, sizeof(dst)-strlen(dst)-1);

// Formatting
sprintf(buf, fmt, ...)     -> snprintf(buf, sizeof(buf), fmt, ...);
gets(buf)                  -> fgets(buf, sizeof(buf), stdin);

// Memory allocation (overflow-safe)
malloc(count * size)       -> calloc(count, size);

// Integer parsing (atoi/atol have no error detection!)
atoi(str)                  -> strtol(str, &end, 10) with validation
atof(str)                  -> strtof(str, &end) with validation
```

**Allocator Discipline:**
```c
// KEEL never calls malloc/free directly: always use the allocator
// BAD:
void *p = malloc(size);
free(p);

// GOOD:
void *p = kl_malloc(alloc, size);
kl_free(alloc, p, size);

// Verify: every kl_malloc has a matching kl_free with correct size
```

### 2. Input Validation

| Issue | What to Check |
|-------|---------------|
| Array bounds | All array indices validated before access |
| Pointer validity | NULL checks before dereference |
| Size parameters | Non-negative, within reasonable bounds |
| String length | Length checked before copy/concat |
| Numeric ranges | Values within expected domain |
| HTTP header count | `num_headers` checked against `KL_MAX_HEADERS` |
| Read buffer | `read_len` checked against `KL_READ_BUF_SIZE` |

### 3. Resource Management

| Issue | What to Check |
|-------|---------------|
| File descriptors | `open`/`accept` paired with `close` |
| Memory | `kl_malloc` paired with `kl_free` (with correct size) |
| Parsers | `kl_parser_llhttp()` paired with `parser->destroy()` |
| Event loop | `kl_event_init` paired with `kl_event_close` |
| Connections | `kl_conn_acquire` paired with `kl_conn_release` |
| Error paths | Resources freed on all exit paths |
| I/O return values | `write`/`read`/`send` return values checked |

**KEEL Cleanup Pattern:**
```c
// Every init/create must have matching free
kl_router_init()       -> kl_router_free()
kl_conn_pool_init()    -> kl_conn_pool_free()
kl_response_init()     -> kl_response_free()
kl_event_init()        -> kl_event_close()
kl_server_init()       -> kl_server_free()
kl_parser_llhttp()     -> parser->destroy()
```

### 4. Integer Overflow

Overflow in size computations can cause undersized allocations and buffer overflows.

```c
// BAD: overflow on 32-bit
int total = count * sizeof(KlConn);
void *buf = kl_malloc(alloc, total);

// GOOD: use size_t
size_t total = (size_t)count * sizeof(KlConn);
void *buf = kl_malloc(alloc, total);

// GOOD: check before multiply
if (count > 0 && (size_t)count > SIZE_MAX / sizeof(KlConn)) {
    return -1;  // overflow
}
```

**Key areas in KEEL:**
- Connection pool allocation (`capacity * sizeof(KlConn)`)
- Route array growth (`capacity * sizeof(KlRoute)`)
- Header buffer growth (realloc doubling)
- Response header block assembly

### 5. Network Security

| Issue | Severity | What to Check |
|-------|----------|---------------|
| SIGPIPE handling | High | `signal(SIGPIPE, SIG_IGN)` before any `write()` |
| Partial writes | High | `write()` may not send all bytes; handle short writes |
| EPIPE/ECONNRESET | Medium | Connection closed by peer; don't crash |
| Request smuggling | High | Parser correctly handles Content-Length vs Transfer-Encoding |
| Header injection | High | Response headers don't contain \r\n from user input |
| Slowloris | Medium | Read timeout enforced on idle connections |
| fd exhaustion | Medium | Max connections limit enforced |

### 6. Defensive Macros

Check for and suggest these patterns:
```c
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
```

### 7. Test Coverage

Check test files (`tests/test_*.c`) for:
- [ ] Basic functionality tests
- [ ] Edge cases (empty input, max values, NULL)
- [ ] Error path tests (what happens when things fail)
- [ ] Bounds checking tests
- [ ] All public API functions have at least one test
- [ ] Parser tested with malformed HTTP
- [ ] Router tested with edge-case paths

### 8. Dead Code Detection

| Pattern | Issue | Fix |
|---------|-------|-----|
| `if (0) { ... }` | Dead branch | Remove |
| `return; code_after;` | Unreachable code | Remove |
| `#if 0 ... #endif` | Disabled code | Remove or document |
| Unused `#define` | Dead macro | Remove |
| Unused static function | Dead function | Remove |

Compile with `-Wunused` flags to detect automatically.

### 9. Build Hardening

**Development build (`make debug`):**
```makefile
-fsanitize=address,undefined -g -fno-omit-frame-pointer
```

**Production build (`make`):**
```makefile
-Wall -Wextra -Wpedantic -Wshadow -Wformat=2
-fstack-protector-strong
-O2
```

**Audit Checks:**
- [ ] `-fstack-protector-strong` in production CFLAGS
- [ ] Debug build with ASan + UBSan available (`make debug`)
- [ ] All tests pass under sanitizers
- [ ] No compiler warnings with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`

## Audit Procedure

When `/c-audit` is invoked:

1. **Locate Files**
   ```
   src/*.c src/protocols/**/*.c include/keel/*.h   # Substrate + protocol source and headers
   tests/test_*.c tests/protocols/*/test_*.c       # Test files
   Makefile                    # Build configuration
   ```

2. **Scan for Critical Issues**
   - Search for unsafe functions: `strcpy`, `sprintf`, `gets`, `strcat`
   - Search for unsafe integer parsing: `atoi`, `atol`, `atof`
   - Search for direct malloc/free (should use `kl_malloc`/`kl_free`)
   - Search for unchecked allocations: `kl_malloc` without NULL check
   - Search for integer overflow in size calculations
   - Search for missing bounds checks on array access
   - Search for unchecked `write()`/`read()` return values

3. **Review Public API**
   - Check all public functions in headers
   - Verify NULL checks on pointer parameters
   - Verify bounds checks on size parameters

4. **Check Resource Management**
   - Every `_init()`/`_create()` has matching `_free()`/`_close()`
   - Error paths free allocated resources
   - No memory leaks on early returns
   - File descriptors closed on error paths
   - Connections properly released

5. **Check Network Safety**
   - SIGPIPE ignored before writes
   - Connection errors handled gracefully
   - Read buffer overflow prevented
   - Response header injection prevented

6. **Detect Dead Code**
   - Compile with `-Wunused` flags
   - Find unused static functions
   - Find unused variables and parameters
   - Flag commented-out or `#if 0` code blocks

7. **Check Build Hardening**
   - Sanitizers available in debug build
   - Stack protection in production build
   - Warning flags comprehensive

8. **Generate Report**
   Format as markdown table with findings, severity, file:line, and suggested fix.

## Report Format

```markdown
## C Audit Report: KEEL

**Date:** YYYY-MM-DD
**Files Scanned:** N
**Issues Found:** N (Critical: N, High: N, Medium: N, Low: N)

### Critical Issues

| # | File:Line | Issue | Current Code | Suggested Fix |
|---|-----------|-------|--------------|---------------|
| C1 | src/foo.c:42 | Buffer overflow | `strcpy(buf, src)` | `snprintf(buf, sizeof(buf), "%s", src)` |

### High Issues
...

### Medium Issues
...

### Low Issues
...

### Recommendations
1. ...
```

## Fix Mode (--fix)

When `--fix` is specified:

1. Generate the audit report first
2. For each fixable issue, apply the transformation
3. Rebuild (`make`)
4. Re-run tests (`make test`)
5. Report any test failures or new warnings

**Auto-fixable Issues:**
- `strcpy` -> `snprintf` with buffer size
- `sprintf` -> `snprintf` with buffer size
- `atoi` -> `strtol` with validation
- Direct `malloc`/`free` -> `kl_malloc`/`kl_free`
- Missing NULL checks (add early return)
- Missing `kl_malloc` return check (add NULL check)
- Integer overflow in size calc -> `calloc` or overflow check
- Missing `size_t` casts in size calculations
- Unused local variables (remove)
- Unused static functions (remove)

**NOT Auto-fixable (require manual review):**
- Logic errors
- Resource leaks in complex control flow
- fd leak in error paths
- Architectural changes
