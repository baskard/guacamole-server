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
#include "common.h"
#include "guac_handlers.h"
#include "sftp.h"
#include "ssh_key.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <libssh2.h>
#include <guacamole/client.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>

#ifdef ENABLE_SSH_AGENT
#include "ssh_agent.h"
#endif

/**
 * Reads a single line from STDIN.
 */
static char* prompt(guac_client* client, const char* title, char* str, int size, bool echo) {

    ssh_guac_client_data* client_data = (ssh_guac_client_data*) client->data;

    int pos;
    char in_byte;

    /* Get STDIN and STDOUT */
    int stdin_fd  = client_data->term->stdin_pipe_fd[0];
    int stdout_fd = client_data->term->stdout_pipe_fd[1];

    /* Print title */
    guac_terminal_write_all(stdout_fd, title, strlen(title));

    /* Make room for null terminator */
    size--;

    /* Read bytes until newline */
    pos = 0;
    while (pos < size && read(stdin_fd, &in_byte, 1) == 1) {

        /* Backspace */
        if (in_byte == 0x7F) {

            if (pos > 0) {
                guac_terminal_write_all(stdout_fd, "\b \b", 3);
                pos--;
            }
        }

        /* CR (end of input */
        else if (in_byte == 0x0D) {
            guac_terminal_write_all(stdout_fd, "\r\n", 2);
            break;
        }

        else {

            /* Store character, update buffers */
            str[pos++] = in_byte;

            /* Print character if echoing */
            if (echo)
                guac_terminal_write_all(stdout_fd, &in_byte, 1);
            else
                guac_terminal_write_all(stdout_fd, "*", 1);

        }

    }

    str[pos] = 0;
    return str;

}

void* ssh_input_thread(void* data) {

    guac_client* client = (guac_client*) data;
    ssh_guac_client_data* client_data = (ssh_guac_client_data*) client->data;

    char buffer[8192];
    int bytes_read;

    int stdin_fd = client_data->term->stdin_pipe_fd[0];

    /* Write all data read */
    while ((bytes_read = read(stdin_fd, buffer, sizeof(buffer))) > 0)
        libssh2_channel_write(client_data->term_channel, buffer, bytes_read);

    return NULL;

}

static int __sign_callback(LIBSSH2_SESSION* session,
        unsigned char** sig, size_t* sig_len,
        const unsigned char* data, size_t data_len, void **abstract) {

    ssh_key* key = (ssh_key*) abstract;

    /* Allocate space for signature */
    *sig = malloc(4096);

    /* Sign with key */
    *sig_len = ssh_key_sign(key, (const char*) data, data_len, *sig);
    if (*sig_len < 0)
        return 1;

    return 0;

}

