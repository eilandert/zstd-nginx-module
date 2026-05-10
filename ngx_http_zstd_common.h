/*
 * Copyright (C) Alex Zhang
 *
 * Shared helpers used by both the filter module and the static module.
 * Included as a static inline header to avoid a separate compilation unit
 * while eliminating the duplication between the two modules.
 */

#ifndef NGX_HTTP_ZSTD_COMMON_H
#define NGX_HTTP_ZSTD_COMMON_H


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


/*
 * ngx_http_zstd_accept_encoding()
 *
 * Returns NGX_OK  if the Accept-Encoding value contains "zstd" with a
 * non-zero quality value (q > 0), NGX_DECLINED otherwise.
 *
 * Implements RFC 7231 §5.3.4 quality-value parsing:
 *   qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] )
 */
static ngx_int_t
ngx_http_zstd_accept_encoding(ngx_str_t *ae)
{
    u_char  *p;

    p = ngx_strcasestrn(ae->data, (char *) "zstd", sizeof("zstd") - 2);
    if (p == NULL) {
        return NGX_DECLINED;
    }

    if (p == ae->data || (*(p - 1) == ',' || *(p - 1) == ' ')) {

        p += sizeof("zstd") - 1;

        if (p == ae->data + ae->len || *p == ',' || *p == ' ' || *p == ';') {
            /* Found "zstd" token; now check quality value if present */
            if (*p == ';') {
                p++;
                /* Skip whitespace */
                while (p < ae->data + ae->len && (*p == ' ' || *p == '\t')) {
                    p++;
                }

                /* Look for q= parameter (RFC 7231) */
                if (p + 1 < ae->data + ae->len
                    && ngx_tolower(p[0]) == 'q' && p[1] == '=')
                {
                    p += 2;
                    /* Skip whitespace after = */
                    while (p < ae->data + ae->len
                           && (*p == ' ' || *p == '\t'))
                    {
                        p++;
                    }

                    /*
                     * Parse quality value.
                     * RFC 7231: weight = OWS ";" OWS "q=" qvalue
                     * qvalue = ( "0" [ "." 0*3DIGIT ] )
                     *        / ( "1" [ "." 0*3("0") ] )
                     * q=0 or q=0.0 (and variants) = not acceptable → decline
                     * q=1 or any q > 0             = acceptable    → accept
                     */
                    if (p < ae->data + ae->len) {
                        if (*p == '0') {
                            p++;

                            /* Just "0" with no decimal = q=0: not acceptable */
                            if (p == ae->data + ae->len
                                || *p == ',' || *p == ' ' || *p == ';')
                            {
                                return NGX_DECLINED;
                            }

                            /* Check for decimal: q=0.xxx */
                            if (*p == '.') {
                                p++;

                                /* All-zero fractional part → not acceptable */
                                while (p < ae->data + ae->len
                                       && *p >= '0' && *p <= '9')
                                {
                                    if (*p != '0') {
                                        /* q=0.xxx (x>0) → accept */
                                        return NGX_OK;
                                    }
                                    p++;
                                }

                                /* q=0.0 / q=0.00 / etc. → not acceptable */
                                return NGX_DECLINED;
                            }

                            /* Malformed: q=0X (lenient) → accept */
                            return NGX_OK;
                        }

                        /* q=1 or any other non-zero value → accept */
                        return NGX_OK;
                    }
                }
            }

            /* No quality value specified → accept */
            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


/*
 * ngx_http_zstd_ok()
 *
 * Returns NGX_OK if the request is a main request whose client advertises
 * acceptable zstd support (Accept-Encoding contains "zstd" with q > 0).
 * Sets r->gzip_tested / r->gzip_ok as side effects for Vary handling.
 */
static ngx_int_t
ngx_http_zstd_ok(ngx_http_request_t *r)
{
    ngx_table_elt_t  *ae;

    if (r != r->main) {
        return NGX_DECLINED;
    }

    ae = r->headers_in.accept_encoding;
    if (ae == NULL) {
        return NGX_DECLINED;
    }

    if (ae->value.len < sizeof("zstd") - 1) {
        return NGX_DECLINED;
    }

    if (ngx_http_zstd_accept_encoding(&ae->value) != NGX_OK) {
        return NGX_DECLINED;
    }

    r->gzip_tested = 1;
    r->gzip_ok = 0;

    return NGX_OK;
}


#endif /* NGX_HTTP_ZSTD_COMMON_H */
