/*
 * Spdylay - SPDY Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "spdylay_session.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include <arpa/inet.h>

#include "spdylay_stream.h"
#include "spdylay_helper.h"

int spdylay_outbound_item_compar(const void *lhsx, const void *rhsx)
{
  const spdylay_outbound_item *lhs, *rhs;
  lhs = (const spdylay_outbound_item*)lhsx;
  rhs = (const spdylay_outbound_item*)rhsx;
  return lhs->pri-rhs->pri;
}

int spdylay_session_client_init(spdylay_session **session_ptr,
                                const spdylay_session_callbacks *callbacks,
                                void *user_data)
{
  int r;
  *session_ptr = malloc(sizeof(spdylay_session));
  if(*session_ptr == NULL) {
    return SPDYLAY_ERR_NOMEM;
  }
  memset(*session_ptr, 0, sizeof(spdylay_session));
  (*session_ptr)->next_stream_id = 1;
  (*session_ptr)->last_accepted_stream_id = 0;

  r = spdylay_zlib_deflate_hd_init(&(*session_ptr)->hd_deflater);
  if(r != 0) {
    free(*session_ptr);
    return r;
  }
  r = spdylay_zlib_inflate_hd_init(&(*session_ptr)->hd_inflater);
  if(r != 0) {
    spdylay_zlib_deflate_free(&(*session_ptr)->hd_deflater);
    free(*session_ptr);
    return r;
  }
  r = spdylay_map_init(&(*session_ptr)->streams);
  if(r != 0) {
    spdylay_zlib_inflate_free(&(*session_ptr)->hd_inflater);
    spdylay_zlib_deflate_free(&(*session_ptr)->hd_deflater);
    free(*session_ptr);
    return r;
  }
  r = spdylay_pq_init(&(*session_ptr)->ob_pq, spdylay_outbound_item_compar);
  if(r != 0) {
    spdylay_map_free(&(*session_ptr)->streams);
    spdylay_zlib_inflate_free(&(*session_ptr)->hd_inflater);
    spdylay_zlib_deflate_free(&(*session_ptr)->hd_deflater);
    free(*session_ptr);
    return r;
  }
  (*session_ptr)->callbacks = *callbacks;
  (*session_ptr)->user_data = user_data;

  (*session_ptr)->ibuf.mark = (*session_ptr)->ibuf.buf;
  (*session_ptr)->ibuf.limit = (*session_ptr)->ibuf.buf;
  
  (*session_ptr)->iframe.state = SPDYLAY_RECV_HEAD;
  return 0;
}

static void spdylay_free_streams(key_type key, void *val)
{
  spdylay_stream_free((spdylay_stream*)val);
  free(val);
}

static void spdylay_outbound_item_free(spdylay_outbound_item *item)
{
  if(item == NULL) {
    return;
  }
  switch(item->frame_type) {
  case SPDYLAY_SYN_STREAM:
    spdylay_frame_syn_stream_free(&item->frame->syn_stream);
    break;
  }
  free(item->frame);
}

void spdylay_session_free(spdylay_session *session)
{
  spdylay_map_each(&session->streams, spdylay_free_streams);
  spdylay_map_free(&session->streams);
  while(!spdylay_pq_empty(&session->ob_pq)) {
    spdylay_outbound_item *item = (spdylay_outbound_item*)
      spdylay_pq_top(&session->ob_pq);
    spdylay_outbound_item_free(item);
    free(item);
    spdylay_pq_pop(&session->ob_pq);
  }
  spdylay_pq_free(&session->ob_pq);
  spdylay_zlib_deflate_free(&session->hd_deflater);
  spdylay_zlib_inflate_free(&session->hd_inflater);
  free(session->iframe.buf);
  free(session);
}

int spdylay_session_add_frame(spdylay_session *session,
                              spdylay_frame_type frame_type,
                              spdylay_frame *frame)
{
  int r;
  spdylay_outbound_item *item;
  item = malloc(sizeof(spdylay_outbound_item));
  if(item == NULL) {
    return SPDYLAY_ERR_NOMEM;
  }
  item->frame_type = frame_type;
  item->frame = frame;
  /* TODO Add pri field to SYN_REPLY, DATA frame which copies
     corresponding SYN_STREAM pri.  PING frame always pri = 0
     (highest) */
  switch(frame_type) {
  case SPDYLAY_SYN_STREAM:
    item->pri = 4-frame->syn_stream.pri;
    break;
  default:
    item->pri = 4;
  };
  r = spdylay_pq_push(&session->ob_pq, item);
  if(r != 0) {
    free(item);
    return r;
  }
  return 0;
}

