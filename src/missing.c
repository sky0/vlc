/*****************************************************************************
 * missing.c: missing libvlccore symbols
 *****************************************************************************
 * Copyright (C) 2008 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/** \file
 * This file contains dummy replacement API for disabled features
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <assert.h>

#ifndef ENABLE_HTTPD
# include <vlc_httpd.h>

char *httpd_ClientIP (const httpd_client_t *cl, char *psz_ip)
{
    assert (0);
}

void httpd_ClientModeBidir (httpd_client_t *cl)
{
    assert (0);
}

void httpd_ClientModeStream (httpd_client_t *cl)
{
    assert (0);
}

httpd_file_sys_t *httpd_FileDelete (httpd_file_t *file)
{
    assert (0);
}

httpd_file_t *httpd_FileNew (httpd_host_t *host,
                             const char *url, const char *content_type,
                             const char *login, const char *password,
                             const vlc_acl_t *acl,
                             httpd_file_callback_t cb, httpd_file_sys_t *data)
{
    assert (0);
}

httpd_handler_sys_t *httpd_HandlerDelete (httpd_handler_t *handler)
{
    assert (0);
}

httpd_handler_t *httpd_HandlerNew (httpd_host_t *host, const char *url,
                                   const char *login, const char *password,
                                   const vlc_acl_t *acl,
                                   httpd_handler_callback_t cb,
                                   httpd_handler_sys_t *data)
{
    assert (0);
}

void httpd_HostDelete (httpd_host_t *h)
{
    assert (0);
}

httpd_host_t *httpd_HostNew (vlc_object_t *obj, const char *host, int port)
{
    return httpd_TLSHostNew (obj, host, port, NULL, NULL, NULL, NULL);
}

void httpd_MsgAdd (httpd_message_t *m, const char *name, const char *fmt, ...)
{
    assert (0);
}

const char *httpd_MsgGet (const httpd_message_t *m, const char *name)
{
    assert (0);
}

void httpd_RedirectDelete (httpd_redirect_t *r)
{
    assert (0);
}

httpd_redirect_t *httpd_RedirectNew (httpd_host_t *host,
                                     const char *dst, const char *src)
{
    assert (0);
}

char *httpd_ServerIP (const httpd_client_t *client, char *ip)
{
    assert (0);
}

void httpd_StreamDelete (httpd_stream_t *stream)
{
    assert (0);
}

int httpd_StreamHeader (httpd_stream_t *stream, uint8_t *data, int count)
{
    assert (0);
}

httpd_stream_t *httpd_StreamNew (httpd_host_t *host,
                                 const char *url, const char *content_type,
                                 const char *login, const char *password,
                                 const vlc_acl_t *acl)
{
    assert (0);
}

int httpd_StreamSend (httpd_stream_t *stream, uint8_t *data, int count)
{
    assert (0);
}

httpd_host_t *httpd_TLSHostNew (vlc_object_t *obj, const char *host, int port,
                                const char *cert, const char *key,
                                const char *ca, const char *crl)
{
     msg_Err (obj, "VLC httpd support not compiled-in!");
     return NULL;
}

int httpd_UrlCatch (httpd_url_t *url, int request, httpd_callback_t cb,
                    httpd_callback_sys_t *data)
{
    assert (0);
}

void httpd_UrlDelete (httpd_url_t *url)
{
    assert (0);
}

httpd_url_t *httpd_UrlNew (httpd_host_t *host, const char *url,
                           const char *login, const char *password,
                           const vlc_acl_t *acl)
{
    assert (0);
}

httpd_url_t *httpd_UrlNewUnique (httpd_host_t *host, const char *url,
                                 const char *login, const char *password,
                                 const vlc_acl_t *acl)
{
    assert (0);
}
#endif /* !ENABLE_HTTPD */

#ifndef ENABLE_SOUT
# include <vlc_sout.h>

