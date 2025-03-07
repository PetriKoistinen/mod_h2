/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>

#include <nghttp2/nghttp2.h>

#include "h2_private.h"
#include "h2_util.h"

int h2_util_hex_dump(char *buffer, size_t maxlen,
                     const char *data, size_t datalen)
{
    size_t offset = 0;
    size_t maxoffset = (maxlen-4);
    int i;
    for (i = 0; i < datalen && offset < maxoffset; ++i) {
        const char *sep = (i && i % 16 == 0)? "\n" : " ";
        int n = apr_snprintf(buffer+offset, maxoffset-offset,
                             "%2x%s", ((unsigned int)data[i]&0xff), sep);
        offset += n;
    }
    strcpy(buffer+offset, (i<datalen)? "..." : "");
    return strlen(buffer);
}

int h2_util_header_print(char *buffer, size_t maxlen,
                         const char *name, size_t namelen,
                         const char *value, size_t valuelen)
{
    size_t offset = 0;
    int i;
    for (i = 0; i < namelen && offset < maxlen; ++i, ++offset) {
        buffer[offset] = name[i];
    }
    for (i = 0; i < 2 && offset < maxlen; ++i, ++offset) {
        buffer[offset] = ": "[i];
    }
    for (i = 0; i < valuelen && offset < maxlen; ++i, ++offset) {
        buffer[offset] = value[i];
    }
    buffer[offset] = '\0';
    return offset;
}


char *h2_strlwr(char *s)
{
    for (char *p = s; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
            *p += 'a' - 'A';
        }
    }
    return s;
}

static const int BASE64URL_TABLE[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,
    7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
    37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1
};

apr_size_t h2_util_base64url_decode(unsigned char **decoded, const char *encoded, 
                                    apr_pool_t *pool)
{
    const unsigned char *e = (const unsigned char *)encoded;
    const unsigned char *p = e;
    int n;
    
    while (*p && BASE64URL_TABLE[ *p ] == -1) {
        ++p;
    }
    apr_size_t len = p - e;
    apr_size_t mlen = (len/4)*4;
    *decoded = apr_pcalloc(pool, len+1);
    
    int i = 0;
    unsigned char *d = *decoded;
    for (; i < mlen; i += 4) {
        n = ((BASE64URL_TABLE[ e[i+0] ] << 18) +
             (BASE64URL_TABLE[ e[i+1] ] << 12) +
             (BASE64URL_TABLE[ e[i+2] ] << 6) +
             BASE64URL_TABLE[ e[i+3] ]);
        *d++ = n >> 16;
        *d++ = n >> 8 & 0xffu;
        *d++ = n & 0xffu;
    }
    int remain = len - mlen;
    switch (remain) {
        case 2:
            n = ((BASE64URL_TABLE[ e[mlen+0] ] << 18) +
                 (BASE64URL_TABLE[ e[mlen+1] ] << 12));
            *d++ = n >> 16;
            break;
        case 3:
            n = ((BASE64URL_TABLE[ e[mlen+0] ] << 18) +
                 (BASE64URL_TABLE[ e[mlen+1] ] << 12) +
                 (BASE64URL_TABLE[ e[mlen+2] ] << 6));
            *d++ = n >> 16;
            *d++ = n >> 8 & 0xffu;
            break;
        default: /* do nothing */
            break;
    }
    return len;
}

int h2_util_contains_token(apr_pool_t *pool, const char *s, const char *token)
{
    if (s) {
        if (!apr_strnatcasecmp(s, token)) {   /* the simple life */
            return 1;
        }
        
        for (char *c = ap_get_token(pool, &s, 0); c && *c;
             c = *s? ap_get_token(pool, &s, 0) : NULL) {
            if (!apr_strnatcasecmp(c, token)) { /* seeing the token? */
                return 1;
            }
            while (*s++ == ';') {            /* skip parameters */
                ap_get_token(pool, &s, 0);
            }
            if (*s++ != ',') {               /* need comma separation */
                return 0;
            }
        }
    }
    return 0;
}

const char *h2_util_first_token_match(apr_pool_t *pool, const char *s, 
                                      const char *tokens[], apr_size_t len)
{
    if (s && *s) {
        for (char *c = ap_get_token(pool, &s, 0); c && *c;
             c = *s? ap_get_token(pool, &s, 0) : NULL) {
            for (int i = 0; i < len; ++i) {
                if (!apr_strnatcasecmp(c, tokens[i])) {
                    return tokens[i];
                }
            }
            while (*s++ == ';') {            /* skip parameters */
                ap_get_token(pool, &s, 0);
            }
            if (*s++ != ',') {               /* need comma separation */
                return 0;
            }
        }
    }
    return NULL;
}

/* DEEP_COPY==0 crashes under load. I think the setaside is fine, 
 * however buckets moved to another thread will still be
 * free'd against the old bucket_alloc. *And* if the old
 * pool gets destroyed too early, the bucket disappears while
 * still needed.
 */
static const int DEEP_COPY = 1;
static const int FILE_MOVE = 0;

