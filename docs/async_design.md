# Userland Async for Hull

## Context

Hull handlers are fully synchronous today. When a handler calls `http.get("https://api.stripe.com/charges")`, it blocks for 100-500ms. During that time, the entire server is stalled — no other requests processed. This is the single biggest architectural limitation for any workload that involves outbound HTTP calls.

The primitives for fixing this already exist in the stack:
- Keel's event loop can register arbitrary FDs (`kl_event_add`)
- Lua 5.4 has coroutines with C-boundary yield support (`lua_yieldk`)
- QuickJS has promises and a microtask queue (`JS_ExecutePendingJob`)
- The body reader pattern already does incremental callback-based I/O

None of these are wired together today. This plan connects them.

---

## User-Facing API

### Lua — Transparent (no syntax change)

```lua
-- http.get() yields internally via coroutine — looks synchronous to the developer
app.get("/charge", function(req, res)
    local resp = http.post("https://api.stripe.com/charges", body, headers)
    res:json({ status = resp.status })
end)

-- Fan-out: multiple outbound calls in parallel
app.get("/dashboard", function(req, res)
    local results = hull.gather({
        function() return http.get("https://api.stripe.com/balance") end,
        function() return http.get("https://api.github.com/user") end,
    })
    res:json({ balance = results[1], user = results[2] })
end)

-- Sleep without blocking the event loop
app.get("/delayed", function(req, res)
    hull.sleep(1000)  -- yields for 1s
    res:json({ ok = true })
end)
```

### JavaScript — Natural async/await

```javascript
app.get("/charge", async (req, res) => {
    const resp = await http.post("https://api.stripe.com/charges", body, headers);
    res.json({ status: resp.status });
});

// Fan-out via standard Promise.all
app.get("/dashboard", async (req, res) => {
    const [balance, user] = await Promise.all([
        http.get("https://api.stripe.com/balance"),
        http.get("https://api.github.com/user"),
    ]);
    res.json({ balance, user });
});
```

### Backward Compatibility

Fully backward compatible. If a handler never yields/awaits, it runs exactly as today:
- Lua: `lua_resume()` returns `LUA_OK` (not `LUA_YIELD`) → same as `lua_pcall()`
- JS: `JS_Call()` returns a non-Promise value → proceeds synchronously

---

## Architecture — Three Layers

```
┌──────────────────────────────────────────────────┐
│  HlLuaAsyncCont    HlJsAsyncCont    (future...)  │
│  Implements HlAsyncCont vtable per runtime        │
│  Lua: coroutine resume  JS: promise resolve       │
├──────────────────────────────────────────────────┤
│  HlAsyncCtx + HlAsyncCont vtable (Hull glue)     │
│  Runtime-agnostic: calls cont->resume/cancel      │
│  One per suspended handler                        │
├──────────────────────────────────────────────────┤
│  Keel Async                                       │
│  Primitives: KlWatcher (FD callbacks),            │
│              KlAsyncOp (suspend/resume/deadline)   │
│  Drivers: KlHttpClient, sleep, future thread pool  │
│  Pure I/O — no Lua, no JS, no application logic   │
└──────────────────────────────────────────────────┘
```

Each layer has a single responsibility and zero knowledge of the layers above it.

Keel owns everything below the application boundary: primitives AND I/O drivers. The HTTP client state machine uses `KlWatcher` (Keel), `KlTls` (Keel), non-blocking sockets (OS) — all Keel-native. Any Keel user (not just Hull) gets async HTTP, thread pool, and sleep for free.

Hull owns the glue layer (`HlAsyncCtx`) and the per-runtime continuations (`HlAsyncCont` vtable). The ctx dispatches through the vtable — it never imports Lua or JS headers. Same pattern as `KlParser` (pluggable HTTP parsers), `KlTls` (pluggable TLS backends), and `HlRuntimeVtable` (pluggable language runtimes). Adding a new runtime requires implementing 3 functions (`resume`, `cancel`, `destroy`) — zero changes to `HlAsyncCtx` or any Keel code.

