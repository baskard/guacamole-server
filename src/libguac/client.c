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

#include "config.h"

#include "client.h"
#include "client-handlers.h"
#include "error.h"
#include "layer.h"
#include "plugin.h"
#include "pool.h"
#include "protocol.h"
#include "socket.h"
#include "time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

guac_layer __GUAC_DEFAULT_LAYER = {
    .index = 0
};

const guac_layer* GUAC_DEFAULT_LAYER = &__GUAC_DEFAULT_LAYER;

guac_layer* guac_client_alloc_layer(guac_client* client) {

    /* Init new layer */
    guac_layer* allocd_layer = malloc(sizeof(guac_layer));
    allocd_layer->index = guac_pool_next_int(client->__layer_pool)+1;

    return allocd_layer;

}

guac_layer* guac_client_alloc_buffer(guac_client* client) {

    /* Init new layer */
    guac_layer* allocd_layer = malloc(sizeof(guac_layer));
    allocd_layer->index = -guac_pool_next_int(client->__buffer_pool) - 1;

    return allocd_layer;

}

void guac_client_free_buffer(guac_client* client, guac_layer* layer) {

    /* Release index to pool */
    guac_pool_free_int(client->__buffer_pool, -layer->index - 1);

    /* Free layer */
    free(layer);

}

void guac_client_free_layer(guac_client* client, guac_layer* layer) {

    /* Release index to pool */
    guac_pool_free_int(client->__layer_pool, layer->index);

    /* Free layer */
    free(layer);

}

guac_stream* guac_client_alloc_stream(guac_client* client) {

    guac_stream* allocd_stream;
    int stream_index;

    /* Refuse to allocate beyond maximum */
    if (client->__stream_pool->active == GUAC_CLIENT_MAX_STREAMS)
        return NULL;

    /* Allocate stream */
    stream_index = guac_pool_next_int(client->__stream_pool);

    /* Initialize stream */
    allocd_stream = &(client->__output_streams[stream_index]);
    allocd_stream->index = stream_index;
    allocd_stream->data = NULL;

    return allocd_stream;

}

void guac_client_free_stream(guac_client* client, guac_stream* stream) {

    /* Release index to pool */
    guac_pool_free_int(client->__stream_pool, stream->index);

    /* Mark stream as closed */
    stream->index = GUAC_CLIENT_CLOSED_STREAM_INDEX;

}

guac_client* guac_client_alloc() {

    int i;

    /* Allocate new client */
    guac_client* client = malloc(sizeof(guac_client));
    if (client == NULL) {
        guac_error = GUAC_STATUS_NO_MEMORY;
        guac_error_message = "Could not allocate memory for client";
        return NULL;
    }

    /* Init new client */
    memset(client, 0, sizeof(guac_client));

    client->last_received_timestamp =
        client->last_sent_timestamp = guac_timestamp_current();

    client->state = GUAC_CLIENT_RUNNING;

    /* Allocate buffer and layer pools */
    client->__buffer_pool = guac_pool_alloc(GUAC_BUFFER_POOL_INITIAL_SIZE);
    client->__layer_pool = guac_pool_alloc(GUAC_BUFFER_POOL_INITIAL_SIZE);

    /* Allocate stream pool */
    client->__stream_pool = guac_pool_alloc(0);

    /* Initialze streams */
    for (i=0; i<GUAC_CLIENT_MAX_STREAMS; i++) {
        client->__input_streams[i].index = GUAC_CLIENT_CLOSED_STREAM_INDEX;
        client->__output_streams[i].index = GUAC_CLIENT_CLOSED_STREAM_INDEX;
    }

    return client;

}

void guac_client_free(guac_client* client) {

    if (client->free_handler) {

        /* FIXME: Errors currently ignored... */
        client->free_handler(client);

    }

    /* Free layer pools */
    guac_pool_free(client->__buffer_pool);
    guac_pool_free(client->__layer_pool);

    /* Free stream pool */
    guac_pool_free(client->__stream_pool);

    free(client);
}

int guac_client_handle_instruction(guac_client* client, guac_instruction* instruction) {

    /* For each defined instruction */
    __guac_instruction_handler_mapping* current = __guac_instruction_handler_map;
    while (current->opcode != NULL) {

        /* If recognized, call handler */
        if (strcmp(instruction->opcode, current->opcode) == 0)
            return current->handler(client, instruction);

        current++;
    }

    /* If unrecognized, ignore */
    return 0;

}

void vguac_client_log_info(guac_client* client, const char* format,
        va_list ap) {

    /* Call handler if defined */
    if (client->log_info_handler != NULL)
        client->log_info_handler(client, format, ap);

}

void vguac_client_log_error(guac_client* client, const char* format,
        va_list ap) {

    /* Call handler if defined */
    if (client->log_error_handler != NULL)
        client->log_error_handler(client, format, ap);

}

void guac_client_log_info(guac_client* client, const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_client_log_info(client, format, args);

    va_end(args);

}

void guac_client_log_error(guac_client* client, const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_client_log_error(client, format, args);

    va_end(args);

}

void guac_client_stop(guac_client* client) {
    client->state = GUAC_CLIENT_STOPPING;
}

void vguac_client_abort(guac_client* client, guac_protocol_status status,
        const char* format, va_list ap) {

    /* Only relevant if client is running */
    if (client->state == GUAC_CLIENT_RUNNING) {

        /* Log detail of error */
        vguac_client_log_error(client, format, ap);

        /* Send error immediately, limit information given */
        guac_protocol_send_error(client->socket, "Aborted. See logs.", status);
        guac_socket_flush(client->socket);

        /* Stop client */
        guac_client_stop(client);

    }

}

void guac_client_abort(guac_client* client, guac_protocol_status status,
        const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_client_abort(client, status, format, args);

    va_end(args);

}

