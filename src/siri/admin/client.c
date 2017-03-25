/*
 * client.c - Client for expanding a siridb database.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2017, Transceptor Technology
 *
 * changes
 *  - initial version, 24-03-2017
 *
 */
#include <siri/admin/client.h>
#include <siri/siri.h>
#include <logger/logger.h>
#include <siri/net/socket.h>
#include <string.h>
#include <siri/net/protocol.h>
#include <siri/admin/request.h>
#include <stdarg.h>

#define CLIENT_REQUEST_TIMEOUT 15000  // 15 seconds
#define CLIENT_FLAGS_TIMEOUT 1

enum
{
    CLIENT_REQUEST_INIT,
    CLIENT_REQUEST_STATUS
};

static void CLIENT_write_cb(uv_write_t * req, int status);
static void CLIENT_on_connect(uv_connect_t * req, int status);
static void CLIENT_on_data(uv_stream_t * client, sirinet_pkg_t * pkg);
static void CLIENT_request_timeout(uv_timer_t * handle);
static void CLIENT_on_auth_success(siri_admin_client_t * adm_client);
static void CLIENT_err(
        siri_admin_client_t * adm_client,
        const char * fmt,
        ...);
static void CLIENT_send_pkg(
        siri_admin_client_t * adm_client,
        sirinet_pkg_t * pkg);
static void CLIENT_on_error_msg(
        siri_admin_client_t * adm_client,
        sirinet_pkg_t * pkg);

int siri_admin_client_request(
        uint16_t pid,
        uint16_t port,
        qp_obj_t * host,
        qp_obj_t * username,
        qp_obj_t * password,
        qp_obj_t * dbname,
        const char * dbpath,
        uv_stream_t * client,
        char * err_msg)
{
    sirinet_socket_t * ssocket;
    siri_admin_client_t * adm_client;
    struct in_addr sa;
//    struct in6_addr sa6;

    if (siri.socket != NULL)
    {
        sprintf(err_msg, "manage socket already in use");
        return -1;
    }

    siri.socket = sirinet_socket_new(SOCKET_MANAGE, &CLIENT_on_data);
    if (siri.socket == NULL)
    {
        sprintf(err_msg, "memory allocation error");
        return -1;
    }

    uv_tcp_init(siri.loop, siri.socket);

    adm_client = (siri_admin_client_t *) malloc(sizeof(siri_admin_client_t));
    if (adm_client == NULL)
    {
        sirinet_socket_decref(siri.socket);
        sprintf(err_msg, "memory allocation error");
        return -1;
    }
    adm_client->pid = pid;
    adm_client->port = port;
    adm_client->host = strndup(host->via.raw, host->len);
    adm_client->username = strndup(username->via.raw, username->len);
    adm_client->password = strndup(password->via.raw, password->len);
    adm_client->dbname = strndup(dbname->via.raw, dbname->len);
    adm_client->dbpath = strdup(dbpath);
    adm_client->client = client;
    adm_client->request = CLIENT_REQUEST_INIT;
    adm_client->flags = 0;

    ssocket = (sirinet_socket_t *) siri.socket->data;
    ssocket->origin = (void *) adm_client;

    sirinet_socket_incref(adm_client->client);

    if (adm_client->host == NULL ||
        adm_client->username == NULL ||
        adm_client->password == NULL ||
        adm_client->dbname == NULL ||
        adm_client->dbpath == NULL)
    {
        sirinet_socket_decref(siri.socket);
        sprintf(err_msg, "memory allocation error");
        return -1;
    }

    if (inet_pton(AF_INET, adm_client->host, &sa))
    {
        /* IPv4 */
        struct sockaddr_in dest;

        uv_connect_t * req = (uv_connect_t *) malloc(sizeof(uv_connect_t));
        if (req == NULL)
        {
            sirinet_socket_decref(siri.socket);
            sprintf(err_msg, "memory allocation error");
            return -1;
        }
        log_debug(
                "Trying to connect to '%s:%u'...",
                adm_client->host,
                adm_client->port);

        uv_ip4_addr(adm_client->host, adm_client->port, &dest);
        uv_tcp_connect(
                req,
                siri.socket,
                (const struct sockaddr *) &dest,
                CLIENT_on_connect);

        return 0;
    }
    sprintf(err_msg, "invalid ipv4");
    return -1;
}

void siri_admin_client_free(siri_admin_client_t * adm_client)
{
    if (adm_client != NULL)
    {
        sirinet_socket_decref(adm_client->client);
        free(adm_client->host);
        free(adm_client->username);
        free(adm_client->password);
        free(adm_client->dbname);
        free(adm_client->dbpath);
        free(adm_client);
    }
}