Hull's capability layer (`hl_cap_http_request`) calls the Keel driver after enforcing the host allowlist.

---

## Keel Async — Primitives + Drivers

### Primitive 1: KlWatcher — generic FD callback

Register any FD with the event loop. When the FD fires, call a callback. Keel doesn't care what the FD is (socket, pipe, eventfd, timerfd, io_uring ring FD).

```c
/* include/keel/async.h */

typedef void (*KlWatcherFn)(int fd, KlEventMask ready, void *user_data);

typedef struct KlWatcher {
    int fd;
    KlWatcherFn on_ready;
    void *user_data;
} KlWatcher;

int  kl_watcher_add(KlServer *s, int fd, KlEventMask mask,
                    KlWatcherFn on_ready, void *user_data);
int  kl_watcher_mod(KlServer *s, int fd, KlEventMask mask);
void kl_watcher_del(KlServer *s, int fd);
```

Implementation: tagged pointer in event loop `udata` (bit 0 set = watcher, clear = connection).

### Primitive 2: KlAsyncOp — connection suspension

Park a connection. Resume it later (by anyone — a watcher callback, a thread, a timer sweep). Enforce a deadline.

```c
typedef void (*KlAsyncFn)(struct KlAsyncOp *op, void *user_data);

typedef struct KlAsyncOp {
    KlConn *conn;              /* suspended connection */
    uint64_t deadline_ms;      /* absolute deadline (0 = no deadline) */
    KlAsyncFn on_resume;       /* called by kl_async_complete */
    KlAsyncFn on_deadline;     /* called when deadline_ms reached */
    KlAsyncFn on_cancel;       /* called if connection dies while suspended */
    void *user_data;           /* opaque — HlAsyncCtx* */
    struct KlAsyncOp *next;    /* active ops list (server-owned) */
} KlAsyncOp;

int  kl_async_suspend(KlServer *s, KlConn *conn, KlAsyncOp *op);
void kl_async_complete(KlServer *s, KlAsyncOp *op);
```

Three separate callbacks because deadline semantics differ per operation:
- Sleep: `on_deadline` → success (deadline = "timer fired")
- HTTP: `on_deadline` → error (deadline = "timeout")
- Gather: `on_deadline` → cancel remaining sub-ops, return partial results

`kl_async_suspend`: sets `conn->state = KL_CONN_SUSPENDED`, removes client FD from event loop, stores op, adds to active list.

`kl_async_complete`: calls `op->on_resume`, re-registers client FD, removes from list.

### New KlConn fields

```c
KlAsyncOp *async_op;       /* non-NULL when SUSPENDED */
uint64_t suspend_start_ms;
```

### Timeout sweep

- Walk active ops list each tick
- `op->deadline_ms > 0 && now >= op->deadline_ms` → call `op->on_deadline(op, op->user_data)`
- Compute minimum deadline; use as `kl_event_wait()` timeout for sub-second granularity

### Accessing KlConn from handlers

Add `void *_server_ctx` to `KlRequest`. Set to `KlConn*` by connection layer before calling handler.

---

## Hull Glue Layer — HlAsyncCont vtable + HlAsyncCtx

The bridge between Keel drivers and runtime continuations. Uses the same vtable pattern as `KlParser`, `KlTls`, `KlBodyReader`, and `HlRuntimeVtable` — `HlAsyncCtx` never knows which runtime it's talking to.

### HlAsyncCont — runtime continuation vtable

```c
/* include/hull/async.h */

typedef struct HlAsyncCont {
    void (*resume)(struct HlAsyncCont *self, void *driver);  /* push result, resume handler */
    void (*cancel)(struct HlAsyncCont *self);                /* free refs without invoking */
    void (*destroy)(struct HlAsyncCont *self);               /* free the cont struct itself */
} HlAsyncCont;
```

Each runtime provides its own concrete implementation:

