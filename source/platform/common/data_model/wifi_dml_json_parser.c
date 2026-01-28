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
#include <unistd.h>
#include <stdbool.h>
#include "bus.h"
#include "wifi_data_model_parse.h"
#include "wifi_data_model.h"
#include "wifi_dml_api.h"

/* Forward declarations for JSON schema parsing functions */
static cJSON* resolve_ref(cJSON* root, const char* ref_str);
static cJSON* follow_ref_if_any(cJSON* root, cJSON* node);
static bool schema_has_type(cJSON* schema, const char* want);
static void parse_property_constraints(cJSON* schema_node, data_model_properties_t* props);
static void parse_readwrite(cJSON* schema_node, data_model_properties_t* props);
static void handle_property_node(cJSON* root, const char* full_path, cJSON* property_schema, bus_handle_t *handle);
static void traverse_schema(cJSON* root, cJSON* schema_node, const char* base_path, bus_handle_t *handle);
static char* yang_to_tr181_path(const char* yang_path);
static int wfa_bus_register_namespace(bus_handle_t *handle, const char *full_namespace, 
                                       bus_element_type_t element_type, bus_callback_table_t cb_table, 
                                       data_model_properties_t data_model_value, int num_of_rows);
static int wfa_set_bus_callbackfunc_pointers(const char *full_namespace, bus_callback_table_t *cb_table);

/* String replace helper function */
static char* str_replace(const char* str, const char* from, const char* to)
{
    if (!str || !from || !to) return NULL;
    
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t str_len = strlen(str);
    
    /* Count occurrences */
    size_t count = 0;
    const char* pos = str;
    while ((pos = strstr(pos, from)) != NULL) {
        count++;
        pos += from_len;
    }
    
    if (count == 0) {
        char* result = malloc(str_len + 1);
        if (result) {
            strcpy(result, str);
        }
        return result;
    }
    
    /* Allocate result buffer */
    size_t result_len = str_len + count * (to_len - from_len);
    char* result = malloc(result_len + 1);
    if (!result) {
        return NULL;
    }
    
    /* Perform replacement */
    char* dst = result;
    pos = str;
    while (1) {
        const char* next = strstr(pos, from);
        if (!next) {
            strcpy(dst, pos);
            break;
        }
        size_t len = next - pos;
        memcpy(dst, pos, len);
        dst += len;
        memcpy(dst, to, to_len);
        dst += to_len;
        pos = next + from_len;
    }
    
    return result;
}

/* Convert YANG path to TR-181 path format */
static char* yang_to_tr181_path(const char* yang_path)
{
    if (!yang_path) {
        return NULL;
    }
    
    /* Mapping table for YANG to TR-181 conversions */
    struct yang_map {
        const char* yang;
        const char* tr181;
    };
    
    static const struct yang_map mappings[] = {
        { "DeviceList", "Device" },
        { "RadioList", "Radio" },
        { "BSSList", "BSS" },
        { "STAList", "STA" },
        { "NetworkSSIDList", "SSID" },
        { NULL, NULL }
    };
    
    char* result = malloc(strlen(yang_path) + 1);
    if (!result) {
        return NULL;
    }
    strcpy(result, yang_path);
    
    /* Apply each mapping */
    for (int i = 0; mappings[i].yang != NULL; i++) {
        char* temp = str_replace(result, mappings[i].yang, mappings[i].tr181);
        if (temp) {
            free(result);
            result = temp;
        }
    }
    
    return result;
}

/* Resolve $ref like "#/definitions/Default8021Q_g" */
static cJSON* resolve_ref(cJSON* root, const char* ref_str)
{
    const char* path = NULL;
    char* path_copy = NULL;
    cJSON* node = NULL;
    char* token = NULL;
    
    if (!root || !ref_str || ref_str[0] != '#') {
        return NULL;
    }
    
    /* Skip "#/" prefix */
    path = ref_str;
    if (strncmp(path, "#/", 2) == 0) {
        path += 2;
    }
    
    /* Make a mutable copy of the path */
    path_copy = strdup(path);
    if (!path_copy) {
        return NULL;
    }
    
    node = root;
    token = strtok(path_copy, "/");
    
    while (token != NULL && node != NULL) {
        node = cJSON_GetObjectItem(node, token);
        token = strtok(NULL, "/");
    }
    
    free(path_copy);
    return node;
}