static void CLIENT_err(
        siri_admin_client_t * adm_client,
        const char * fmt,
        ...)
{
    char err_msg[SIRI_MAX_SIZE_ERR_MSG];

    va_list args;
    va_start(args, fmt);
    vsnprintf(err_msg, SIRI_MAX_SIZE_ERR_MSG, fmt, args);
    va_end(args);

    sirinet_pkg_t * package = sirinet_pkg_err(
            adm_client->pid,
            strlen(err_msg),
            CPROTO_ERR_ADMIN,
            err_msg);

    if (package != NULL)
    {
        sirinet_pkg_send(adm_client->client, package);
    }

    log_error(err_msg);

    siri_admin_request_rollback(adm_client->dbpath);

    sirinet_socket_decref(siri.socket);

    uv_close((uv_handle_t *) &siri.timer, NULL);
}

static void CLIENT_send_pkg(
        siri_admin_client_t * adm_client,
        sirinet_pkg_t * pkg)
{
    uv_write_t * req = (uv_write_t *) malloc(sizeof(uv_write_t));
    if (req == NULL)
    {
        free(pkg);
        CLIENT_err(adm_client, "memory allocation error");
    }
    req->data = adm_client;

    adm_client->pkg = pkg;

    /* set the correct check bit */
    pkg->checkbit = pkg->tp ^ 255;

    uv_timer_start(
            &siri.timer,
            CLIENT_request_timeout,
            CLIENT_REQUEST_TIMEOUT,
            0);

    siri.timer.data = adm_client;

    uv_buf_t wrbuf = uv_buf_init(
            (char *) pkg,
            sizeof(sirinet_pkg_t) + pkg->len);

    uv_write(
            req,
            (uv_stream_t *) siri.socket,
            &wrbuf,
            1,
            CLIENT_write_cb);
}

/*
 * Write call-back.
 */
static void CLIENT_write_cb(uv_write_t * req, int status)
{
    siri_admin_client_t * adm_client = (siri_admin_client_t *)  req->data;

    if (status)
    {
        uv_timer_stop(&siri.timer);

        CLIENT_err(adm_client, "socket write error: %s", uv_strerror(status));
    }

    free(adm_client->pkg);
    free(req);
}

/*
 * This function can raise a SIGNAL.
 */
static void CLIENT_on_connect(uv_connect_t * req, int status)
{
    sirinet_socket_t * ssocket = req->handle->data;
    siri_admin_client_t * adm_client = (siri_admin_client_t *) ssocket->origin;

    uv_timer_init(siri.loop, &siri.timer);

    if (status == 0)
    {
        log_debug(
                "Connected to SiriDB server: '%s:%u', "
                "sending authentication request",
                adm_client->host, adm_client->port);

        uv_read_start(
                req->handle,
                sirinet_socket_alloc_buffer,
                sirinet_socket_on_data);

        sirinet_pkg_t * pkg;
        qp_packer_t * packer = sirinet_packer_new(512);

        if (packer == NULL)
        {
            CLIENT_err(adm_client, "memory allocation error");
        }
        else
        {
            if (qp_add_type(packer, QP_ARRAY3) ||
                qp_add_string(packer, adm_client->username) ||
                qp_add_string(packer, adm_client->password) ||
                qp_add_string(packer, adm_client->dbname))
            {
                qp_packer_free(packer);
            }
            else
            {
                pkg = sirinet_packer2pkg(packer, 0, CPROTO_REQ_AUTH);
                CLIENT_send_pkg(adm_client, pkg);
            }

        }
    }
    else
    {
        CLIENT_err(
                adm_client,
                "connecting to server '%s:%u' failed with error: %s",
                adm_client->host,
                adm_client->port,
                uv_strerror(status));
    }
    free(req);
}

/*
 * on-data call-back function.
 *In case the promise is found, promise->cb() will be called.
 */