static LIBSSH2_SESSION* __guac_ssh_create_session(guac_client* client,
        int* socket_fd) {

    int retval;

    int fd;
    struct addrinfo* addresses;
    struct addrinfo* current_address;

    char connected_address[1024];
    char connected_port[64];

    ssh_guac_client_data* client_data = (ssh_guac_client_data*) client->data;

    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP
    };

    /* Get socket */
    fd = socket(AF_INET, SOCK_STREAM, 0);

    /* Get addresses connection */
    if ((retval = getaddrinfo(client_data->hostname, client_data->port,
                    &hints, &addresses))) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR, "Error parsing given address or port: %s",
                gai_strerror(retval));
        return NULL;

    }

    /* Attempt connection to each address until success */
    current_address = addresses;
    while (current_address != NULL) {

        int retval;

        /* Resolve hostname */
        if ((retval = getnameinfo(current_address->ai_addr,
                current_address->ai_addrlen,
                connected_address, sizeof(connected_address),
                connected_port, sizeof(connected_port),
                NI_NUMERICHOST | NI_NUMERICSERV)))
            guac_client_log_info(client, "Unable to resolve host: %s", gai_strerror(retval));

        /* Connect */
        if (connect(fd, current_address->ai_addr,
                        current_address->ai_addrlen) == 0) {

            guac_client_log_info(client, "Successfully connected to "
                    "host %s, port %s", connected_address, connected_port);

            /* Done if successful connect */
            break;

        }

        /* Otherwise log information regarding bind failure */
        else
            guac_client_log_info(client, "Unable to connect to "
                    "host %s, port %s: %s",
                    connected_address, connected_port, strerror(errno));

        current_address = current_address->ai_next;

    }

    /* If unable to connect to anything, fail */
    if (current_address == NULL) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR, "Unable to connect to any addresses.");
        return NULL;
    }

    /* Free addrinfo */
    freeaddrinfo(addresses);

    /* Open SSH session */
    LIBSSH2_SESSION* session = libssh2_session_init_ex(NULL, NULL,
            NULL, client);
    if (session == NULL) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR, "Session allocation failed.");
        return NULL;
    }

    /* Perform handshake */
    if (libssh2_session_handshake(session, fd)) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR, "SSH handshake failed.");
        return NULL;
    }

    /* Save file descriptor */
    if (socket_fd != NULL)
        *socket_fd = fd;

    /* Authenticate with key if available */
    if (client_data->key != NULL) {
        if (!libssh2_userauth_publickey(session, client_data->username,
                    (unsigned char*) client_data->key->public_key,
                    client_data->key->public_key_length,
                    __sign_callback, (void**) client_data->key))
            return session;
        else {
            char* error_message;
            libssh2_session_last_error(session, &error_message, NULL, 0);
            guac_client_abort(client, GUAC_PROTOCOL_STATUS_CLIENT_UNAUTHORIZED,
                    "Public key authentication failed: %s", error_message);
            return NULL;
        }
    }

    /* Authenticate with password */
    if (!libssh2_userauth_password(session, client_data->username,
                client_data->password))
        return session;

    else {
        char* error_message;
        libssh2_session_last_error(session, &error_message, NULL, 0);
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_CLIENT_UNAUTHORIZED,
                "Password authentication failed: %s", error_message);
        return NULL;
    }

}

void* ssh_client_thread(void* data) {

    guac_client* client = (guac_client*) data;
    ssh_guac_client_data* client_data = (ssh_guac_client_data*) client->data;

    char name[1024];

    guac_socket* socket = client->socket;
    char buffer[8192];
    int bytes_read = -1234;

    int socket_fd;
    int stdout_fd = client_data->term->stdout_pipe_fd[1];

    pthread_t input_thread;

    libssh2_init(0);

    /* Get username */
    if (client_data->username[0] == 0)
        prompt(client, "Login as: ", client_data->username, sizeof(client_data->username), true);

    /* Send new name */
    snprintf(name, sizeof(name)-1, "%s@%s", client_data->username, client_data->hostname);
    guac_protocol_send_name(socket, name);

    /* If key specified, import */
    if (client_data->key_base64[0] != 0) {

        /* Attempt to read key without passphrase */
        client_data->key = ssh_key_alloc(client_data->key_base64,
                strlen(client_data->key_base64), "");

        /* On failure, attempt with passphrase */
        if (client_data->key == NULL) {

            /* Prompt for passphrase if missing */
            if (client_data->key_passphrase[0] == 0)
                prompt(client, "Key passphrase: ", client_data->key_passphrase,
                        sizeof(client_data->key_passphrase), false);

            /* Import key with passphrase */
            client_data->key = ssh_key_alloc(client_data->key_base64,
                    strlen(client_data->key_base64),
                    client_data->key_passphrase);

            /* If still failing, give up */
            if (client_data->key == NULL) {
                guac_client_log_error(client, "Auth key import failed.");
                return NULL;
            }

        } /* end decrypt key with passphrase */

        /* Success */
        guac_client_log_info(client, "Auth key successfully imported.");

    } /* end if key given */

    /* Otherwise, get password if not provided */
    else if (client_data->password[0] == 0)
        prompt(client, "Password: ", client_data->password, sizeof(client_data->password), false);

    /* Clear screen */
    guac_terminal_write_all(stdout_fd, "\x1B[H\x1B[J", 6);

    /* Open SSH session */
    client_data->session = __guac_ssh_create_session(client, &socket_fd);
    if (client_data->session == NULL) {
        /* Already aborted within __guac_ssh_create_session() */
        return NULL;
    }

    /* Open channel for terminal */
    client_data->term_channel = libssh2_channel_open_session(client_data->session);
    if (client_data->term_channel == NULL) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR, "Unable to open terminal channel.");
        return NULL;
    }

