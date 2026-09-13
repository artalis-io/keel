#!/bin/sh
# check_no_httplegacy.sh - the renamed HTTP taxonomy keeps no pre-rename name, and no living doc
# points at a deleted module path.
#
# Logic unchanged from the inline Makefile recipe this replaces; only its home moved. It ran as a
# ~30-line recipe expanding to ~9.5 KB of regex, which Git for Windows' sh failed to parse under
# make (an "unexpected EOF" on the longest line) while the identical text ran fine when executed
# directly. The cause was not worth pinning down further: every other non-trivial gate in this tree
# already lives in tools/, and a script has no recipe-quoting layer to go wrong. Linux CI was always
# fine, so this is about a Windows maintainer running the gate locally BEFORE pushing.
#
# Four self-tests run first and are not optional: each proves a regex still detects what it is for,
# so the gate cannot silently degrade into one that passes by matching nothing.
set -u

HTTPLEGACY_TYPES_RE='\b(KlServerStats|KlServer|KlConfig|KlClientPoolConfig|KlClientPoolConn|KlClientPoolEntry|KlClientPool|KlClientConfig|KlClientResponse|KlClientHeader|KlClientDoneFn|KlClientBodyFn|KlClientHeadersFn|KlClientReadFn|KlClientStreamCfg|KlClientState|KlClientConnectAttempt|KlClient|KlProxyConfig|KlRequestParser|KlRequest|KlResponseParserFactory|KlResponseParser|KlResponse|KlBodyMode|KlWriteFn|KlConnState|KlConnPool|KlConn|KlHandler|KlMiddlewareEntry|KlMiddleware|KlRouter|KlRoute|KlParam|KlBodyReaderFactory|KlCorsConfig|KlBodyReader|KlBufReader|KlMultipartReader|KlMultipartPartMeta|KlMultipartPart|KlMultipartConfig|KlMultipartEvent|KlMultipartErrorCode|KlSse|KlCompressStream|KlRedirectClient|KlRedirectConfig|KlRedirectDoneFn|KlChunkedDecoder|KlChunkedState|KlParserFactory|KlParseResult|KlParser|KlH2ServerSessionFactory|KlH2ServerSession|KlH2ServerConfig|KlH2ServerConn|KlH2ServerCallbacks|KlH2ServerStream|KlH2ServerHooks|KlH2ClientSessionFactory|KlH2ClientSession|KlH2ClientConfig|KlH2ClientConn|KlH2ClientCallbacks|KlH2ClientStream|KlH2ClientResponseFn|KlH2ClientResponse|KlH2ClientHeader|KlH2ClientErrorFn|KlH2Client|KlH2WriteFn|KlH2CompHooks|KlAccessLogFn|KlLogFn|KlTransport)\b'
HTTPLEGACY_CONST_RE='KL_(CONN|BODY|CLIENT|H2|PARSE|CHUNK|MP|CORS|CPOOL|REDIRECT|TRANSPORT|LOG)_|\bKL_READ_BUF_SIZE\b|\bKL_PEER_(SOCKET|PROXY)\b|\bKL_MAX_PARAMS\b|\bKL_DEFAULT_(MAX_CONNS|READ_TIMEOUT|MAX_BODY_SIZE)\b'
HTTPLEGACY_FN_RE='kl_(server|client|request|response|conn|router|cors|body_reader|buf_reader|multipart|sse|redirect|cpool|parser|chunked|h2|comp_h2|compress_stream)_[A-Za-z0-9_]*|\bkl_log(_errno)?\b'
HTTPLEGACY_SCAN='src include tests examples bench fuzz integrations README.md CLAUDE.md AGENTS.md CONTRIBUTING.md docs/architecture/overview.md docs/architecture/invariants.md site/index.html docs/contracts/alpn_policy.md docs/contracts/async_lifecycle.md docs/operations/capability_matrix.md docs/contracts/compatibility.md docs/roadmap/roadmap.md docs/contracts/stream.md docs/contracts/streaming.md docs/architecture/public_api.md'
HTTPLEGACY_FILES_RE='\b(body_reader\.h|body_reader_buffer\.c|body_reader_multipart\.c|body_reader_multipart\.h|chunked\.c|client\.h|client_async\.c|client_common\.c|client_internal\.h|client_pool\.c|client_pool\.h|client_proxy\.c|client_proxy\.h|client_sync\.c|completion_h2\.c|completion_server\.c|conn_internal\.h|connection\.c|connection\.h|cors\.c|cors\.h|h2\.h|h2_client\.h|h2_internal\.h|h2_nghttp2_client\.c|h2_nghttp2_server\.c|h2_server\.h|keel_h2_nghttp2\.h|parser_llhttp\.c|proto_hooks\.c|proto_hooks\.h|redirect\.c|redirect\.h|request\.h|response\.c|response\.h|response_internal\.h|response_parser_llhttp\.c|router\.c|router\.h|server\.c|server\.h|server_activation\.c|server_core\.c|server_h2\.c|server_plat\.h|server_plat_posix\.c|server_plat_win\.c|server_ws\.c|sse\.h)\b|\bsrc/(client|h2_client|sse)\.c\b'