int spdylay_session_open_stream(spdylay_session *session, int32_t stream_id)
{
  int r;
  spdylay_stream *stream = malloc(sizeof(spdylay_stream));
  if(stream == NULL) {
    return SPDYLAY_ERR_NOMEM;
  }
  spdylay_stream_init(stream, stream_id);
  r = spdylay_map_insert(&session->streams, stream_id, stream);
  if(r != 0) {
    free(stream);
  }
  return r;
}

ssize_t spdylay_session_prep_frame(spdylay_session *session,
                                   spdylay_outbound_item *item,
                                   uint8_t **framebuf_ptr)
{
  uint8_t *framebuf;
  ssize_t framebuflen;
  int r;
  switch(item->frame_type) {
  case SPDYLAY_SYN_STREAM: {
    item->frame->syn_stream.stream_id = session->next_stream_id;
    session->next_stream_id += 2;
    framebuflen = spdylay_frame_pack_syn_stream(&framebuf,
                                                &item->frame->syn_stream,
                                                &session->hd_deflater);
    if(framebuflen < 0) {
      return framebuflen;
    }
    printf("packed %d bytes\n", framebuflen);
    r = spdylay_session_open_stream(session, item->frame->syn_stream.stream_id);
    if(r != 0) {
      free(framebuf);
      return r;
    }
    *framebuf_ptr = framebuf;
    break;
  }
  default:
    framebuflen = SPDYLAY_ERR_INVALID_ARGUMENT;
  }
  return framebuflen;
}

static void spdylay_active_outbound_item_reset
(spdylay_active_outbound_item *aob)
{
  spdylay_outbound_item_free(aob->item);
  free(aob->item);
  free(aob->framebuf);
  memset(aob, 0, sizeof(spdylay_active_outbound_item));
}

int spdylay_session_send(spdylay_session *session)
{
  printf("session_send\n");
  while(session->aob.item || !spdylay_pq_empty(&session->ob_pq)) {
    const uint8_t *data;
    size_t datalen;
    ssize_t sentlen;
    if(session->aob.item == NULL) {
      spdylay_outbound_item *item = spdylay_pq_top(&session->ob_pq);
      uint8_t *framebuf;
      ssize_t framebuflen;
      spdylay_pq_pop(&session->ob_pq);
      /* TODO Get or validate stream id here */
      framebuflen = spdylay_session_prep_frame(session, item, &framebuf);
      if(framebuflen < 0) {
        /* TODO Call error callback? */
        spdylay_outbound_item_free(item);
        free(item);
        continue;;
      }
      session->aob.item = item;
      session->aob.framebuf = framebuf;
      session->aob.framebuflen = framebuflen;
    }
    data = session->aob.framebuf + session->aob.framebufoff;
    datalen = session->aob.framebuflen - session->aob.framebufoff;
    sentlen = session->callbacks.send_callback(data, datalen, 0,
                                               session->user_data);
    if(sentlen < 0) {
      if(sentlen == SPDYLAY_ERR_WOULDBLOCK) {
        return 0;
      } else {
        return sentlen;
      }
    } else {
      printf("sent %d bytes\n", sentlen);
      session->aob.framebufoff += sentlen;
      if(session->aob.framebufoff == session->aob.framebuflen) {
        /* Frame has completely sent */
        spdylay_active_outbound_item_reset(&session->aob);
        /* TODO If frame is data frame, we need to sent all chunk of
           data.*/
      } else {
        /* partial write */
        break;
      }
    }
  }
  return 0;
}

