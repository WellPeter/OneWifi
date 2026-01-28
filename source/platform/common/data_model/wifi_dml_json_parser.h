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

#ifndef WIFI_DML_JSON_PARSER_H
#define WIFI_DML_JSON_PARSER_H

#include "bus.h"
#include "cJSON.h"

/* Callback function pointer type for setting bus callbacks */
typedef int (*bus_cb_setter_fn)(const char *full_namespace, bus_callback_table_t *cb_table);

/**
 * @brief Common API to parse JSON schema file and register with bus callbacks
 * 
 * This function parses a JSON Schema file (WFA format) and automatically registers
 * all elements with the bus system using the provided callback setter function.
 * Supports both WFA Data Elements and Native data models using WFA JSON format.
 * 
 * @param handle Pointer to the bus handle
 * @param json_schema_filename Path to the JSON schema file
 * @param base_path Base path for TR-181 registration (e.g., "Device.WiFi" or "Device.WiFi.DataElements.Network")
 * @param search_key Optional key to search for in properties (e.g., "Network", NULL to process all)
 * @param callback_setter Function pointer to set bus callbacks (set_bus_callbackfunc_pointers or wfa_set_bus_callbackfunc_pointers)
 * @return RETURN_OK on success, RETURN_ERR on failure
 */
int parse_json_schema_and_register(bus_handle_t *handle, const char *json_schema_filename, 
                                   const char *base_path, const char *search_key,
                                   bus_cb_setter_fn callback_setter);

/**
 * @brief Parse and register WFA Data Elements JSON schema
 * 
 * Convenience wrapper for WFA Data Elements schemas.
 * 
 * @param handle Pointer to the bus handle
 * @param json_schema_filename Path to the JSON schema file
 * @return RETURN_OK on success, RETURN_ERR on failure
 */
int parse_and_register_wfa_schema(bus_handle_t *handle, const char *json_schema_filename);

/**
 * @brief Parses native DML schema from JSON file and registers it with the bus.
 *
 * Convenience wrapper for native DML schemas.
 *
 * @param handle Pointer to the bus handle
 * @param json_schema_filename Path to the JSON schema file
 * @return RETURN_OK on success, RETURN_ERR on failure
 */
int parse_and_register_native_dml_schema(bus_handle_t *handle, const char *json_schema_filename);

#endif // WIFI_DML_JSON_PARSER_H