char *sdp_AddMedia (char **sdp, const char *type, const char *protocol,
                    int dport, unsigned pt, bool bw_indep, unsigned bw,
                    const char *ptname, unsigned clockrate, unsigned channels,
                    const char *fmtp)
{
    assert (*sdp == NULL);
    return NULL;
}

char *sdp_AddAttribute (char **sdp, const char *name, const char *fmt, ...)
{
    assert (*sdp == NULL);
    return NULL;
}

int sout_AccessOutControl (sout_access_out_t *out, int query, ...)
{
    assert (0);
}

void sout_AccessOutDelete (sout_access_out_t *out)
{
    assert (0);
}

#undef sout_AccessOutNew
sout_access_out_t *sout_AccessOutNew (vlc_object_t *obj,
                                      const char *access, const char *name)
{
    msg_Err (obj, "Output support not compiled-in!");
    return NULL;
}

ssize_t sout_AccessOutRead (sout_access_out_t *out, block_t *block)
{
    assert (0);
}

int sout_AccessOutSeek (sout_access_out_t *out, off_t offset)
{
    assert (0);
}

ssize_t sout_AccessOutWrite (sout_access_out_t *out, block_t *block)
{
    assert (0);
}

#undef sout_AnnounceRegisterSDP
session_descriptor_t *sout_AnnounceRegisterSDP (vlc_object_t *obj,
                                                const char *sdp,
                                                const char *dst,
                                                announce_method_t *method)
{
    msg_Err (obj, "SDP export not compiled-in!");
    assert (method == NULL);
    return NULL;
}

#undef sout_AnnounceUnRegister
sout_AnnounceUnRegister (vlc_object_t *obj, session_descriptor_t *d)
{
    assert (0);
}

void sout_MethodRelease (announce_method_t *method)
{
    (void)method;
}

sout_input_t *sout_MuxAddStream (sout_mux_t *mux, es_format_t *fmt)
{
    assert (0);
}

void sout_MuxDelete (sout_mux_t *mux)
{
    assert (0);
}

void sout_MuxDeleteStream (sout_mux_t *mux, sout_input_t *input)
{
    assert (0);
}

sout_mux_t *sout_MuxNew (sout_instance_t *instance, char *mux,
                         sout_access_out_t *out)
{
    assert (0);
}

void sout_MuxSendBuffer (sout_mux_t *mux, sout_input_t *input, block_t *block)
{
    assert (0);
}

announce_method_t *sout_SAPMethod (void)
{
    return NULL;
}

void sout_StreamDelete (sout_stream_t *stream)
{
    assert (0);
}

sout_stream_t *sout_StreamNew (sout_instance_t *instance, char *chain)
{
    assert (0);
}

void sout_UpdateStatistic (sout_instance_t *instance, sout_statistic_t stat,
                           int value)
{
    assert (0);
}

char *vlc_sdp_Start (vlc_object_t *obj, const char *cfg,
                     const struct sockaddr *src, size_t srclen,
                     const struct sockaddr *addr, size_t addrlen)
{
    return NULL;
}

# ifndef ENABLE_VLM
#  include <vlc_vlm.h>

int vlm_Control (vlm_t *vlm, int query, ...)
{
    assert (0);
}

void vlm_Delete (vlm_t *vlm)
{
    assert (0);
}

int vlm_ExecuteCommand (vlm_t *vlm, const char *cmd, vlm_message_t **pm)
{
    assert (0);
}

vlm_message_t *vlm_MessageAdd (vlm_message_t *a, vlm_message_t *b)
{
    assert (0);
}

void vlm_MessageDelete (vlm_message_t *m)
{
    assert (0);
}

vlm_message_t *vlm_MessageNew (const char *a, const char *fmt, ...)
{
    return NULL;
}

vlm_t *__vlm_New (vlc_object_t *obj)
{
     msg_Err (obj, "VLM not compiled-in!");
     return NULL;
}

# endif /* !ENABLE_VLM */
#endif /* !ENABLE_SOUT */