apr_status_t h2_util_move(apr_bucket_brigade *to, apr_bucket_brigade *from, 
                          apr_size_t maxlen, const char *msg)
{
    apr_status_t status = APR_SUCCESS;
    
    assert(to);
    assert(from);
    int same_alloc = (to->bucket_alloc == from->bucket_alloc);
    
    if (!APR_BRIGADE_EMPTY(from)) {
        apr_bucket *end = NULL;
        apr_bucket *b;
        
        if (maxlen > 0) {
            /* Find the bucket, up to which we reach maxlen bytes */
            for (b = APR_BRIGADE_FIRST(from); 
                 (b != APR_BRIGADE_SENTINEL(from)) && (maxlen > 0);
                 b = APR_BUCKET_NEXT(b)) {
                
                if (b->length == -1) {
                    const char *ign;
                    apr_size_t ilen;
                    status = apr_bucket_read(b, &ign, &ilen, APR_BLOCK_READ);
                    if (status != APR_SUCCESS) {
                        return status;
                    }
                }
                
                if ((same_alloc || FILE_MOVE) && APR_BUCKET_IS_FILE(b)) {
                    /* this has no memory footprint really unless
                     * it is read, disregard it in length count */
                }
                else if (maxlen < b->length) {
                    apr_bucket_split(b, maxlen);
                    maxlen = 0;
                }
                else {
                    maxlen -= b->length;
                }
            }
            end = b;
        }
        
        while (!APR_BRIGADE_EMPTY(from) && status == APR_SUCCESS) {
            b = APR_BRIGADE_FIRST(from);
            if (b == end) {
                break;
            }
            
            if (same_alloc || (b->list == to->bucket_alloc)) {
                /* both brigades use the same bucket_alloc and auto-cleanups
                 * have the same life time. It's therefore safe to just move
                 * directly. */
                APR_BUCKET_REMOVE(b);
                APR_BRIGADE_INSERT_TAIL(to, b);
            }
            else if (DEEP_COPY) {
                /* we have not managed the magic of passing buckets from
                 * one thread to another. Any attempts result in
                 * cleanup of pools scrambling memory.
                 */
                if (APR_BUCKET_IS_METADATA(b)) {
                    if (APR_BUCKET_IS_EOS(b)) {
                        APR_BRIGADE_INSERT_TAIL(to, apr_bucket_eos_create(to->bucket_alloc));
                    }
                    else if (APR_BUCKET_IS_FLUSH(b)) {
                        APR_BRIGADE_INSERT_TAIL(to, apr_bucket_flush_create(to->bucket_alloc));
                    }
                    else {
                        /* ignore */
                    }
                }
                else if (FILE_MOVE && APR_BUCKET_IS_FILE(b)) {
                    /* We do not want to read files when passing buckets, if
                     * we can avoid it. However, what we've come up so far
                     * is not working corrently, resulting either in crashes or
                     * too many open file descriptors.
                     */
                    apr_bucket_file *f = (apr_bucket_file *)b->data;
                    apr_file_t *fd = f->fd;
                    int setaside = (f->readpool != to->p);
                    ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, to->p,
                                  "h2_util: %s, moving FILE bucket %ld-%ld "
                                  "from=%lx(p=%lx) to=%lx(p=%lx), setaside=%d",
                                  msg, (long)b->start, (long)b->length, 
                                  (long)from, (long)from->p, 
                                  (long)to, (long)to->p, setaside);
                    if (setaside) {
                        /*status = apr_bucket_setaside(b, to->p);*/
                        /*status = apr_file_dup(&fd, fd, to->p);*/
                        status = apr_file_setaside(&fd, fd, to->p);
                        if (status != APR_SUCCESS) {
                            ap_log_perror(APLOG_MARK, APLOG_ERR, status, to->p,
                                          "h2_util: %s, dup on FILE", msg);
                            return status;
                        }
                    }
                    apr_brigade_insert_file(to, fd, b->start, b->length, 
                                            to->p);
                }
                else {
                    const char *data;
                    apr_size_t len;
                    status = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
                    if (status == APR_SUCCESS) {
                        status = apr_brigade_write(to, NULL, NULL, data, len);
                    }
                }
                apr_bucket_delete(b);
            }
            else {
                apr_bucket_setaside(b, to->p);
                APR_BUCKET_REMOVE(b);
                APR_BRIGADE_INSERT_TAIL(to, b);
            }
        }
    }
    
    return status;
}

apr_status_t h2_util_pass(apr_bucket_brigade *to, apr_bucket_brigade *from, 
                          apr_size_t maxlen)
{
    apr_status_t status = APR_SUCCESS;
    
    assert(to);
    assert(from);
    
    if (!APR_BRIGADE_EMPTY(from)) {
        apr_bucket *end = NULL;
        apr_bucket *b;
        
        if (maxlen > 0) {
            /* Find the bucket, up to which we reach maxlen bytes */
            for (b = APR_BRIGADE_FIRST(from); 
                 (b != APR_BRIGADE_SENTINEL(from)) && (maxlen > 0);
                 b = APR_BUCKET_NEXT(b)) {
                
                if (b->length == -1) {
                    const char *ign;
                    apr_size_t ilen;
                    status = apr_bucket_read(b, &ign, &ilen, APR_BLOCK_READ);
                    if (status != APR_SUCCESS) {
                        return status;
                    }
                }
                
                if (APR_BUCKET_IS_FILE(b)) {
                    /* this has no memory footprint really unless
                     * it is read, disregard it in length count */
                }
                else if (maxlen < b->length) {
                    apr_bucket_split(b, maxlen);
                    maxlen = 0;
                }
                else {
                    maxlen -= b->length;
                }
            }
            end = b;
        }
        
        while (!APR_BRIGADE_EMPTY(from) && status == APR_SUCCESS) {
            b = APR_BRIGADE_FIRST(from);
            if (b == end) {
                break;
            }
            
            APR_BUCKET_REMOVE(b);
            APR_BRIGADE_INSERT_TAIL(to, b);
        }
    }
    
    return status;
}

int h2_util_has_flush_or_eos(apr_bucket_brigade *bb) {
    apr_bucket *b;
    for (b = APR_BRIGADE_FIRST(bb);
         b != APR_BRIGADE_SENTINEL(bb);
         b = APR_BUCKET_NEXT(b))
    {
        if (APR_BUCKET_IS_EOS(b) || APR_BUCKET_IS_FLUSH(b)) {
            return 1;
        }
    }
    return 0;
}