```c
/* hull/src/hull/runtime/lua/async.c */
typedef struct HlLuaAsyncCont {
    HlAsyncCont base;           /* vtable — must be first member */
    lua_State *co;              /* coroutine to resume */
    int thread_ref;             /* registry ref (prevents GC) */
    HlLua *lua;                 /* runtime instance (for lua_resume) */
} HlLuaAsyncCont;

/* hull/src/hull/runtime/js/async.c */
typedef struct HlJsAsyncCont {
    HlAsyncCont base;           /* vtable — must be first member */
    JSValue resolve;            /* Promise resolve function */
    JSValue reject;             /* Promise reject function */
    JSContext *ctx;             /* JS context (for JS_Call, JS_FreeValue) */
} HlJsAsyncCont;
```

Adding a new runtime (WASM, Python, etc.) means implementing these 3 functions. Zero changes to `HlAsyncCtx`, zero changes to any Keel driver.

### HlAsyncCtx — runtime-agnostic glue

```c
/* include/hull/async.h */

typedef struct HlAsyncCtx {
    KlAsyncOp op;               /* embedded — container_of to get ctx */
    KlServer *server;

    /* Keel driver — opaque to the ctx */
    void *driver;               /* KlHttpClient*, NULL for sleep, etc. */
    void (*free_driver)(void *driver);

    /* Runtime continuation — vtable dispatch, no runtime knowledge */
    HlAsyncCont *cont;          /* Lua, JS, or any future runtime */

    KlAllocator *alloc;
} HlAsyncCtx;
```

**`on_resume` callback** (set on `ctx->op`):
1. `container_of(op, HlAsyncCtx, op)` to get the ctx
2. `ctx->cont->resume(ctx->cont, ctx->driver)` — vtable dispatch
3. `ctx->cont->destroy(ctx->cont)` — free continuation
4. `ctx->free_driver(ctx->driver)` — free driver
5. Free ctx

**`on_cancel` callback**: `ctx->cont->cancel(ctx->cont)` then `ctx->cont->destroy(ctx->cont)` — frees runtime refs without invoking the handler.

**`on_deadline` callback**: Same as cancel, but may also write a timeout response (504) before cleanup.

Hull's capability layer remains the enforcement boundary. It receives a fully-constructed `HlAsyncCtx` (with `cont` already set by the caller — Lua or JS module code) and only wires up the Keel driver:

```c
/* hull/src/hull/cap/http.c */
int hl_cap_http_request_async(KlServer *s, HlAsyncCtx *ctx,
                               const char *method, const char *url, ...)
{
    /* 1. Parse URL, check host allowlist (same as sync version) */
    /* 2. Audit log the request */
    /* 3. Call kl_http_client_start(s, &ctx->op, ...) — Keel does the I/O */
    /* 4. ctx->driver = client; ctx->free_driver = kl_http_client_free; */
}
```

### Driver 1: KlHttpClient — async HTTP/1.1

Lives in Keel. Uses `KlWatcher` for the outbound socket and `KlTls` for HTTPS. No Hull, no Lua, no JS — pure I/O.

```c
/* include/keel/http_client.h */

typedef struct KlHttpClient {
    int state;               /* CONNECTING, TLS, SENDING, RECEIVING, DONE, ERROR */
    int fd;                  /* outbound socket */
    char *send_buf;          /* serialized HTTP request */
    size_t send_len, send_off;
    char *recv_buf;          /* raw response bytes */
    size_t recv_len, recv_cap;
    KlTls *tls;              /* NULL for plain HTTP */
    KlHttpClientResponse response; /* parsed status + headers + body */
    KlAllocator *alloc;

    KlAsyncOp *op;           /* back-pointer — calls kl_async_complete when done */
    KlServer *server;        /* for watcher management */
} KlHttpClient;

/* Start an async HTTP request. Creates socket, starts connect, registers watcher. */
KlHttpClient *kl_http_client_start(KlServer *s, KlAsyncOp *op,
                                    const char *method, const char *url,
                                    const KlHttpClientHeader *headers, int num_headers,
                                    const char *body, size_t body_len,
                                    KlAllocator *alloc);

void kl_http_client_free(KlHttpClient *c);
```