/* Resolve $ref if present on the node; otherwise return the node itself */
static cJSON* follow_ref_if_any(cJSON* root, cJSON* node)
{
    cJSON* ref = NULL;
    cJSON* resolved = NULL;
    cJSON* comb = NULL;
    cJSON* it = NULL;
    cJSON* type = NULL;
    bool only_null = false;
    cJSON* t = NULL;
    
    if (!node) {
        return NULL;
    }
    
    /* Resolve $ref */
    ref = cJSON_GetObjectItem(node, "$ref");
    if (ref && cJSON_IsString(ref)) {
        resolved = resolve_ref(root, ref->valuestring);
        if (resolved) {
            return follow_ref_if_any(root, resolved);
        }
    }
    
    /* Unwrap oneOf / anyOf, skip null */
    comb = cJSON_GetObjectItem(node, "oneOf");
    if (!comb) {
        comb = cJSON_GetObjectItem(node, "anyOf");
    }
    
    if (comb && cJSON_IsArray(comb)) {
        it = comb->child;
        while (it) {
            type = cJSON_GetObjectItem(it, "type");
            
            /* Skip null-only variants */
            if (type) {
                if (cJSON_IsString(type) && strcmp(type->valuestring, "null") == 0) {
                    it = it->next;
                    continue;
                }
                if (cJSON_IsArray(type)) {
                    only_null = true;
                    t = type->child;
                    while (t) {
                        if (cJSON_IsString(t) && strcmp(t->valuestring, "null") != 0) {
                            only_null = false;
                            break;
                        }
                        t = t->next;
                    }
                    if (only_null) {
                        it = it->next;
                        continue;
                    }
                }
            }
            return follow_ref_if_any(root, it);
        }
    }
    return node;
}

/* Extract min/max range, type, read/write from leaf node */
static void parse_property_constraints(cJSON* schema_node, data_model_properties_t* props)
{
    if (!schema_node || !props) {
        return;
    }
    
    /* min / max from JSON schema */
    cJSON* minimum = cJSON_GetObjectItem(schema_node, "minimum");
    cJSON* maximum = cJSON_GetObjectItem(schema_node, "maximum");
    
    if (minimum && cJSON_IsNumber(minimum)) {
        props->min_data_range = minimum->valuedouble;
    }
    
    if (maximum && cJSON_IsNumber(maximum)) {
        props->max_data_range = maximum->valuedouble;
    }
}

static void parse_readwrite(cJSON* schema_node, data_model_properties_t* props)
{
    if (!schema_node || !props) {
        return;
    }
    
    cJSON* writable = cJSON_GetObjectItem(schema_node, "writable");
    if (writable && cJSON_IsTrue(writable)) {
        props->data_permission = 1;
    } else {
        props->data_permission = 0;
    }
}

static bool schema_has_type(cJSON* schema, const char* want)
{
    if (!schema || !want) {
        return false;
    }
    
    cJSON* type = cJSON_GetObjectItem(schema, "type");
    if (!type) {
        return false;
    }
    
    if (cJSON_IsString(type)) {
        return strcmp(type->valuestring, want) == 0;
    }
    
    if (cJSON_IsArray(type)) {
        cJSON* it = type->child;
        while (it) {
            if (cJSON_IsString(it) && strcmp(it->valuestring, want) == 0) {
                return true;
            }
            it = it->next;
        }
    }
    return false;
}

