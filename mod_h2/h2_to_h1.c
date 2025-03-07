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
#include <stdio.h>

#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_connection.h>

#include "h2_private.h"
#include "h2_bucket.h"
#include "h2_mplx.h"
#include "h2_response.h"
#include "h2_to_h1.h"
#include "h2_util.h"

#define HTTP_RLINE_SUFFIX       " HTTP/1.1\r\n"
#define HTTP_RLINE_SUFFIX_LEN   11

static const apr_off_t HEADERSIZE      = 16 * 1024;


struct h2_to_h1 {
    h2_bucket *data;
    int stream_id;
    h2_mplx *m;
    int eoh;
    int eos;
    int flushed;
    int seen_host;
    int chunked;
    apr_size_t remain_len;
};

h2_to_h1 *h2_to_h1_create(int stream_id, apr_pool_t *pool, h2_mplx *m)
{
    h2_to_h1 *to_h1 = apr_pcalloc(pool, sizeof(h2_to_h1));
    if (to_h1) {
        to_h1->stream_id = stream_id;
        to_h1->m = m;
    }
    return to_h1;
}

void h2_to_h1_destroy(h2_to_h1 *to_h1)
{
    if (to_h1->data) {
        h2_bucket_destroy(to_h1->data);
        to_h1->data = NULL;
    }
}

static apr_status_t ensure_data(h2_to_h1 *to_h1)
{
    if (!to_h1->data) {
        to_h1->data = h2_bucket_alloc(HEADERSIZE);
        if (!to_h1->data) {
            return APR_ENOMEM;
        }
    }
    return APR_SUCCESS;
}


apr_status_t h2_to_h1_start_request(h2_to_h1 *to_h1,
                                    int stream_id,
                                    const char *method,
                                    const char *path,
                                    const char *authority)
{
    apr_status_t status = APR_SUCCESS;
    if (!method) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, h2_mplx_get_conn(to_h1->m),
                      "h2_to_h1: header start but :method missing");
        return APR_EGENERAL;
    }
    if (!path) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, h2_mplx_get_conn(to_h1->m),
                      "h2_to_h1: header start but :path missing");
        return APR_EGENERAL;
    }
    
    status = ensure_data(to_h1);
    if (status != APR_SUCCESS) {
        return status;
    }
    
    size_t mlen = strlen(method);
    size_t plen = strlen(path);
    size_t total = mlen + 1 + plen + HTTP_RLINE_SUFFIX_LEN;
    if (!h2_bucket_has_free(to_h1->data, total)) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_ENAMETOOLONG,
                      h2_mplx_get_conn(to_h1->m), "h2_to_h1: adding request line");
        return APR_ENAMETOOLONG;
    }
    h2_bucket_append(to_h1->data, method, mlen);
    h2_bucket_append(to_h1->data, " ", 1);
    h2_bucket_append(to_h1->data, path, plen);
    h2_bucket_append(to_h1->data, HTTP_RLINE_SUFFIX, HTTP_RLINE_SUFFIX_LEN);

    if (authority) {
        status = h2_to_h1_add_header(to_h1, "Host", 4,
                                     authority, strlen(authority));
    }
    return status;
}


static apr_status_t append_header(h2_bucket *bucket,
                                  const char *name, size_t nlen,
                                  const char *value, size_t vlen)
{
    if (nlen > 0) {
        size_t total = nlen + vlen + 4;
        if (!h2_bucket_has_free(bucket, total)) {
            return APR_ENAMETOOLONG;
        }
        h2_bucket_append(bucket, name, nlen);
        h2_bucket_append(bucket, ": ", 2);
        if (vlen > 0) {
            h2_bucket_append(bucket, value, vlen);
        }
        h2_bucket_append(bucket, "\r\n", 2);
    }
    return APR_SUCCESS;
}