bad=0;
tcanary='KlServer x; KlHttpServer ok';
if ! printf '%s\n' "$tcanary" | grep -qE "$HTTPLEGACY_TYPES_RE"; then
  echo "check-no-httplegacy: SELF-TEST FAILED -- type regex no longer detects KlServer"; exit 1; fi;
ccanary='KL_H2_DEFAULT_MAX_STREAMS x; KL_HTTP2_DEFAULT_MAX_STREAMS ok';
cleak=`printf '%s\n' "$ccanary" | grep -oE "$HTTPLEGACY_CONST_RE"`;
if [ "$cleak" != "KL_H2_" ]; then
  echo "check-no-httplegacy: SELF-TEST FAILED -- constant regex leaked/masked (surfaced: '$cleak')"; exit 1; fi;
fcanary='kl_server_init(a); kl_http_server_init(b)';
fleak=`printf '%s\n' "$fcanary" | grep -oE "$HTTPLEGACY_FN_RE"`;
if [ "$fleak" != "kl_server_init" ]; then
  echo "check-no-httplegacy: SELF-TEST FAILED -- a new kl_http_* name masked/leaked (surfaced: '$fleak')"; exit 1; fi;
types=`grep -rInE "$HTTPLEGACY_TYPES_RE" $HTTPLEGACY_SCAN 2>/dev/null`;
for keep in examples/sse.c examples/h2_client.c examples/client.c h2_server.c; do
  if printf '%s\n' "$keep" | grep -qE "$HTTPLEGACY_FILES_RE"; then
    echo "check-no-httplegacy: SELF-TEST FAILED -- filename regex flagged a retained example ($keep)"; exit 1; fi;
done;
for drop in src/connection.c src/sse.c src/h2_client.c src/client.c server.h h2_client.h; do
  if ! printf '%s\n' "$drop" | grep -qE "$HTTPLEGACY_FILES_RE"; then
    echo "check-no-httplegacy: SELF-TEST FAILED -- filename regex no longer detects deleted $drop"; exit 1; fi;
done;
if [ -n "$types" ]; then echo "$types"; echo "check-no-httplegacy: FAILED -- a renamed HTTP object TYPE reappeared"; bad=1; fi;
consts=`grep -rInE "$HTTPLEGACY_CONST_RE" $HTTPLEGACY_SCAN 2>/dev/null`;
if [ -n "$consts" ]; then echo "$consts"; echo "check-no-httplegacy: FAILED -- a renamed HTTP CONSTANT reappeared"; bad=1; fi;
fns=`grep -rInoE "$HTTPLEGACY_FN_RE" $HTTPLEGACY_SCAN 2>/dev/null`;
if [ -n "$fns" ]; then echo "$fns"; echo "check-no-httplegacy: FAILED -- a renamed kl_http* FUNCTION token reappeared"; bad=1; fi;
files=`grep -rInE "$HTTPLEGACY_FILES_RE" $HTTPLEGACY_SCAN 2>/dev/null`;
if [ -n "$files" ]; then echo "$files"; echo "check-no-httplegacy: FAILED -- a reference to a RENAMED/DELETED module filename reappeared (living docs must not point at nonexistent files)"; bad=1; fi;
if [ $bad -ne 0 ]; then exit 1; fi;
echo "check-no-httplegacy: OK (types/constants/functions/filenames -- no legacy HTTP name or deleted-module path in code or living docs)"