The watcher's `on_ready` drives the state machine:
```
DNS resolve (synchronous — getaddrinfo, cached/fast)
    → CONNECTING (non-blocking connect, WRITE interest)
    → TLS_HANDSHAKE (if HTTPS — WANT_READ/WANT_WRITE loop via KlTls vtable)
    → SENDING (write request bytes, WRITE interest)
    → RECEIVING (read response, parse, READ interest)
    → DONE → kl_async_complete(s, op) / ERROR → kl_async_abort(s, op)
```

Fully testable standalone — socket + event loop + TLS, no application logic.

### Driver 2: Sleep

Not a separate driver struct. Just a `KlAsyncOp` with `deadline_ms` set and `on_deadline` pointing to `kl_async_complete`. The timeout sweep handles it.

### Future Driver 3: KlThreadPool

```c
/* include/keel/thread_pool.h — future */

typedef struct KlThreadPool {
    int completion_fd;       /* read end of pipe (or eventfd on Linux) */
    pthread_t *workers;
    int num_workers;
    KlServer *server;
    /* work queue (mutex-protected), result queue */
} KlThreadPool;

/* Submit blocking work. on_done called on event loop thread when complete. */
int kl_thread_pool_submit(KlThreadPool *pool, KlAsyncOp *op,
                          void (*work)(void *arg, void *result),
                          void *arg, size_t result_size);
```

Worker thread: executes `work(arg, result)`, writes completion token to pipe.
Watcher on completion_fd: reads token, finds op, calls `kl_async_complete`.

This is how SQLite queries, WASM execution, file I/O, and DNS resolution become async — all through the same Keel primitive. Hull just submits work items.

### Future Driver 4: KlH2Pool — HTTP/2 connection pool

One watcher per upstream TCP connection. Multiple ops (one per stream). Pool-level frame demuxing. Same pattern for HTTP/3 (QUIC over UDP).

All drivers live in `keel/src/` and `keel/include/keel/`. Tested independently of Hull.

---

## Hull Runtime Changes

### Lua: `lua_pcall` → `lua_resume`

```c
int hl_lua_dispatch(HlLua *lua, int handler_id, KlRequest *req, KlResponse *res)
{
    lua_State *co = lua_newthread(lua->L);
    /* move handler + req/res to co */
    int nres = 0;
    int status = lua_resume(co, lua->L, 2, &nres);

    if (status == LUA_OK)     return 0;  /* sync — proceed as today */
    if (status == LUA_YIELD)  return 1;  /* suspended — don't write response */
    /* else: error */                     return -1;
}
```

### Lua: yielding C functions

```c
static int lua_http_get(lua_State *L) {
    /* validate args, parse URL, check host allowlist */

    /* Create runtime-agnostic async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(server, conn, alloc);

    /* Create Lua-specific continuation (implements HlAsyncCont vtable) */
    HlLuaAsyncCont *lc = hl_lua_async_cont_create(lua, L, alloc);
    ctx->cont = &lc->base;

    /* Keel driver does the I/O — Hull just enforces capabilities */
    KlHttpClient *client = kl_http_client_start(server, &ctx->op, ...);
    ctx->driver = client;
    ctx->free_driver = (void(*)(void*))kl_http_client_free;

    return lua_yieldk(L, 0, (lua_KContext)ctx, lua_http_get_k);
}

static int lua_http_get_k(lua_State *L, int status, lua_KContext kctx) {
    HlAsyncCtx *ctx = (HlAsyncCtx *)kctx;
    KlHttpClient *client = ctx->driver;
    /* push response table from client->response */
    hl_async_ctx_free(ctx);  /* frees cont + driver */
    return 1;
}
```