static void CLIENT_on_data(uv_stream_t * client, sirinet_pkg_t * pkg)
{
    sirinet_socket_t * ssocket = client->data;
    siri_admin_client_t * adm_client = (siri_admin_client_t *) ssocket->origin;
    log_debug(
            "Client response received (pid: %" PRIu16
            ", len: %" PRIu32 ", tp: %s)",
            pkg->pid,
            pkg->len,
            sirinet_cproto_server_str(pkg->tp));

    if (adm_client->flags & CLIENT_FLAGS_TIMEOUT)
    {
        log_error("Client response received which was timed-out earlier");
    }
    else
    {
        uv_timer_stop(&siri.timer);

        switch ((cproto_server_t) pkg->tp)
        {
        case CPROTO_RES_AUTH_SUCCESS:
            CLIENT_on_auth_success(adm_client);
            break;
        case CPROTO_RES_QUERY:
            switch (adm_client->request)
            {
            case CLIENT_REQUEST_STATUS:
                CLIENT_on_request_status(adm_client, pkg);
                break;
            default:
                CLIENT_err(adm_client, "unexpected query response");
            }
            break;
        case CPROTO_ERR_AUTH_CREDENTIALS:
            CLIENT_err(
                    adm_client,
                    "invalid credentials for database '%s' on server '%s:%u'",
                    adm_client->dbname,
                    adm_client->host,
                    adm_client->port);
            break;
        case CPROTO_ERR_AUTH_UNKNOWN_DB:
            CLIENT_err(
                    adm_client,
                    "database '%s' does not exist on server '%s:%u'",
                    adm_client->dbname,
                    adm_client->host,
                    adm_client->port);
            break;
        case CPROTO_ERR_MSG:
        case CPROTO_ERR_QUERY:
        case CPROTO_ERR_INSERT:
        case CPROTO_ERR_SERVER:
        case CPROTO_ERR_POOL:
        case CPROTO_ERR_USER_ACCESS:
            CLIENT_on_error_msg(adm_client, pkg);
            break;
        default:
            CLIENT_err(
                    adm_client,
                    "unexpected response (%u) received from server '%s:%u'",
                    pkg->tp,
                    adm_client->host,
                    adm_client->port);
        }
    }
}
static void CLIENT_on_request_status(
        siri_admin_client_t * adm_client,
        sirinet_pkg_t * pkg)
{
    qp_unpacker_t unpacker;
    qp_unpacker_init(&unpacker, pkg->data, pkg->len);
    qp_obj_t qp_val;
    qp_obj_t qp_name;
    qp_obj_t qp_status;
    int columns_found = 0;

    if (qp_is_map(qp_next(&unpacker, NULL)))
    {
        CLIENT_err(adm_client, "invalid server status response");
        return;
    }

    qp_next(&unpacker, &qp_val);

    while (qp_val.tp == QP_RAW)
    {
        if (    strncmp(qp_val.via.raw, "columns", qp_val.len) == 0 &&
                qp_is_array(qp_next(&unpacker, NULL)) &&
                qp_next(&unpacker, &qp_val) == QP_RAW &&
                strncmp(qp_val.via.raw, "name", qp_val.len) == 0 &&
                qp_next(&unpacker, &qp_val) == QP_RAW &&
                strncmp(qp_val.via.raw, "status", qp_val.len) == 0)
        {
            if (qp_next(&unpacker, &qp_val) == QP_ARRAY_CLOSE)
            {
                qp_next(&unpacker, &qp_val);
            }
            columns_found = 1;
            continue;
        }
        if (    strncmp(qp_val.via.raw, "data", qp_val.len) == 0 &&
                qp_is_array(qp_next(&unpacker, NULL)))
        {
            while (qp_is_array(qp_next(&unpacker, NULL)))
            {
                if (qp_next(&unpacker, &qp_name) == QP_RAW &&
                    qp_next(&unpacker, &qp_status) == QP_RAW)
                {
                    if (strncmp(qp_val.via.raw, "running", qp_val.len) != 0)
                    {
                        CLIENT_err(adm_client, "")
                    }
                }
                else
                {
                    CLIENT_err(adm_client, "invalid server status response");
                    return;
                }
            }
            continue;
        }
        return CPROTO_ERR_ADMIN_INVALID_REQUEST;
    }
}

static void CLIENT_on_error_msg(
        siri_admin_client_t * adm_client,
        sirinet_pkg_t * pkg)
{
    qp_unpacker_t unpacker;
    qp_unpacker_init(&unpacker, pkg->data, pkg->len);
    qp_obj_t qp_err;

    if (qp_is_map(qp_next(&unpacker, NULL)) &&
        qp_next(&unpacker, NULL) == QP_RAW &&
        qp_next(&unpacker, &qp_err) == QP_RAW)
    {
        CLIENT_err(
                adm_client,
                "error on server '%s:%u': %.*s",
                adm_client->host,
                adm_client->port,
                (int) qp_err.len,
                qp_err.via.raw);
    }
    else
    {
        CLIENT_err(
                adm_client,
                "unexpected error on server '%s:%u'",
                adm_client->host,
                adm_client->port);
    }
}

static void CLIENT_on_auth_success(siri_admin_client_t * adm_client)
{
    qp_packer_t * packer = sirinet_packer_new(512);
    sirinet_pkg_t * pkg;

    if (packer == NULL)
    {
        CLIENT_err(adm_client, "memory allocation error");
    }
    else
    {
        adm_client->request = CLIENT_REQUEST_STATUS;

        /* no need to check since this will always fit */
        qp_add_type(packer, QP_ARRAY1);
        qp_add_string(packer, "list servers status");
        pkg = sirinet_packer2pkg(packer, 0, CPROTO_REQ_QUERY);
        CLIENT_send_pkg(adm_client, pkg);
    }
}

/*
 * Timeout received.
 */
static void CLIENT_request_timeout(uv_timer_t * handle)
{
    siri_admin_client_t * adm_client = (siri_admin_client_t *) handle->data;

    adm_client->flags |= CLIENT_FLAGS_TIMEOUT;

    CLIENT_err(adm_client, "request timeout");
}