/*
 * This file is part of the Soletta (TM) Project
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sol-flow/location.h"
#include "sol-flow-internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <math.h>

#include <sol-http-client.h>
#include <sol-json.h>
#include <sol-mainloop.h>
#include <sol-util-internal.h>

struct freegeoip_data {
    struct sol_flow_node *node;
    struct sol_ptr_vector pending_conns;
    char *endpoint;
};

static void
freegeoip_query_finished(void *data,
    struct sol_http_client_connection *connection,
    struct sol_http_response *response)
{
    struct freegeoip_data *mdata = data;
    struct sol_json_scanner scanner;
    struct sol_json_token token, key, value;
    enum sol_json_loop_status reason;
    struct sol_location location = {
        .lat = FP_NAN,
        .lon = FP_NAN,
        .alt = FP_NAN,
    };

    if (sol_ptr_vector_remove(&mdata->pending_conns, connection) < 0)
        SOL_WRN("Failed to find pending connection %p", connection);

    if (!response) {
        sol_flow_send_error_packet(mdata->node, EINVAL,
            "Error while reaching Freegeoip");
        return;
    }

    if (response->response_code != SOL_HTTP_STATUS_OK) {
        sol_flow_send_error_packet(mdata->node, EINVAL,
            "FreeGeoIP returned an unknown response code: %d",
            response->response_code);
        return;
    }

    if (!response->content.used) {
        sol_flow_send_error_packet(mdata->node, EINVAL,
            "Empty response from FreeGeoIP");
        return;
    }

#define JSON_FIELD_TO_FLOW_PORT(json_field_, flow_field_) \
    if (sol_json_token_str_eq(&key, json_field_, strlen(json_field_))) { \
        sol_flow_send_string_slice_packet(mdata->node, \
            SOL_FLOW_NODE_TYPE_LOCATION_FREEGEOIP__OUT__ ## flow_field_, \
            SOL_STR_SLICE_STR(value.start + 1, value.end - value.start - 2)); \
        continue; \
    }

    sol_json_scanner_init(&scanner, response->content.data, response->content.used);
    SOL_JSON_SCANNER_OBJECT_LOOP (&scanner, &token, &key, &value, reason) {
        JSON_FIELD_TO_FLOW_PORT("ip", IP)
        JSON_FIELD_TO_FLOW_PORT("country_name", COUNTRY_NAME)
        JSON_FIELD_TO_FLOW_PORT("country_code", COUNTRY_CODE)
        JSON_FIELD_TO_FLOW_PORT("city", CITY_NAME)
        JSON_FIELD_TO_FLOW_PORT("zip_code", ZIP_CODE)
        JSON_FIELD_TO_FLOW_PORT("time_zone", TIMEZONE)

        if (SOL_JSON_TOKEN_STR_LITERAL_EQ(&key, "latitude")) {
            if (sol_json_token_get_double(&value, &location.lat) < 0)
                goto error;
        } else if (SOL_JSON_TOKEN_STR_LITERAL_EQ(&key, "longitude")) {
            if (sol_json_token_get_double(&value, &location.lon) < 0)
                goto error;
        } else {
            SOL_DBG("Unknown key in FreeGeoip response: %.*s. Ignoring.",
                (int)(key.end - key.start), key.start);
        }
    }

#undef JSON_FIELD_TO_FLOW_PORT

    /* `reason` is ignored here; only the following matters now: */
    if (!isnan(location.lat) && !isnan(location.lon)) {
        sol_flow_send_location_packet(mdata->node,
            SOL_FLOW_NODE_TYPE_LOCATION_FREEGEOIP__OUT__LOCATION, &location);
    }

    return;

error:
    sol_flow_send_error_packet(mdata->node, EINVAL,
        "Error while parsing location from FreeGeoIP");
}

static int
query_addr(struct freegeoip_data *mdata, const char *addr)
{
    int r;
    char json_endpoint[PATH_MAX];
    struct sol_http_client_connection *connection;

    r = snprintf(json_endpoint, sizeof(json_endpoint), "%s/json/%s",
        mdata->endpoint, addr ? addr : "");
    if (r < 0 || r >= (int)sizeof(json_endpoint)) {
        SOL_WRN("Could not prepare endpoint");
        return -EINVAL;
    }

    connection = sol_http_client_request(SOL_HTTP_METHOD_GET, json_endpoint,
        NULL, freegeoip_query_finished, mdata);

    SOL_NULL_CHECK_MSG(connection, -ENOTCONN, "Could not create HTTP request");

    r = sol_ptr_vector_append(&mdata->pending_conns, connection);
    if (r < 0) {
        SOL_WRN("Failed to keep pending connection.");
        sol_http_client_connection_cancel(connection);
        return -ENOMEM;
    }

    return 0;
}

static int
freegeoip_in_process(struct sol_flow_node *node, void *data,
    uint16_t port, uint16_t conn_id, const struct sol_flow_packet *packet)
{
    struct freegeoip_data *mdata = data;

    return query_addr(mdata, NULL);
}

static int
freegeoip_ip_process(struct sol_flow_node *node, void *data,
    uint16_t port, uint16_t conn_id, const struct sol_flow_packet *packet)
{
    struct freegeoip_data *mdata = data;
    const char *addr;
    int r;

    r = sol_flow_packet_get_string(packet, &addr);
    if (r < 0)
        return r;

    return query_addr(mdata, addr);
}

static void
freegeoip_close(struct sol_flow_node *node, void *data)
{
    struct sol_http_client_connection *connection;
    struct freegeoip_data *mdata = data;
    uint16_t i;

    free(mdata->endpoint);

    SOL_PTR_VECTOR_FOREACH_IDX (&mdata->pending_conns, connection, i)
        sol_http_client_connection_cancel(connection);
    sol_ptr_vector_clear(&mdata->pending_conns);
}

static int
freegeoip_open(struct sol_flow_node *node, void *data, const struct sol_flow_node_options *options)
{
    struct freegeoip_data *mdata = data;
    const struct sol_flow_node_type_location_freegeoip_options *opts;

    SOL_FLOW_NODE_OPTIONS_SUB_API_CHECK(options, SOL_FLOW_NODE_TYPE_LOCATION_FREEGEOIP_OPTIONS_API_VERSION,
        -EINVAL);
    opts = (const struct sol_flow_node_type_location_freegeoip_options *)options;

    mdata->node = node;
    mdata->endpoint = strdup(opts->endpoint);
    SOL_NULL_CHECK(mdata->endpoint, -ENOMEM);

    sol_ptr_vector_init(&mdata->pending_conns);

    return 0;
}

#include "location-gen.c"