### JS: promise detection + resolution

```c
/* Dispatch */
JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);
if (JS_PromiseState(js->ctx, ret) == JS_PROMISE_PENDING) {
    /* park connection, return 1 */
}

/* http.get returns a Promise */
static JSValue js_http_get(JSContext *ctx, ...) {
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    /* Create runtime-agnostic async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(server, conn, alloc);

    /* Create JS-specific continuation (implements HlAsyncCont vtable) */
    HlJsAsyncCont *jc = hl_js_async_cont_create(ctx, resolving_funcs, alloc);
    actx->cont = &jc->base;

    KlHttpClient *client = kl_http_client_start(server, &actx->op, ...);
    actx->driver = client;
    return promise;
}

/* On completion: cont->resume() resolves promise, drain microtasks, check if handler settled */
```

---

## Testing Strategy

### Keel Unit Tests (`tests/test_async.c`)

Test the two primitives in isolation using `socketpair()` and manual event loop stepping.

```c
/* Watcher tests */
UTEST(async, watcher_add_fires_on_write) {
    /* socketpair → register read end as watcher → write to write end →
       run one kl_event_wait tick → verify on_ready callback fired */
}

UTEST(async, watcher_mod_changes_interest) {
    /* register for READ → mod to WRITE → verify only WRITE events fire */
}

UTEST(async, watcher_del_stops_events) {
    /* register → del → write to FD → verify callback does NOT fire */
}

UTEST(async, watcher_multiple_fds) {
    /* register 3 watchers on 3 socketpairs → write to all →
       verify all 3 callbacks fire in one wait */
}

UTEST(async, watcher_coexists_with_connections) {
    /* register a watcher + accept a real HTTP connection →
       verify both watcher and connection events dispatch correctly */
}

/* Suspend/resume tests */
UTEST(async, suspend_sets_state) {
    /* create mock conn → kl_async_suspend → verify state == SUSPENDED,
       verify conn->async_op set, verify client FD removed from event loop */
}

UTEST(async, complete_calls_on_resume) {
    /* suspend → kl_async_complete → verify on_resume called,
       verify state transitions back, verify client FD re-registered */
}

UTEST(async, deadline_fires_on_timeout) {
    /* suspend with deadline_ms = now + 10 → advance time / run sweep →
       verify on_deadline called */
}

UTEST(async, cancel_on_connection_close) {
    /* suspend → close client FD / simulate disconnect →
       verify on_cancel called, verify resources freed */
}

UTEST(async, suspend_exempt_from_idle_timeout) {
    /* suspend → wait longer than idle timeout → verify NOT reaped */
}

UTEST(async, dynamic_wait_timeout) {
    /* suspend with deadline_ms = now + 50 → verify kl_event_wait called
       with timeout <= 50, not the default 1000 */
}

/* Integration: watcher completes an op */
UTEST(async, watcher_completes_suspended_conn) {
    /* Accept connection → suspend it → register watcher on a pipe →
       write to pipe → watcher on_ready calls kl_async_complete →
       verify connection resumed, on_resume called */
}
```

**Test infrastructure**: Create a test helper that:
1. Initializes a `KlServer` with a listener on a random port
2. Provides `kl_test_step(server)` — runs one iteration of the event loop
3. Provides `kl_test_connect(server)` — connects a client, returns the `KlConn*`
4. Provides `kl_test_advance_time(ms)` — for testing deadlines without real waits

### Keel Integration Tests (E2E)

Test watchers and suspension in a real event loop with actual connections.

```bash
# tests/e2e_async.sh

# Test 1: server with a slow handler (uses sleep) stays responsive
# Start server with a /slow route (hull.sleep(2000)) and a /fast route
# Hit /fast, verify it responds immediately while /slow is sleeping

# Test 2: multiple concurrent suspended connections
# Send 10 requests to /slow simultaneously, verify all resume

# Test 3: deadline timeout
# Suspend with 100ms deadline, wait 200ms, verify 504 returned

# Test 4: connection close while suspended
# Send request to /slow, close client before sleep finishes,
# verify server handles cancel gracefully (no crash, no leak)
```