apr_status_t h2_to_h1_add_header(h2_to_h1 *to_h1,
                                 const char *name, size_t nlen,
                                 const char *value, size_t vlen)
{
    if (H2_HD_MATCH_LIT("transfer-encoding", name, nlen)) {
        if (!apr_strnatcasecmp("chunked", value)) {
            to_h1->chunked = 1;
        }
    }
    else if (H2_HD_MATCH_LIT("content-length", name, nlen)) {
        char *end;
        to_h1->remain_len = apr_strtoi64(value, &end, 10);
        if (value == end) {
            ap_log_cerror(APLOG_MARK, APLOG_WARNING, APR_EINVAL, 
                          h2_mplx_get_conn(to_h1->m),
                          "h2_request(%d): content-length value not parsed: %s",
                          to_h1->stream_id, value);
            return APR_EINVAL;
        }
    }
    else if ((to_h1->seen_host && H2_HD_MATCH_LIT("host", name, nlen))
             || H2_HD_MATCH_LIT("expect", name, nlen)
             || H2_HD_MATCH_LIT("upgrade", name, nlen)
             || H2_HD_MATCH_LIT("connection", name, nlen)
             || H2_HD_MATCH_LIT("proxy-connection", name, nlen)
             || H2_HD_MATCH_LIT("keep-alive", name, nlen)
             || H2_HD_MATCH_LIT("http2-settings", name, nlen)) {
        // ignore these.
        return APR_SUCCESS;
    }

    apr_status_t status = ensure_data(to_h1);
    if (status == APR_SUCCESS) {
        status = append_header(to_h1->data, name, nlen, value, vlen);
        if (status == APR_ENAMETOOLONG && to_h1->data->data_len > 0) {
            /* header did not fit into bucket, push bucket to input and
             * get a new one */
            status = h2_to_h1_flush(to_h1);
            if (status == APR_SUCCESS) {
                apr_status_t status = ensure_data(to_h1);
                if (status == APR_SUCCESS) {
                    status = append_header(to_h1->data, name, nlen, 
                                           value, vlen);
                }
                /* if this still does not work, we fail */
            }
        }
        if (!to_h1->seen_host && H2_HD_MATCH_LIT("host", name, nlen)) {
            to_h1->seen_host = 1;
        }
    }
    return status;
}


apr_status_t h2_to_h1_end_headers(h2_to_h1 *to_h1)
{
    if (to_h1->eoh) {
        return APR_EINVAL;
    }
    
    apr_status_t status = ensure_data(to_h1);
    if (status != APR_SUCCESS) {
        return status;
    }
    
    if (!h2_bucket_has_free(to_h1->data, 2)) {
        status = h2_to_h1_flush(to_h1);
    }
    
    if (status == APR_SUCCESS) {
        h2_bucket_cat(to_h1->data, "\r\n");
        status = h2_to_h1_flush(to_h1);
    }
    to_h1->eoh = 1;

    return status;
}

static apr_status_t h2_to_h1_add_data_raw(h2_to_h1 *to_h1,
                                          const char *data, size_t len)
{
    if (to_h1->eos || !to_h1->eoh) {
        return APR_EINVAL;
    }
    
    apr_status_t status = APR_SUCCESS;
    while (len > 0 && status == APR_SUCCESS) {
        status = ensure_data(to_h1);
        if (status == APR_SUCCESS) {
            apr_size_t written = h2_bucket_append(to_h1->data, data, len);
            if (written >= len) {
                /* transferred all there is, return. */
                return APR_SUCCESS;
            }
            /* not all written. flush and write rest */
            len -= written;
            data += written;
            status = h2_to_h1_flush(to_h1);
        }
    }
    return status;
}


apr_status_t h2_to_h1_add_data(h2_to_h1 *to_h1,
                               const char *data, size_t len)
{
    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, h2_mplx_get_conn(to_h1->m),
                  "h2_to_h1(%d): add %ld data bytes", 
                  to_h1->stream_id, (long)len);
    
    if (to_h1->chunked) {
        /* if input may have a body and we have not seen any
         * content-length header, we need to chunk the input data.
         */
        char buffer[32];
        size_t chunklen = sprintf(buffer, "%lx\r\n", len);
        
        apr_status_t status = h2_to_h1_add_data_raw(to_h1, buffer, chunklen);
        if (status == APR_SUCCESS) {
            status = h2_to_h1_add_data_raw(to_h1, data, len);
            if (status == APR_SUCCESS) {
                status = h2_to_h1_add_data_raw(to_h1, "\r\n", 2);
            }
        }
        return status;
    }
    
    return h2_to_h1_add_data_raw(to_h1, data, len);
}

apr_status_t h2_to_h1_flush(h2_to_h1 *to_h1)
{
    apr_status_t status = APR_SUCCESS;
    if (to_h1->data && to_h1->data->data_len) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, h2_mplx_get_conn(to_h1->m),
                      "h2_to_h1(%d): flush %ld data bytes", 
                      to_h1->stream_id, to_h1->data->data_len);
        
        status = h2_mplx_in_write(to_h1->m, to_h1->stream_id, to_h1->data);
        to_h1->data = NULL;
        if (status != APR_SUCCESS) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, status,
                          h2_mplx_get_conn(to_h1->m),
                          "h2_request(%d): pushing request data",
                          to_h1->stream_id);
        }
    }
    return status;
}

apr_status_t h2_to_h1_close(h2_to_h1 *to_h1)
{
    apr_status_t status = APR_SUCCESS;
    if (!to_h1->eos) {
        to_h1->eos = 1;
        if (to_h1->chunked) {
            status = h2_to_h1_add_data_raw(to_h1, "0\r\n\r\n", 5);
        }
        status = h2_to_h1_flush(to_h1);
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, h2_mplx_get_conn(to_h1->m),
                      "h2_to_h1(%d): close", to_h1->stream_id);
        
        status = h2_mplx_in_close(to_h1->m, to_h1->stream_id);
    }
    return status;
}


