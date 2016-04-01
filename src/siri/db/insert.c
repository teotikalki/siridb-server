/*
 * insert.c - Handler database inserts.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 24-03-2016
 *
 */
#include <siri/db/insert.h>
#include <qpack/qpack.h>
#include <logger/logger.h>
#include <stdio.h>
#include <siri/siri.h>
#include <siri/net/protocol.h>
#include <siri/net/handle.h>
#include <siri/db/series.h>

static int32_t assign_by_map(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_packer_t * packer[],
        qp_obj_t * qp_obj);

static void send_points_to_pools(uv_async_t * handle);

static const char * err_msg[SIRIDB_INSERT_ERR_SIZE] = {
        "Expecting an array with points.",

        "Expecting a series name (string value) with an array of points where "
        "each point should be an integer time-stamp with a value.",

        "Expecting an array or map containing series and points.",

        "Expecting an integer value as time-stamp.",

        "Received at least one time-stamp which is out-of-range.",

        "Unsupported value received. (only integer, string and float values "
        "are supported).",

        "Expecting a series to have at least one point."
};

const char * siridb_insert_err_msg(siridb_insert_err_t err)
{
    return err_msg[err + SIRIDB_INSERT_ERR_SIZE];
}

int32_t siridb_insert_assign_pools(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_obj_t * qp_obj,
        qp_packer_t * packer[])
{
    size_t n;
    qp_types_t tp;

    for (n = 0; n < siridb->pools->size; n++)
    {
        packer[n] = qp_new_packer(QP_SUGGESTED_SIZE);
        qp_add_type(packer[n], QP_MAP_OPEN);
    }

    tp = qp_next(unpacker, NULL);

    if (qp_is_array(tp))
    {
        log_debug("Got an array...");
        return 0;
    }

    if (qp_is_map(tp))
        return assign_by_map(siridb, unpacker, packer, qp_obj);

    return ERR_EXPECTING_MAP_OR_ARRAY;
}

void siridb_insert_points(
        uint64_t pid,
        uv_handle_t * client,
        size_t size,
        uint16_t packer_size,
        qp_packer_t * packer[])
{
    siridb_insert_t * insert;
    uv_async_t * handle = (uv_async_t *) malloc(sizeof(uv_async_t));
    insert = (siridb_insert_t *) malloc(sizeof(siridb_insert_t) + packer_size * sizeof(qp_packer_t *));
    insert->pid = pid;
    insert->client = client;
    insert->size = size;
    insert->packer_size = packer_size;
    for (size_t n = 0; n < insert->packer_size; n++)
        insert->packer[n] = packer[n];

    uv_async_init(siri.loop, handle, (uv_async_cb) send_points_to_pools);
    handle->data = (void *) insert;
    uv_async_send(handle);
}

void siridb_free_insert(uv_handle_t * handle)
{
    siridb_insert_t * insert = (siridb_insert_t *) handle->data;

    /* free packer */
    for (size_t n = 0; n < insert->packer_size; n++)
        qp_free_packer(insert->packer[n]);

    /* free insert */
    free(insert);

    /* free handle */
    free((uv_async_t *) handle);

    #ifdef DEBUG
    log_debug("Free insert!, hooray!");
    #endif
}