### Hull Unit Tests — Lua Runtime (`tests/hull/runtime/test_lua_async.c`)

Test the Lua dispatch changes: coroutine creation, yield handling, resume, gas metering.

```c
UTEST(lua_async, sync_handler_unchanged) {
    /* Register a non-yielding handler → dispatch → verify LUA_OK,
       response written, same behavior as lua_pcall path */
}

UTEST(lua_async, yield_returns_suspended) {
    /* Register handler that calls hull.sleep(100) → dispatch →
       verify lua_resume returns LUA_YIELD, dispatch returns 1 */
}

UTEST(lua_async, resume_continues_handler) {
    /* Yield → manually call hl_lua_async_resume → verify handler
       continues, response written correctly */
}

UTEST(lua_async, error_in_handler_after_resume) {
    /* Yield → resume → handler raises error → verify 500 response,
       verify coroutine ref freed */
}

UTEST(lua_async, gas_metering_across_yield) {
    /* Set max_instructions = 1000 → handler does 400 instructions,
       yields, does 400 more → verify it completes.
       Then: 600 + yield + 600 → verify it hits the limit */
}

UTEST(lua_async, cancel_frees_coroutine) {
    /* Yield → simulate connection cancel → verify coroutine ref
       removed from registry, no memory leak (ASan) */
}

UTEST(lua_async, multiple_yields_same_handler) {
    /* Handler calls hull.sleep then http.get → yields twice →
       verify both resume correctly, response written once */
}
```

### Hull Unit Tests — JS Runtime (`tests/hull/runtime/test_js_async.c`)

```c
UTEST(js_async, sync_handler_unchanged) {
    /* Non-async handler → JS_Call returns non-Promise → same as today */
}

UTEST(js_async, async_handler_detects_promise) {
    /* async handler → JS_Call returns Promise → verify PENDING detected,
       dispatch returns 1 */
}

UTEST(js_async, resolve_resumes_handler) {
    /* Pending promise → call resolve with value → drain microtasks →
       verify handler completes, response written */
}

UTEST(js_async, reject_sends_500) {
    /* Pending promise → call reject → verify 500 response */
}

UTEST(js_async, cancel_frees_handles) {
    /* Pending → cancel → verify resolve/reject JS_FreeValue'd, no leak */
}

UTEST(js_async, promise_all_multiple_ops) {
    /* Handler does Promise.all([sleep, sleep]) → verify both
       resolve before handler completes */
}
```

### Keel Unit Tests — HTTP Client (`tests/test_http_client.c`)

Test the HTTP client state machine in isolation — no Hull, no Lua, no JS.

```c
UTEST(http_client, connect_nonblocking) {
    /* Start client to localhost echo server → verify state = CONNECTING,
       verify FD is non-blocking */
}

UTEST(http_client, send_request_incremental) {
    /* Start client → advance to SENDING → feed small write buffer →
       verify send_off advances, state stays SENDING until complete */
}

UTEST(http_client, receive_response_parse) {
    /* Advance to RECEIVING → feed raw HTTP response bytes →
       verify response.status, response.body parsed correctly */
}

UTEST(http_client, tls_handshake_want_read_write) {
    /* Mock TLS vtable returning WANT_READ/WANT_WRITE →
       verify client requests correct event mask transitions */
}

UTEST(http_client, connect_refused) {
    /* Connect to closed port → verify state = ERROR */
}

UTEST(http_client, partial_response) {
    /* Feed response in 1-byte chunks → verify parser handles
       incremental data correctly */
}

UTEST(http_client, completes_async_op) {
    /* Full lifecycle: start → connect → send → receive → done →
       verify kl_async_complete called on the op */
}
```

### Hull Integration Tests — E2E