/* Handle ANY property under an object: decide if TABLE or PROPERTY */
static void handle_property_node(cJSON* root, const char* full_path, cJSON* property_schema, bus_handle_t *handle)
{
    bus_callback_table_t cb_table;
    data_model_properties_t data_model_value;
    cJSON* effective = NULL;
    cJSON* props_obj = NULL;
    cJSON* items = NULL;
    cJSON* items_eff = NULL;
    cJSON* item_props = NULL;
    char table_name[512] = {0};
    char* tr181_path = NULL;
    
    if (!property_schema || !full_path || !handle) {
        return;
    }
    
    memset(&cb_table, 0, sizeof(cb_table));
    memset(&data_model_value, 0, sizeof(data_model_value));
    
    /* 1) follow top-level $ref / combiners if present */
    effective = follow_ref_if_any(root, property_schema);
    if (!effective) {
        return;
    }
    
    /* 2) If effective has properties -> expand (this handles $ref -> object with properties) */
    props_obj = cJSON_GetObjectItem(effective, "properties");
    if (props_obj && cJSON_IsObject(props_obj)) {
        traverse_schema(root, effective, full_path, handle);
        return;
    }
    
    /* 3) If type is array -> register TABLE, and examine items, only if array type is object */
    if (schema_has_type(effective, "array")) {
        /* now inspect items */
        items = cJSON_GetObjectItem(effective, "items");
        if (!items) {
            return;
        }
        
        items_eff = follow_ref_if_any(root, items);
        if (!items_eff) {
            return;
        }
        
        item_props = cJSON_GetObjectItem(items_eff, "properties");
        if (item_props && cJSON_IsObject(item_props)) {
            snprintf(table_name, sizeof(table_name), "%s.{i}", full_path);
            
            /* reset and fill constraints for the array property itself */
            memset(&data_model_value, 0, sizeof(data_model_value));
            parse_property_constraints(effective, &data_model_value);
            parse_readwrite(effective, &data_model_value);
            
            tr181_path = yang_to_tr181_path(table_name);
            if (tr181_path) {
                wfa_set_bus_callbackfunc_pointers(tr181_path, &cb_table);
                wfa_bus_register_namespace(handle, tr181_path, bus_element_type_table, cb_table, data_model_value, 1);
                free(tr181_path);
            }
            
            /* expand row children under table_name */
            traverse_schema(root, items_eff, table_name, handle);
        } else {
            /* primitive array -> register the row as property */
            memset(&data_model_value, 0, sizeof(data_model_value));
            parse_property_constraints(items_eff, &data_model_value);
            parse_readwrite(items_eff, &data_model_value);
            
            tr181_path = yang_to_tr181_path(full_path);
            if (tr181_path) {
                wfa_set_bus_callbackfunc_pointers(tr181_path, &cb_table);
                wfa_bus_register_namespace(handle, tr181_path, bus_element_type_property, cb_table, data_model_value, 1);
                free(tr181_path);
            }
        }
        return;
    }
    
    /* 4) If type is object (but had no direct properties above),
       try to resolve any nested $ref and check again */
    if (schema_has_type(effective, "object")) {
        /* we've already tried follow_ref_if_any at top-level; if still no properties, treat as leaf object */
        memset(&data_model_value, 0, sizeof(data_model_value));
        parse_property_constraints(effective, &data_model_value);
        parse_readwrite(effective, &data_model_value);
        
        tr181_path = yang_to_tr181_path(full_path);
        if (tr181_path) {
            wfa_set_bus_callbackfunc_pointers(tr181_path, &cb_table);
            wfa_bus_register_namespace(handle, tr181_path, bus_element_type_property, cb_table, data_model_value, 1);
            free(tr181_path);
        }
        return;
    }
    
    /* 5) Fallback: primitive (string/number/boolean/enum) - register as property */
    memset(&data_model_value, 0, sizeof(data_model_value));
    parse_property_constraints(effective, &data_model_value);
    parse_readwrite(effective, &data_model_value);
    
    tr181_path = yang_to_tr181_path(full_path);
    if (tr181_path) {
        wfa_set_bus_callbackfunc_pointers(tr181_path, &cb_table);
        wfa_bus_register_namespace(handle, tr181_path, bus_element_type_property, cb_table, data_model_value, 1);
        free(tr181_path);
    }
}

/* Traverse object schema and process all "properties" */
static void traverse_schema(cJSON* root, cJSON* schema_node, const char* base_path, bus_handle_t *handle)
{
    cJSON* effective = NULL;
    cJSON* props = NULL;
    cJSON* child = NULL;
    char new_path[512] = {0};
    
    if (!schema_node || !base_path || !handle) {
        return;
    }
    
    /* ensure we operate on resolved node (if schema_node is a wrapper with $ref) */
    effective = follow_ref_if_any(root, schema_node);
    if (!effective) {
        return;
    }
    
    props = cJSON_GetObjectItem(effective, "properties");
    if (!props || !cJSON_IsObject(props)) {
        return;
    }
    
    child = props->child;
    while (child) {
        if (child->string) {

            if (base_path[strlen(base_path) - 1] != '.') {
                snprintf(new_path, sizeof(new_path), "%s.%s", base_path, child->string);
            } else {
                snprintf(new_path, sizeof(new_path), "%s%s", base_path, child->string);
            }
            
            /* pass the child's schema node (not child->child) because child is a property pair */
            handle_property_node(root, new_path, child, handle);
        }
        child = child->next;
    }
}

