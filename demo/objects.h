/*
 * Copyright 2017 AVSystem <avsystem@avsystem.com>
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

#ifndef DEMO_OBJECTS_H
#define DEMO_OBJECTS_H

#include <stdint.h>
#include <stdbool.h>

#include <avsystem/commons/list.h>

#include <anjay/access_control.h>
#include <anjay/server.h>
#include <anjay/anjay.h>

#include "iosched.h"
#include "demo_utils.h"

typedef struct anjay_demo_struct anjay_demo_t;

#define DEMO_OID_SECURITY           0
#define DEMO_OID_SERVER             1
#define DEMO_OID_DEVICE             3
#define DEMO_OID_CONN_MONITORING    4
#define DEMO_OID_FIRMWARE_UPDATE    5
#define DEMO_OID_LOCATION           6
#define DEMO_OID_CONN_STATISTICS    7
#define DEMO_OID_CELL_CONNECTIVITY 10
#define DEMO_OID_APN_CONN_PROFILE  11
#define DEMO_OID_TEST            1337
#define DEMO_OID_EXT_DEV_INFO   11111
#define DEMO_OID_IP_PING        12359
#define DEMO_OID_GEOPOINTS      12360
#define DEMO_OID_DOWNLOAD_DIAG  12361

const anjay_dm_object_def_t **device_object_create(iosched_t *iosched,
                                                   const char *endpoint_name);
void device_object_release(const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **
firmware_update_object_create(iosched_t *iosched,
                              const char *fw_updated_marker_path);
void firmware_update_object_release(const anjay_dm_object_def_t **def);

void firmware_update_set_package_path(anjay_t *anjay,
                                      const anjay_dm_object_def_t **fw_obj,
                                      const char *path);

#define MAX_SERVERS 1024

typedef struct {
    anjay_ssid_t id;
    bool is_bootstrap;
    const char *uri;
} server_entry_t;

typedef struct {
    server_entry_t servers[MAX_SERVERS];
    int32_t bootstrap_holdoff_s;
    int32_t bootstrap_timeout_s;
    int32_t lifetime;
    anjay_binding_mode_t binding_mode;
    anjay_udp_security_mode_t security_mode;
    uint8_t *public_cert_or_psk_identity;
    size_t public_cert_or_psk_identity_size;

    uint8_t *private_cert_or_psk_key;
    size_t private_cert_or_psk_key_size;

    uint8_t *server_public_key;
    size_t server_public_key_size;
} server_connection_args_t;

#define DEMO_FOREACH_SERVER_ENTRY(It, ConnArgs) \
        for ((It) = &(ConnArgs)->servers[0]; \
                (It) < &(ConnArgs)->servers[MAX_SERVERS] && (It)->uri; \
                ++(It))

#define UNDEFINED_LIFETIME -1

const anjay_dm_object_def_t **test_object_create(void);
void test_object_release(const anjay_dm_object_def_t **def);
void test_notify_time_dependent(anjay_t *anjay,
                                const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **cm_object_create(void);
void cm_object_release(const anjay_dm_object_def_t **def);
void cm_notify_time_dependent(anjay_t *anjay,
                              const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **cs_object_create(void);
void cs_object_release(const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **download_diagnostics_object_create(void);
void download_diagnostics_object_release(const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **ext_dev_info_object_create(void);
void ext_dev_info_object_release(const anjay_dm_object_def_t **def);
void ext_dev_info_notify_time_dependent(anjay_t *anjay,
                                        const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **
ip_ping_object_create(iosched_t *iosched);
void ip_ping_object_release(const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **apn_conn_profile_object_create(void);
void apn_conn_profile_object_release(const anjay_dm_object_def_t **def);

AVS_LIST(anjay_iid_t)
apn_conn_profile_list_activated(const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **
cell_connectivity_object_create(anjay_demo_t *demo);
void cell_connectivity_object_release(const anjay_dm_object_def_t **def);

const anjay_dm_object_def_t **location_object_create(void);
void location_object_release(const anjay_dm_object_def_t **def);
void location_notify_time_dependent(anjay_t *anjay,
                                    const anjay_dm_object_def_t **def);
void location_get(const anjay_dm_object_def_t **def,
                  double *out_latitude, double *out_longitude);
int location_open_csv(const anjay_dm_object_def_t **def,
                      const char *file_name, time_t frequency_s);

const anjay_dm_object_def_t **geopoints_object_create(anjay_demo_t *demo);
void geopoints_object_release(const anjay_dm_object_def_t **def);
void geopoints_notify_time_dependent(anjay_t *anjay,
                                     const anjay_dm_object_def_t **def);

#endif // DEMO_OBJECTS_H