```bash
# tests/e2e_async.sh

# 1. hull.sleep() — Lua
echo 'app.manifest({})
app.get("/slow", function(req, res) hull.sleep(500); res:json({ok=true}) end)
app.get("/fast", function(req, res) res:json({ok=true}) end)' > app.lua
hull dev app.lua &
# /fast should respond in <10ms while /slow is sleeping
curl -s localhost:3000/slow &
sleep 0.05
time curl -s localhost:3000/fast  # should be instant, not blocked by /slow

# 2. hull.sleep() — JS (same test, different runtime)

# 3. Async HTTP — handler calls external API
# Start a local echo server on port 9999
# Handler does http.get("http://localhost:9999/echo")
# Verify response, verify event loop responsive during the call

# 4. hull.gather() — parallel fan-out
# Handler does hull.gather with 3 http.get calls, each taking 200ms
# Verify total wall time ~200ms (not 600ms)

# 5. Timeout — async operation exceeds deadline
# Handler does http.get to a server that never responds
# Verify 504 returned after deadline

# 6. Stress — many concurrent suspended handlers
# Send 50 requests to /slow simultaneously
# Verify all 50 resume correctly

# 7. Connection close during suspension
# Send request to /slow, immediately close socket
# Verify server handles it (no crash, no leak under ASan)
```

### Fuzz Testing (`fuzz/fuzz_async_http.c`)

Fuzz the HTTP driver's response parser with random bytes. Same pattern as existing `fuzz_parser`:

```c
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlHttpClient client = {0};
    client.state = KL_HTTP_CLIENT_RECEIVING;
    /* Feed fuzz data as if received from network */
    kl_http_client_feed(&client, (const char *)data, size);
    kl_http_client_free(&client);
    return 0;
}
```

---

## Implementation Phases

### Phase 1: Keel primitives + hull.sleep()

**Keel:**
1. New `include/keel/async.h` — `KlWatcher`, `KlAsyncOp`, API
2. New `src/async.c` — watcher management, suspend/complete, active ops list
3. `KL_CONN_SUSPENDED` state + fields on `KlConn`
4. Tagged-pointer dispatch in `kl_server_run()` event loop
5. Deadline sweep + dynamic `kl_event_wait` timeout
6. `void *_server_ctx` on `KlRequest`
7. `tests/test_async.c` — watcher + suspend/resume unit tests

**Hull:**
8. `HlAsyncCont` vtable + `HlAsyncCtx` in `include/hull/async.h`
9. `HlLuaAsyncCont` in `runtime/lua/async.c` — implements resume/cancel/destroy via coroutine
10. `HlJsAsyncCont` in `runtime/js/async.c` — implements resume/cancel/destroy via promise
11. Lua dispatch: `lua_pcall` → `lua_newthread` + `lua_resume`
12. JS dispatch: promise detection after `JS_Call()`
13. Sleep (deadline-only, no driver)
14. `hull.sleep()` for both Lua and JS
15. `tests/hull/runtime/test_lua_async.c` — coroutine dispatch tests
16. `tests/hull/runtime/test_js_async.c` — promise dispatch tests
17. `tests/e2e_async.sh` — sleep E2E (responsiveness verification)

### Phase 2: Keel HTTP client + Hull integration

**Keel:**
1. `KlHttpClient` in `src/http_client.c` — async state machine, pure I/O
2. Watcher integration (one socket FD per outbound request)
3. `tests/test_http_client.c` — state machine unit tests
4. `fuzz/fuzz_http_client.c` — response parser fuzzing
5. Keep sync `getaddrinfo()` (phase 4 concern)

**Hull:**
6. `hl_cap_http_request_async` — capability enforcement → `kl_http_client_start`
7. Lua `http.get/post` → `lua_yieldk()` + Keel client
8. JS `http.get/post` → Promise + Keel client
9. E2E tests with local echo server

### Phase 3: hull.gather() + Promise.all