/* Entry function to parse and register WFA schema */
static bool parse_and_register_json_schema(bus_handle_t *handle, const char *filename)
{
    FILE* f = NULL;
    long size = 0;
    char* buf = NULL;
    size_t read_bytes = 0;
    cJSON* root = NULL;
    cJSON* props = NULL;
    cJSON* child = NULL;
    
    /* Load file */
    f = fopen(filename, "rb");
    if (!f) {
        return false;
    }
    
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size < 0) {
        fclose(f);
        return false;
    }
    
    buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    
    read_bytes = fread(buf, 1, size, f);
    fclose(f);
    buf[read_bytes] = '\0';
    
    root = cJSON_Parse(buf);
    free(buf);
    
    if (!root) {
        return false;
    }
    
    /* Find top-level Network element */
    props = cJSON_GetObjectItem(root, "properties");
    if (!props) {
        cJSON_Delete(root);
        return false;
    }
    
    child = props->child;
    while (child) {
        if (child->string && strstr(child->string, "Network") != NULL) {
            traverse_schema(root, child, "Device.WiFi.DataElements.Network", handle);
            break;
        }
        child = child->next;
    }
    
    cJSON_Delete(root);
    return true;
}

/* Stub implementations - these would need to be properly implemented */
static int wfa_set_bus_callbackfunc_pointers(const char *full_namespace, bus_callback_table_t *cb_table)
{
    /* TODO: Implement proper callback function pointer assignment based on namespace */
    /* For now, use default handlers */
    bus_data_cb_func_t bus_default_data_cb = { " ",
        { default_get_param_value, default_set_param_value, default_table_add_row_handler,
          default_table_remove_row_handler, default_event_sub_handler, NULL }
    };

    memcpy(cb_table, &bus_default_data_cb.cb_func, sizeof(bus_callback_table_t));

    return RETURN_OK;
}

static int wfa_bus_register_namespace(bus_handle_t *handle, const char *full_namespace, 
                                       bus_element_type_t element_type, bus_callback_table_t cb_table, 
                                       data_model_properties_t data_model_value, int num_of_rows)
{
    /* Reuse existing bus_register_namespace function */
    return bus_register_namespace(handle, (char*)full_namespace, element_type, cb_table, data_model_value, num_of_rows);
}

/**
 * @brief Parse and register WFA Data Elements JSON schema
 * 
 * This function parses the WFA Data Elements JSON Schema (e.g., Data_Elements_JSON_Schema_v3.0.json)
 * and automatically registers all elements with the bus system. It handles:
 * - JSON $ref resolution
 * - oneOf/anyOf schema combinations
 * - Array types (registered as tables)
 * - Object types with properties
 * - Primitive types
 * - YANG to TR-181 path conversion
 * 
 * @param handle Pointer to the bus handle
 * @param json_schema_filename Path to the JSON schema file
 * @return RETURN_OK on success, RETURN_ERR on failure
 */
int parse_wfa_data_elements_schema(bus_handle_t *handle, const char *json_schema_filename)
{
    bool result = false;
    
    if (!handle || !json_schema_filename) {
        wifi_util_error_print(WIFI_DMCLI, "%s:%d: Invalid parameters\n", __func__, __LINE__);
        return RETURN_ERR;
    }
    
    wifi_util_info_print(WIFI_DMCLI, "%s:%d: Parsing WFA Data Elements schema: %s\n", 
                        __func__, __LINE__, json_schema_filename);
    
    result = parse_and_register_json_schema(handle, json_schema_filename);
    
    if (result) {
        wifi_util_info_print(WIFI_DMCLI, "%s:%d: Successfully parsed and registered WFA schema\n", 
                            __func__, __LINE__);
        return RETURN_OK;
    } else {
        wifi_util_error_print(WIFI_DMCLI, "%s:%d: Failed to parse WFA schema: %s\n", 
                             __func__, __LINE__, json_schema_filename);
        return RETURN_ERR;
    }
}
