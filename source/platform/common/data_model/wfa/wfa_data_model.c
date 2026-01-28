/************************************************************************************
  If not stated otherwise in this file or this component's LICENSE file the
  following copyright and licenses apply:

  Copyright 2025 RDK Management

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 **************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bus.h"
#include "wifi_data_model_parse.h"
#include "wifi_data_model.h"
#include "wifi_dml_api.h"
#include "wfa_data_model.h"
#include "wfa_dml_cb.h"
#include "wifi_ctrl.h"

static bus_error_t wfa_network_get(char *event_name, raw_data_t *p_data,struct bus_user_data * user_data )
{
    char     extension[64]    = {0};
    wifi_global_param_t *pcfg = get_wifidb_wifi_global_param();
    dml_callback_table_t dml_data_cb = {
        NULL, NULL, wfa_network_get_param_uint_value, wfa_network_get_param_string_value,
        NULL, NULL, NULL, NULL
    };
    
    sscanf(event_name, DATAELEMS_NETWORK_OBJ ".%s", extension);

    wifi_util_info_print(WIFI_DMCLI,"%s:%d get event:[%s][%s]\n", __func__, __LINE__, event_name, extension);

    bus_error_t status = dml_get_set_param_value(&dml_data_cb, DML_GET_CB, (void *)pcfg, extension, p_data);
    if (status != bus_error_success) {
        wifi_util_error_print(WIFI_DMCLI,"%s:%d wifi param get failed for:[%s][%s]\r\n", __func__, __LINE__, event_name, extension);
    }

    return status;
}

/* WFA DataElements callback function pointer mapping */
int wfa_set_bus_callbackfunc_pointers(const char *full_namespace, bus_callback_table_t *cb_table)
{
    bus_data_cb_func_t bus_data_cb[] = {
        /* Device.WiFi.DataElements.Network */
        { DATAELEMS_NETWORK_OBJ,
            { wfa_network_get, NULL, NULL,
              NULL, default_event_sub_handler, NULL } },
    };

    /* For now, use default handlers */
    bus_data_cb_func_t bus_default_data_cb = { " ",
        { default_get_param_value, default_set_param_value, default_table_add_row_handler,
          default_table_remove_row_handler, default_event_sub_handler, NULL }
    };

    uint32_t index = 0;
    bool     table_found = false;

    for (index = 0; index < (uint32_t)ARRAY_SZ(bus_data_cb); index++) {
        if (STR_CMP(full_namespace, bus_data_cb[index].cb_table_name)) {
            memcpy(cb_table, &bus_data_cb[index].cb_func, sizeof(bus_callback_table_t));
            table_found = true;
            break;
        }
    }

    if (table_found == false) {
        wifi_util_info_print(WIFI_DMCLI,"%s:%d:default cb set for namespace:[%s]\n", __func__, __LINE__, full_namespace);
        memcpy(cb_table, &bus_default_data_cb.cb_func, sizeof(bus_callback_table_t));
    }

    return RETURN_OK;
}