static void send_points_to_pools(uv_async_t * handle)
{
    siridb_insert_t * insert = (siridb_insert_t *) handle->data;
    qp_types_t tp;
    siridb_series_t ** series;
    siridb_t * siridb = ((sirinet_handle_t *) insert->client->data)->siridb;
    uint16_t pool = siridb->server->pool;
    qp_obj_t * qp_series_name = qp_new_object();
    qp_obj_t * qp_series_ts = qp_new_object();
    qp_obj_t * qp_series_val = qp_new_object();
    sirinet_pkg_t * package;
    qp_packer_t * packer;

    for (uint16_t n = 0; n < insert->packer_size; n++)
    {
        if (n == pool)
        {
            qp_unpacker_t * unpacker = qp_new_unpacker(
                    insert->packer[n]->buffer,
                    insert->packer[n]->len);
            qp_next(unpacker, NULL); // map
            tp = qp_next(unpacker, qp_series_name); // first series or end
            while (tp == QP_RAW)
            {

                series = (siridb_series_t **) ct_get_sure(
                        siridb->series,
                        qp_series_name->via->raw);

                qp_next(unpacker, NULL); // array open
                qp_next(unpacker, NULL); // first point array2
                qp_next(unpacker, qp_series_ts); // first ts
                qp_next(unpacker, qp_series_val); // first val

                if (ct_is_empty(*series))
                {
                    (*series) = siridb_create_series(
                            siridb,
                            qp_series_name->via->raw,
                            siridb_qp_map_tp(qp_series_val->tp));
                }
                while ((tp = qp_next(unpacker, qp_series_name)) == QP_ARRAY2)
                {
                    qp_next(unpacker, qp_series_ts); // ts
                    qp_next(unpacker, qp_series_val); // val
                }
                if (tp == QP_ARRAY_CLOSE)
                    tp = qp_next(unpacker, qp_series_name);
            }
            qp_free_unpacker(unpacker);
        }
    }

    qp_free_object(qp_series_name);
    qp_free_object(qp_series_ts);
    qp_free_object(qp_series_val);

    packer = qp_new_packer(1024);
    qp_map_open(packer);
    qp_add_raw(packer, "success_msg", 11);
    qp_add_fmt(packer, "Inserted %zd point(s) successfully.", insert->size);

    package = sirinet_new_pkg(
            insert->pid,
            packer->len,
            SN_MSG_RESULT,
            packer->buffer);
    sirinet_send_pkg(insert->client, package, NULL);

    free(package);
    qp_free_packer(packer);

    uv_close((uv_handle_t *) handle, (uv_close_cb) siridb_free_insert);
}


static int32_t assign_by_map(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_packer_t * packer[],
        qp_obj_t * qp_obj)
{
    qp_types_t tp;
    uint16_t pool;
    int32_t count = 0;

    tp = qp_next(unpacker, qp_obj);

    while (tp == QP_RAW)
    {
        pool = siridb_pool_sn_raw(
                siridb,
                qp_obj->via->raw,
                qp_obj->len);

        qp_add_raw_term(packer[pool],
                qp_obj->via->raw,
                qp_obj->len);

        if (!qp_is_array(qp_next(unpacker, NULL)))
            return ERR_EXPECTING_ARRAY;

        qp_add_type(packer[pool], QP_ARRAY_OPEN);

        if ((tp = qp_next(unpacker, NULL)) != QP_ARRAY2)
            return ERR_EXPECTING_AT_LEAST_ONE_POINT;

        for (; tp == QP_ARRAY2; count++, tp = qp_next(unpacker, qp_obj))
        {
            qp_add_type(packer[pool], QP_ARRAY2);

            if (qp_next(unpacker, qp_obj) != QP_INT64)
                return ERR_EXPECTING_INTEGER_TS;

            if (!siridb_int64_valid_ts(siridb, qp_obj->via->int64))
                return ERR_TIMESTAMP_OUT_OF_RANGE;

            qp_add_int64(packer[pool], qp_obj->via->int64);

            switch (qp_next(unpacker, qp_obj))
            {
            case QP_RAW:
                qp_add_raw(packer[pool],
                        qp_obj->via->raw,
                        qp_obj->len);
                break;

            case QP_INT64:
                qp_add_int64(packer[pool],
                        qp_obj->via->int64);
                break;

            case QP_DOUBLE:
                qp_add_double(packer[pool],
                        qp_obj->via->real);
                break;

            default:
                return ERR_UNSUPPORTED_VALUE;
            }
        }

        if (tp == QP_ARRAY_CLOSE)
            tp = qp_next(unpacker, qp_obj);

        qp_add_type(packer[pool], QP_ARRAY_CLOSE);
    }

    if (tp != QP_END && tp != QP_MAP_CLOSE)
        return ERR_EXPECTING_SERIES_NAME;

    return count;
}