static void spdylay_inbound_buffer_shift(spdylay_inbound_buffer *ibuf)
{
  ptrdiff_t len = ibuf->limit-ibuf->mark;
  memmove(ibuf->buf, ibuf->mark, len);
  ibuf->limit = ibuf->buf+len;
  ibuf->mark = ibuf->buf;
}

static ssize_t spdylay_recv(spdylay_session *session)
{
  ssize_t r;
  size_t recv_max;
  if(session->ibuf.mark != session->ibuf.buf) {
    spdylay_inbound_buffer_shift(&session->ibuf);
  }
  recv_max = session->ibuf.buf+sizeof(session->ibuf.buf)-session->ibuf.limit;
  r = session->callbacks.recv_callback
    (session->ibuf.limit, recv_max, 0, session->user_data);
  if(r > 0) {
    if(r > recv_max) {
      return SPDYLAY_ERR_CALLBACK_FAILURE;
    } else {
      session->ibuf.limit += r;
    }
  } else if(r < 0) {
    if(r != SPDYLAY_ERR_WOULDBLOCK) {
      r = SPDYLAY_ERR_CALLBACK_FAILURE;
    }
  }
  return r;
}

static size_t spdylay_inbound_buffer_avail(spdylay_inbound_buffer *ibuf)
{
  return ibuf->limit-ibuf->mark;
}

static void spdylay_inbound_frame_reset(spdylay_inbound_frame *iframe)
{
  iframe->state = SPDYLAY_RECV_HEAD;
  free(iframe->buf);
  iframe->buf = NULL;
  iframe->len = iframe->off = 0;
  iframe->ign = 0;
}

static void spdylay_debug_print_nv(char **nv)
{
  int i;
  for(i = 0; nv[i]; i += 2) {
    printf("%s: %s\n", nv[i], nv[i+1]);
  }
}

int spdylay_session_process_ctrl_frame(spdylay_session *session)
{
  int r;
  uint16_t type;
  memcpy(&type, &session->iframe.headbuf[2], sizeof(uint16_t));
  type = ntohs(type);
  switch(type) {
  case SPDYLAY_SYN_STREAM: {
    spdylay_syn_stream frame;
    printf("SYN_STREAM\n");
    r = spdylay_frame_unpack_syn_stream(&frame, session->iframe.headbuf,
                                        sizeof(session->iframe.headbuf),
                                        session->iframe.buf,
                                        session->iframe.len,
                                        &session->hd_inflater);
    if(r == 0) {
      spdylay_debug_print_nv(frame.nv);
      spdylay_frame_syn_stream_free(&frame);
    }
    break;
  }
  case SPDYLAY_SYN_REPLY: {
    spdylay_syn_reply frame;
    printf("SYN_REPLY\n");
    r = spdylay_frame_unpack_syn_reply(&frame, session->iframe.headbuf,
                                       sizeof(session->iframe.headbuf),
                                       session->iframe.buf,
                                       session->iframe.len,
                                       &session->hd_inflater);
    if(r == 0) {
      spdylay_debug_print_nv(frame.nv);
      spdylay_frame_syn_reply_free(&frame);
    }
    break;
  }
  default:
    /* ignore */
    printf("Received control frame type %x\n", type);
  }
  return 0;
}

int spdylay_session_process_data_frame(spdylay_session *session)
{
  printf("Received data frame, stream_id %d, %zu bytes, fin=%d\n",
         spdylay_get_uint32(session->iframe.headbuf),
         session->iframe.len,
         session->iframe.headbuf[4] & SPDYLAY_FLAG_FIN);
  return 0;
}

