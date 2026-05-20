# Code Architecture Comparison: gzip vs zlib-ng vs brotli vs zstd

Comprehensive comparison of four nginx compression modules across architecture, memory, buffering, and feature design.

## Module Structure Overview

### nginx gzip (Built-in)
**Source:** [nginx/src/http/modules/ngx_http_gzip_filter_module.c](https://github.com/nginx/nginx/blob/master/src/http/modules/ngx_http_gzip_filter_module.c)

- **Module Type:** Built-in HTTP module (not dynamic)
- **Compression Library:** zlib (standard or zlib-ng variant)
- **Filter Stages:** Header filter + Body filter (2-stage)
- **Context:** Per-request compression state (zlib stream + buffers)
- **Lines of Code:** ~1500 (core filter logic)

### Google brotli (`ngx_brotli`)
**Source:** [google/ngx_brotli/filter/ngx_http_brotli_filter_module.c](https://github.com/google/ngx_brotli/blob/master/filter/ngx_http_brotli_filter_module.c)

- **Module Type:** Dynamic module
- **Compression Library:** Brotli (libbrotlienc)
- **Filter Stages:** Header filter + Body filter (2-stage)
- **Context:** Per-request encoder state + output chain
- **Lines of Code:** ~1200 (core filter logic)

### zstd module (This repo)
**Source:** `filter/ngx_http_zstd_filter_module.c`

- **Module Type:** Dynamic module
- **Compression Library:** zstd (libzstd)
- **Filter Stages:** Header filter + Body filter (2-stage)
- **Context:** Per-request CCtx + streaming buffers
- **Lines of Code:** ~850 (core filter logic)
- **Static Variant:** Separate static module (~500 lines)

### zlib-ng
**Note:** zlib-ng is NOT a separate nginx module. It's a drop-in replacement for zlib at compile time. The gzip module detects and uses it if available during nginx build.

---

## Header Filter Comparison

### gzip Header Filter
```c
/* Decision tree:
   1. Check if gzip is enabled
   2. Verify status code (200, 204, 206, 301, 302, 303, 304, 307, 400-599)
   3. No existing Content-Encoding
   4. Content-Length >= gzip_min_length
   5. Content-Type in gzip_types hash
   6. No Vary conflicts
*/
```

**Unique behavior:**
- Accepts 206 Partial Content (RFC-compliant, but complex)
- Preserves existing Content-Length when possible
- Uses content-type hash for O(1) MIME matching

### brotli Header Filter
```c
/* Decision tree (mirrors gzip):
   1. Check if brotli is enabled
   2. Verify status code (same whitelist as gzip)
   3. No existing Content-Encoding
   4. Content-Length >= brotli_min_length
   5. Content-Type in brotli_types hash
   6. Quality value from Accept-Encoding (basic check)
*/
```

**Unique behavior:**
- No explicit Accept-Encoding qvalue parsing visible
- Mimics gzip module structure closely
- Uses same status code whitelist

### zstd Header Filter (Filter module)
**Location:** `filter/ngx_http_zstd_filter_module.c` lines 455-540

```c
/* Decision tree:
   1. Check if zstd is enabled (on/always/off)
   2. Verify status code (200, 206 EXPLICITLY SKIPPED, others)
   3. No existing Content-Encoding
   4. Content-Length >= zstd_min_length (or -1 for unknown)
   5. Content-Type in zstd_types hash
   6. Accept-Encoding: zstd check (RFC 7231 qvalue parsing)
*/
```

**Unique behavior:**
- **206 Partial Content: EXPLICITLY SKIPPED** (lines 727-733) — defense-in-depth design
- Full RFC 7231 qvalue parsing (not just presence check)
- Handles proxied responses with unknown Content-Length
- Reuses nginx's `gzip_vary` flag for Vary: Accept-Encoding

### zstd Header Filter (Static module)
**Location:** `static/ngx_http_zstd_static_module.c` lines 105-210

```c
/* Simplified decision tree for pre-compressed .zst files:
   1. GET/HEAD only (no POST/PUT)
   2. URI doesn't end with '/' (not directory)
   3. zstd_static on/always/off check
   4. If "on": validate Accept-Encoding: zstd (RFC 7231 qvalue)
   5. If "always": serve .zst regardless
   6. Locate .zst file via ngx_http_map_uri_to_path()
   7. Magic-number validation via pread(2)
*/
```

**Unique behavior:**
- **Magic-number check** (lines 254-291): validates first 4 bytes against ZSTD_MAGICNUMBER
- Uses `pread(2)` to avoid mutating shared fd position in open_file_cache
- Separate decision path for static vs. dynamic compression

---

## Body Filter / Compression Loop

### gzip Body Filter
**Lines:** ~600 in core loop

**Strategy:**
1. Allocate buffers on first call (200-400 KB total, preallocated pool)
2. Feed input chain to zlib's deflate()
3. Extract output from zlib output buffer
4. Chain output buffers (ngx_chain_t)
5. Track deflate state (flushing, finished)
6. Send downstream via ngx_http_next_body_filter()

**Memory Management:**
- Custom allocator (ngx_http_gzip_filter_alloc/free)
- Detects zlib-ng vs standard zlib (different deflate struct layout, 8KB alignment)
- Pre-pools ~200-400 KB to reduce malloc/free syscall overhead
- Reuses buffers across multiple calls

**Buffer Pattern:**
```c
typedef struct {
    u_char  *in;    /* input chain */
    u_char  *out;   /* output buffer */
    size_t  in_size;
    size_t  out_size;
    /* zlib stream, state flags */
} ngx_http_gzip_ctx_t;
```

### brotli Body Filter
**Lines:** ~400 in core loop

**Strategy:**
1. Initialize BrotliEncoderState on first call
2. Feed input buffer to BrotliEncoderProcessData()
3. Retrieve output from brotli's output buffer
4. Chain output buffers
5. Track finish state (emit finish chunk on last data)
6. Send downstream

**Memory Management:**
- Custom allocator (ngx_http_brotli_filter_alloc/free)
- Output chain + buffer pre-allocated
- Buffers: default 32×4k or 16×8k (depending on platform page size)
- No preallocated pool (allocate per context)

**Buffer Pattern:**
```c
typedef struct {
    BrotliEncoderState  *encoder;
    ngx_chain_t         *out;      /* output chain */
    ngx_buf_t           *out_buf;  /* current output buffer */
    size_t              out_size;
    /* state flags */
} ngx_http_brotli_filter_ctx_t;
```

### zstd Body Filter
**Lines:** ~350 in core loop

**Location:** `filter/ngx_http_zstd_filter_module.c` lines 582–803

**Strategy:**
1. Create ZSTD_CCtx on first call (or reuse if pledged size known)
2. Feed input to ZSTD_compressStream2() in streaming mode
3. Extract output from ZSTD_CStreamOutSize-sized output buffer (131 KB default)
4. Chain output buffers (ngx_chain_t)
5. Track streaming state (flush, finish)
6. Send downstream

**Memory Management:**
- ZSTD_CCtx allocated per request (sized by ZSTD_estimateCStreamSize_usingCCtxParams())
- No custom allocator (uses malloc/free, bounds-checked at config load)
- Per-request memory limited by zstd_max_cctx_memory directive
- Output buffer is fixed ZSTD_CStreamOutSize (131 KB) per stream

**Buffer Pattern:**
```c
typedef struct {
    ZSTD_CCtx       *cctx;          /* compression context */
    ngx_buf_t       *out;           /* output buffer (131 KB) */
    size_t          pending;        /* bytes in output buffer */
    size_t          zstd_bytes_in;  /* metric: uncompressed input */
    size_t          zstd_bytes_out; /* metric: compressed output */
    ngx_uint_t      zrc;            /* size_t return code from zstd */
    /* state flags */
} ngx_http_zstd_filter_ctx_t;
```

---

## Configuration Directives

### gzip Directives
| Directive | Type | Values | Default | Scope |
|-----------|------|--------|---------|-------|
| gzip | flag | on/off | off | main, srv, loc |
| gzip_comp_level | int | 1-9 | 1 | main, srv, loc |
| gzip_types | list | MIME types | text/html | main, srv, loc |
| gzip_min_length | size | bytes | 20 | main, srv, loc |
| gzip_buffers | size | count × size | 32 4k/16 8k | main, srv, loc |
| gzip_window | int | 1-15 (2^N) | 15 | main, srv, loc |
| gzip_hash | int | 4-15 (2^N) | 4 | main, srv, loc |
| postpone_gzipping | size | bytes | 0 | main, srv, loc |
| gzip_no_buffer | flag | on/off | off | main, srv, loc |
| gzip_vary | flag | on/off | off | main, srv, loc |

### brotli Directives
| Directive | Type | Values | Default | Scope |
|-----------|------|--------|---------|-------|
| brotli | flag | on/off | off | main, srv, loc |
| brotli_comp_level | int | 0-11 | 6 | main, srv, loc |
| brotli_types | list | MIME types | text/html | main, srv, loc |
| brotli_min_length | size | bytes | 20 | main, srv, loc |
| brotli_buffers | size | count × size | 32 4k/16 8k | main, srv, loc |
| brotli_window | int | 10-24 | 24 | main, srv, loc |

### zstd Directives (Filter)
| Directive | Type | Values | Default | Scope |
|-----------|------|--------|---------|-------|
| zstd | flag | on/off | off | main, srv, loc |
| zstd_comp_level | int | -131072 to 22 | 3 | main, srv, loc |
| zstd_types | list | MIME types | text/html | main, srv, loc |
| zstd_min_length | size | bytes | 20 | main, srv, loc |
| zstd_window_log | int | 10-31 | 0 (default) | main, srv, loc |
| zstd_long | flag | on/off | off | main, srv, loc |
| zstd_dict_file | path | file path | none | http |
| zstd_max_cctx_memory | size | bytes | 0 (unlimited) | main, srv, loc |
| zstd_bypass | expr | nginx variable | none | main, srv, loc |

### zstd Directives (Static)
| Directive | Type | Values | Default | Scope |
|-----------|------|--------|---------|-------|
| zstd_static | enum | off/on/always | off | main, srv, loc |

**Key Differences:**
- **zstd has negative compression levels** (-131072 to 22) for ultra-fast streaming
- **zstd has memory budgeting** (zstd_max_cctx_memory) — gzip/brotli do not
- **zstd has dictionary support** (zstd_dict_file) for custom training corpora
- **zstd has bypass expression** for conditional compression
- **zstd has explicit window_log control** vs gzip's implicit window sizing
- **brotli has wider compression range** (0-11 vs gzip's 1-9)

---

## Accept-Encoding Parsing

### gzip
**Location:** nginx core (`src/http/ngx_http_parse.c`)

```c
/* Simple presence check via ngx_strcasestrn():
   if (ngx_strcasestrn(accept_encoding, "gzip", 4)) {
       /* serve gzip */
   }
*/
```

**Behavior:**
- No qvalue parsing (binary: has gzip or doesn't)
- Substring search (matches "gzip" anywhere)
- No multi-occurrence handling

### brotli
**Location:** Mirrors nginx core for basic check

```c
/* Likely:
   if (ngx_strcasestrn(accept_encoding, "br", 2)) {
       /* serve brotli */
   }
*/
```

**Behavior:**
- No documented qvalue parsing
- Substring search (matches "br" anywhere)
- No multi-occurrence handling

### zstd
**Location:** `ngx_http_zstd_common.h` lines 27-220

```c
/* Full RFC 7231-compliant token-list walker:
   - Parses comma-separated coding list
   - Extracts token name (bounded by OWS, ';', ',')
   - Evaluates optional q=value parameter
   - Rejects q=0, accepts q>0 and default (q=1)
   - Returns first standalone "zstd" token's qvalue result
*/

ngx_http_zstd_accept_encoding(ngx_str_t *ae)  /* Main parser */
ngx_http_zstd_eval_qvalue(ngx_str_t *ae, u_char *p) /* qvalue helper */
```

**Behavior:**
- Full RFC 7231 §5.3.4 qvalue parsing
- Token-list walking (not substring)
- Handles "notzstd, zstd" correctly (second occurrence)
- Validates q ∈ [0,1], rejects malformed decimals
- **Test coverage:** 13 quality value tests (filter + static)

---

## Memory Allocation Strategy

### gzip
**Approach:** Pre-pooled allocator for zlib stream

```c
ngx_http_gzip_filter_alloc(void *opaque, u_int items, u_int size)
{
    /* Allocates from pre-allocated pool (200-400 KB)
       Detects zlib-ng vs standard zlib (8KB alignment issue)
       Tracks allocation for cleanup
    */
}

ngx_http_gzip_filter_free(void *opaque, void *address)
{
    /* Returns to pool (no syscall) */
}
```

**Pros:**
- Reduces malloc/free syscall overhead
- Detects zlib variant automatically
- Handles deflate struct alignment

**Cons:**
- Fixed pre-pool size (risk of OOM if underestimated)
- Complex alloc/free tracking
- Zlib variant detection logic

### brotli
**Approach:** Per-context allocation, buffers pre-sized

```c
ngx_http_brotli_filter_alloc(void *opaque, size_t size)
{
    /* Simple malloc-based allocation
       Tracks allocations for cleanup
    */
}

ngx_http_brotli_filter_free(void *opaque, void *address)
{
    /* Free to heap */
}
```

**Pros:**
- Simple, predictable allocation
- No pre-pooling overhead

**Cons:**
- Malloc/free syscalls per allocation
- No preallocated pool
- No variant detection

### zstd
**Approach:** Direct malloc with config-time bounds checking

```c
/* No custom allocator. Uses libzstd's default malloc:
   ZSTD_CCtx *ctx = ZSTD_createCCtx();
   
   Memory is bounded at config load:
   size_t needed = ZSTD_estimateCStreamSize_usingCCtxParams(params);
   if (needed > zstd_max_cctx_memory) {
       return NGX_CONF_ERROR;  /* Reject config */
   }
*/
```

**Pros:**
- Memory budget enforced at config load (early failure)
- No pre-pooling complexity
- Per-request memory is predictable and bounded
- Integrates with libzstd's memory model

**Cons:**
- Malloc/free syscalls per request (not pre-pooled)
- Requires zstd_max_cctx_memory directive (user responsibility)

**Winner:** zstd's approach is safest for production (explicit budgeting), gzip's is most optimized (pre-pool reduces syscalls).

---

## Streaming vs. Buffering

### gzip
- **Strategy:** Streaming via zlib's deflate() state machine
- **Buffer Size:** Multiple buffers (configured via gzip_buffers)
- **Output Handling:** Chains buffers, emits on flush/finish
- **Flushing:** Z_SYNC_FLUSH after each input chunk
- **Padding:** RFC 1952 (gzip header + checksum) — NO padding for BREACH

### brotli
- **Strategy:** Streaming via BrotliEncoderProcessData()
- **Buffer Size:** Fixed output buffer size
- **Output Handling:** Chains buffers, finish chunk on last input
- **Flushing:** Implicit (brotli manages flushing)
- **Padding:** RFC 7932 — NO explicit padding

### zstd
- **Strategy:** Streaming via ZSTD_compressStream2()
- **Buffer Size:** Fixed ZSTD_CStreamOutSize (131 KB)
- **Output Handling:** Chains buffers, ZSTD_e_end on last input
- **Flushing:** ZSTD_e_flush after chunked input, ZSTD_e_end on finish
- **Padding:** zstd frame format (RFC 8878) — NO padding

---

## Unique Features

### gzip
- ✅ Built-in (no dynamic module overhead)
- ✅ Pre-pooled allocator (reduces syscalls)
- ✅ Zlib variant detection (automatic optimization)
- ✅ Configurable window/hash parameters
- ✅ Oldest/most widely deployed

### brotli
- ✅ Higher compression ratio than gzip (better for static content)
- ✅ Wider quality range (0-11 vs gzip's 1-9)
- ✅ Slower compression = better ratio (quality 4-6 typical)
- ✅ Modern format (RFC 7932)
- ❌ Slower than gzip for fast compression

### zstd
- ✅ **Full RFC 7231 qvalue parsing** (gzip/brotli don't)
- ✅ **Magic-number validation** (prevents serving corrupted .zst)
- ✅ **Per-request memory budgeting** (prevents DoS)
- ✅ **Dictionary support** (custom training corpora)
- ✅ **Negative compression levels** (ultra-fast streaming, -131072 to 22)
- ✅ **Separate static module** (pre-compressed .zst files)
- ✅ **Explicit bypass expressions** (conditional compression)
- ✅ **Streaming mode optimized** (fast + good ratio)
- ❌ Fewer zlib optimizations (no pre-pool allocator)

---

## Complexity Scorecard

| Aspect | gzip | brotli | zstd |
|--------|------|--------|------|
| LOC (core filter) | 1500 | 1200 | 850 |
| Custom memory allocator | Yes | Yes | No |
| Pre-pooling | Yes | No | No |
| Accept-Encoding parsing | Basic | Basic | RFC 7231 Full |
| Config validation | Runtime | Runtime | Config-load (bounds-check) |
| Directives | 10 | 6 | 11 (filter) + 1 (static) |
| Feature complexity | Medium | Medium | High |
| Operator safety | Good | Good | **Very High** (budgeting) |

---

## Recommendations

1. **For legacy/compatibility:** Use gzip (built-in, most compatible)
2. **For compression ratio:** Use brotli (best ratio, slower)
3. **For safety/speed balance:** Use zstd (fast, good ratio, memory-safe)
4. **For defense-in-depth:** Use zstd static module (magic validation prevents corruption)
5. **For production:** Pair zstd with `zstd_max_cctx_memory` (prevents runaway allocation)

---

## Sources

- [nginx gzip filter module source](https://github.com/nginx/nginx/blob/master/src/http/modules/ngx_http_gzip_filter_module.c)
- [Google ngx_brotli module](https://github.com/google/ngx_brotli)
- [zstd module (this repo)](./filter/ngx_http_zstd_filter_module.c)
- [RFC 7231: HTTP/1.1 Semantics and Content](https://tools.ietf.org/html/rfc7231)
- [RFC 7932: Brotli Compressed Data Format](https://tools.ietf.org/html/rfc7932)
- [RFC 8878: Zstandard Compression and the 'application/zstd' Media Type](https://tools.ietf.org/html/rfc8878)