1. Gather context tracking N sub-ops
2. Lua `hull.gather()` — sub-coroutines
3. JS — `Promise.all()` works naturally (multiple ops per suspended conn)
4. E2E: parallel fan-out timing verification

### Phase 4: Thread pool + hardening

**Keel:**
1. `KlThreadPool` in `src/thread_pool.c` — generic thread pool with pipe signaling
2. `tests/test_thread_pool.c`

**Hull + Keel:**
3. Gas metering across yields
4. Graceful shutdown for suspended connections
5. Async DNS via thread pool
6. HTTP/2 connection pool (`KlH2Pool`)

---

## Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Connection lifetime — suspended conn holds parser, response, body reader state | High | `on_cancel` must mirror `kl_conn_release()` cleanup. Test under ASan. |
| Resource leaks — Lua coroutine refs or JS promise handles not freed on cancel | High | Cancel tests with ASan. `on_cancel` frees all runtime refs via vtable. |
| Gas metering gap across yields | Medium | Track consumed instructions before yield, re-arm hook on resume |
| Pool exhaustion via slow async | Medium | Hard deadline cap (configurable, default 30s). Sweep reclaims. |
| Blocking DNS | Medium | Accept in phase 1-2. Thread-based DNS in phase 4. |
| Sleep granularity (sweep = 1000ms) | Low | Dynamic `kl_event_wait()` timeout = min(default, nearest deadline) |

---

## Critical Files

| File | Layer | Changes |
|------|-------|---------|
| `keel/include/keel/async.h` | Keel | New — `KlWatcher`, `KlAsyncOp` primitives |
| `keel/src/async.c` | Keel | New — watcher management, suspend/complete, ops list |
| `keel/include/keel/http_client.h` | Keel | New — `KlHttpClient` async HTTP state machine |
| `keel/src/http_client.c` | Keel | New — non-blocking HTTP client driver |
| `keel/include/keel/connection.h` | Keel | `KL_CONN_SUSPENDED`, `async_op`, `suspend_start_ms` |
| `keel/include/keel/request.h` | Keel | `void *_server_ctx` |
| `keel/src/server.c` | Keel | Tagged-pointer dispatch, deadline sweep, dynamic timeout |
| `keel/src/connection.c` | Keel | Set `_server_ctx` before handler call |
| `keel/tests/test_async.c` | Keel | New — watcher + suspend/resume unit tests |
| `keel/tests/test_http_client.c` | Keel | New — HTTP client state machine tests |
| `keel/fuzz/fuzz_http_client.c` | Keel | New — HTTP response parser fuzzing |
| `hull/include/hull/async.h` | Hull | New — `HlAsyncCont` vtable + `HlAsyncCtx` (runtime-agnostic) |
| `hull/src/hull/cap/http.c` | Hull | Add `hl_cap_http_request_async` (capability check → Keel driver) |
| `hull/src/hull/runtime/lua/async.c` | Hull | New — `HlLuaAsyncCont` (implements vtable: coroutine resume/cancel) |
| `hull/src/hull/runtime/lua/runtime.c` | Hull | `lua_resume` dispatch |
| `hull/src/hull/runtime/lua/modules.c` | Hull | `http.get/post` → `lua_yieldk`, `hull.sleep`, `hull.gather` |
| `hull/src/hull/runtime/js/async.c` | Hull | New — `HlJsAsyncCont` (implements vtable: promise resolve/reject) |
| `hull/src/hull/runtime/js/runtime.c` | Hull | Promise detection |
| `hull/src/hull/runtime/js/modules.c` | Hull | `http.get/post` → Promise, `hull.sleep` |
| `hull/tests/hull/runtime/test_lua_async.c` | Hull | New — Lua coroutine dispatch tests |
| `hull/tests/hull/runtime/test_js_async.c` | Hull | New — JS promise dispatch tests |
| `hull/tests/e2e_async.sh` | Hull | New — async E2E (sleep, HTTP, gather, timeout, stress) |