int spdylay_session_recv(spdylay_session *session)
{
  printf("session_recv\n");
  while(1) {
    ssize_t r;
    if(session->iframe.state == SPDYLAY_RECV_HEAD) {
      uint32_t payloadlen;
      if(spdylay_inbound_buffer_avail(&session->ibuf) < SPDYLAY_HEAD_LEN) {
        r = spdylay_recv(session);
        printf("r=%d\n", r);
        /* TODO handle EOF */
        if(r < 0) {
          if(r == SPDYLAY_ERR_WOULDBLOCK) {
            return 0;
          } else {
            return r;
          }
        }
        printf("Recved %d bytes\n", r);
        if(spdylay_inbound_buffer_avail(&session->ibuf) < SPDYLAY_HEAD_LEN) {
          return 0;
        }
      }
      session->iframe.state = SPDYLAY_RECV_PAYLOAD;
      payloadlen = spdylay_get_uint32(&session->ibuf.mark[4]) & 0xffffff;
      memcpy(session->iframe.headbuf, session->ibuf.mark, SPDYLAY_HEAD_LEN);
      session->ibuf.mark += SPDYLAY_HEAD_LEN;
      if(spdylay_frame_is_ctrl_frame(session->iframe.headbuf[0])) {
        /* control frame */
        session->iframe.len = payloadlen;
        session->iframe.buf = malloc(session->iframe.len);
        if(session->iframe.buf == NULL) {
          return SPDYLAY_ERR_NOMEM;
        }
        session->iframe.off = 0;
      } else {
        int32_t stream_id;
        /* data frame */
        /* For data frame, We dont' buffer data. Instead, just pass
           received data to callback function. */
        stream_id = spdylay_get_uint32(session->iframe.headbuf) & 0x7fffffff;
        /* TODO validate stream id here */
        session->iframe.len = payloadlen;
        session->iframe.off = 0;
      }
    }
    if(session->iframe.state == SPDYLAY_RECV_PAYLOAD) {
      size_t rempayloadlen = session->iframe.len - session->iframe.off;
      size_t bufavail, readlen;
      if(spdylay_inbound_buffer_avail(&session->ibuf) == 0 &&
         rempayloadlen > 0) {
        r = spdylay_recv(session);
        if(r <= 0) {
          if(r == SPDYLAY_ERR_WOULDBLOCK) {
            return 0;
          } else {
            return r;
          }
        }
      }
      bufavail = spdylay_inbound_buffer_avail(&session->ibuf);
      readlen =  bufavail < rempayloadlen ? bufavail : rempayloadlen;
      if(session->iframe.buf != NULL) {
        memcpy(session->iframe.buf, session->ibuf.mark, readlen);
      }
      session->iframe.off += readlen;
      session->ibuf.mark += readlen;
      if(session->iframe.len == session->iframe.off) {
        if(spdylay_frame_is_ctrl_frame(session->iframe.headbuf[0])) {
          spdylay_session_process_ctrl_frame(session);
        } else {
          spdylay_session_process_data_frame(session);
        }
        spdylay_inbound_frame_reset(&session->iframe);
      }
    }
  }
  return 0;
}

int spdylay_session_want_read(spdylay_session *session)
{
  return 1;
}

int spdylay_session_want_write(spdylay_session *session)
{
  return session->aob.item != NULL || !spdylay_pq_empty(&session->ob_pq);
}

int spdylay_req_submit(spdylay_session *session, const char *path)
{
  int r;
  spdylay_frame *frame;
  char **nv;
  frame = malloc(sizeof(spdylay_frame));
  nv = malloc(9*sizeof(char*));
  nv[0] = strdup("method");
  nv[1] = strdup("GET");
  nv[2] = strdup("scheme");
  nv[3] = strdup("https");
  nv[4] = strdup("url");
  nv[5] = strdup(path);
  nv[6] = strdup("version");
  nv[7] = strdup("HTTP/1.1");
  nv[8] = NULL;
  spdylay_frame_syn_stream_init(&frame->syn_stream,
                                SPDYLAY_FLAG_FIN, 0, 0, 0, nv);
  r = spdylay_session_add_frame(session, SPDYLAY_SYN_STREAM, frame);
  assert(r == 0);
}