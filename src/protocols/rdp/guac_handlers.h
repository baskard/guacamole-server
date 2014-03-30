/*
 * Copyright (C) 2013 Glyptodon LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#ifndef _GUAC_RDP_GUAC_HANDLERS_H
#define _GUAC_RDP_GUAC_HANDLERS_H

#include "config.h"

#include <guacamole/client.h>
#include <guacamole/stream.h>

int rdp_guac_client_free_handler(guac_client* client);
int rdp_guac_client_handle_messages(guac_client* client);
int rdp_guac_client_mouse_handler(guac_client* client, int x, int y, int mask);
int rdp_guac_client_key_handler(guac_client* client, int keysym, int pressed);
int rdp_guac_client_clipboard_handler(guac_client* client, char* data);

int rdp_guac_client_file_handler(guac_client* client, guac_stream* stream,
        char* mimetype, char* filename);

int rdp_guac_client_pipe_handler(guac_client* client, guac_stream* stream,
        char* mimetype, char* name);

int rdp_guac_client_blob_handler(guac_client* client, guac_stream* stream,
        void* data, int length);

int rdp_guac_client_end_handler(guac_client* client, guac_stream* stream);

int rdp_guac_client_ack_handler(guac_client* client, guac_stream* stream,
        char* message, guac_protocol_status status);

#endif