#ifdef ENABLE_SSH_AGENT
    /* Start SSH agent forwarding, if enabled */
    if (client_data->enable_agent) {
        libssh2_session_callback_set(client_data->session,
                LIBSSH2_CALLBACK_AUTH_AGENT, (void*) ssh_auth_agent_callback);

        /* Request agent forwarding */
        if (libssh2_channel_request_auth_agent(client_data->term_channel))
            guac_client_log_error(client, "Agent forwarding request failed");
        else
            guac_client_log_info(client, "Agent forwarding enabled.");
    }

    client_data->auth_agent = NULL;
#endif

    /* Start SFTP session as well, if enabled */
    if (client_data->enable_sftp) {

        /* Create SSH session specific for SFTP */
        guac_client_log_info(client, "Reconnecting for SFTP...");
        client_data->sftp_ssh_session = __guac_ssh_create_session(client, NULL);
        if (client_data->sftp_ssh_session == NULL) {
            /* Already aborted within __guac_ssh_create_session() */
            return NULL;
        }

        /* Request SFTP */
        client_data->sftp_session = libssh2_sftp_init(client_data->sftp_ssh_session);
        if (client_data->sftp_session == NULL) {
            guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR, "Unable to start SFTP session.");
            return NULL;
        }

        /* Set file handlers */
        client->ack_handler  = guac_sftp_ack_handler;
        client->file_handler = guac_sftp_file_handler;
        client->blob_handler = guac_sftp_blob_handler;
        client->end_handler  = guac_sftp_end_handler;

        guac_client_log_info(client, "SFTP session initialized");

    }

    /* Request PTY */
    if (libssh2_channel_request_pty_ex(client_data->term_channel, "linux", sizeof("linux")-1, NULL, 0,
            client_data->term->term_width, client_data->term->term_height, 0, 0)) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR, "Unable to allocate PTY.");
        return NULL;
    }

    /* Request shell */
    if (libssh2_channel_shell(client_data->term_channel)) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR, "Unable to associate shell with PTY.");
        return NULL;
    }

    /* Logged in */
    guac_client_log_info(client, "SSH connection successful.");

    /* Start input thread */
    if (pthread_create(&(input_thread), NULL, ssh_input_thread, (void*) client)) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR, "Unable to start input thread");
        return NULL;
    }

    /* Set non-blocking */
    libssh2_session_set_blocking(client_data->session, 0);

    /* While data available, write to terminal */
    bytes_read = 0;
    while (!libssh2_channel_eof(client_data->term_channel)) {

        /* Track total amount of data read */
        int total_read = 0;

        /* Read terminal data */
        bytes_read = libssh2_channel_read(client_data->term_channel,
                buffer, sizeof(buffer));

        /* Attempt to write data received. Exit on failure. */
        if (bytes_read > 0) {
            int written = guac_terminal_write_all(stdout_fd, buffer, bytes_read);
            if (written < 0)
                break;

            total_read += bytes_read;
        }

        else if (bytes_read < 0 && bytes_read != LIBSSH2_ERROR_EAGAIN)
            break;

#ifdef ENABLE_SSH_AGENT
        /* If agent open, handle any agent packets */
        if (client_data->auth_agent != NULL) {
            bytes_read = ssh_auth_agent_read(client_data->auth_agent);
            if (bytes_read > 0)
                total_read += bytes_read;
            else if (bytes_read < 0 && bytes_read != LIBSSH2_ERROR_EAGAIN)
                client_data->auth_agent = NULL;
        }
#endif

        /* Wait for more data if reads turn up empty */
        if (total_read == 0) {
            fd_set fds;
            struct timeval timeout;

            FD_ZERO(&fds);
            FD_SET(socket_fd, &fds);

            /* Wait for one second */
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            if (select(socket_fd+1, &fds, NULL, NULL, &timeout) < 0)
                break;
        }

    }

    /* Kill client and Wait for input thread to die */
    guac_client_stop(client);
    pthread_join(input_thread, NULL);

    guac_client_log_info(client, "SSH connection ended.");
    return NULL;

}

