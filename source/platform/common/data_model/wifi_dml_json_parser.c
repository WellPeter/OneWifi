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
#include "wifi_dml_json_parser.h"
#include "wfa/wfa_data_model.h"

/* Forward declarations for JSON schema parsing functions */
static cJSON* resolve_ref(cJSON* root, const char* ref_str);
static cJSON* follow_ref_if_any(cJSON* root, cJSON* node);
static bool schema_has_type(cJSON* schema, const char* want);
static void parse_property_constraints(cJSON* schema_node, data_model_properties_t* props);
static void handle_property_node(cJSON* root, const char* full_path, cJSON* property_schema, bus_handle_t *handle, bus_callback_setter_fn callback_setter);
static void traverse_schema(cJSON* root, cJSON* schema_node, const char* base_path, bus_handle_t *handle, bus_callback_setter_fn callback_setter);
static char* yang_to_tr181_path(const char* yang_path);
static bool parse_and_register_schema_internal(bus_handle_t *handle, cJSON* root, const char* base_path, const char* search_key, bus_callback_setter_fn callback_setter);

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
            wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Path conversion: %s -> %s\n", __func__, __LINE__, yang_path, result);
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

    wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Resolving $ref: %s\n", __func__, __LINE__, ref_str);
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
        } else {
            wifi_util_info_print(WIFI_DMCLI, "%s:%d: Failed to resolve $ref: %s\n", __func__, __LINE__, ref->valuestring);
        }
    }

    /* Unwrap oneOf / anyOf, skip null */
    comb = cJSON_GetObjectItem(node, "oneOf");
    if (!comb) {
        comb = cJSON_GetObjectItem(node, "anyOf");
    }
    
    if (comb && cJSON_IsArray(comb)) {
        wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Following oneOf/anyOf combiner\n", __func__, __LINE__);
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

static void parse_property_readwrite(cJSON* schema_node, data_model_properties_t* props)
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

static void parse_property_type(cJSON* schema_node, data_model_properties_t* props)
{
    if (!schema_node || !props) {
        return;
    }
    
    cJSON* type = cJSON_GetObjectItem(schema_node, "type");

    if (type && cJSON_IsArray(type)) {
        cJSON* it = type->child;
        while (it) {
            /* Find first non-null type */
            if (cJSON_IsString(it) && strcmp(it->valuestring, "null") != 0) {
                break;
            }
            it = it->next;
        }
        type = it;
    }

    if (type && cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "string") == 0) {
            props->data_format = bus_data_type_string;
        } else if (strcmp(type->valuestring, "boolean") == 0 ||
                   strcmp(type->valuestring, "bool") == 0) {
            props->data_format = bus_data_type_boolean;
        } else if (strcmp(type->valuestring, "integer") == 0) {
            props->data_format = bus_data_type_int32;
        } else if (strcmp(type->valuestring, "uint32_t") == 0) {
            props->data_format = bus_data_type_uint32;
        } else if (strcmp(type->valuestring, "uint16_t") == 0) {
            props->data_format = bus_data_type_uint16;
        } else if (strcmp(type->valuestring, "uint8_t") == 0) {
            props->data_format = bus_data_type_uint8;
        } else if (strcmp(type->valuestring, "int32_t") == 0) {
            props->data_format = bus_data_type_int32;
        } else if (strcmp(type->valuestring, "int16_t") == 0) {
            props->data_format = bus_data_type_int16;
        } else if (strcmp(type->valuestring, "int8_t") == 0) {
            props->data_format = bus_data_type_int8;
        } else {
            wifi_util_info_print(WIFI_DMCLI, "%s:%d: Unknown type: %s\n", __func__, __LINE__, type->valuestring);
        }
    }

    cJSON* str_enum = cJSON_GetObjectItem(schema_node, "enum");

    if (str_enum && cJSON_IsArray(str_enum)) {
        props->num_of_str_validation = cJSON_GetArraySize(str_enum);
        props->str_validation = malloc(sizeof(char *) * props->num_of_str_validation);
        if (props->str_validation == NULL) {
            props->num_of_str_validation = 0;
            return;
        }

        for (uint32_t i = 0; i < props->num_of_str_validation; i++) {
            cJSON *item = cJSON_GetArrayItem(str_enum, i);
            if (item != NULL && cJSON_IsString(item)) {
                props->str_validation[i] = malloc(strlen(item->valuestring) + 1);
                strncpy(props->str_validation[i], item->valuestring, strlen(item->valuestring) + 1);
            }
        }
    }
}

/* Extract min/max range, type, enum, read/write from leaf node */
/* Expecting to enter after following all $ref / combiners */
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

    parse_property_readwrite(schema_node, props);
    parse_property_type(schema_node, props);
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
static void handle_property_node(cJSON* root, const char* full_path, cJSON* property_schema, bus_handle_t *handle, bus_callback_setter_fn callback_setter)
{
    bus_callback_table_t cb_table;
    data_model_properties_t data_model_value;
    cJSON* effective = NULL;
    cJSON* props_obj = NULL;
    cJSON* items = NULL;
    cJSON* items_eff = NULL;
    cJSON* item_props = NULL;
    bus_name_string_t table_name = {0};
    char* tr181_path = NULL;
    
    if (!property_schema || !full_path || !handle || !callback_setter) {
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
        wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Found object with properties at: %s\n", __func__, __LINE__, full_path);
        traverse_schema(root, effective, full_path, handle, callback_setter);
        return;
    }

    /* 3) If type is array -> register TABLE, and examine items, only if array type is object */
    if (schema_has_type(effective, "array")) {
        wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Found array type at: %s\n", __func__, __LINE__, full_path);
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
            parse_property_constraints(effective, &data_model_value); // TODO maybe need only writable

            tr181_path = yang_to_tr181_path(table_name);
            if (tr181_path) {
                wifi_util_info_print(WIFI_DMCLI, "%s:%d: Registering table: %s\n", __func__, __LINE__, tr181_path);
                callback_setter(tr181_path, &cb_table);
                cb_table.get_handler = NULL;
                cb_table.set_handler = NULL;
                bus_register_namespace(handle, tr181_path, bus_element_type_table, cb_table, data_model_value, 1);
                free(tr181_path);
            }

            /* expand row children under table_name */
            traverse_schema(root, items_eff, table_name, handle, callback_setter);
        } else {
            /* primitive array -> register the row as property */
            wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Found primitive array at: %s\n", __func__, __LINE__, full_path);
            memset(&data_model_value, 0, sizeof(data_model_value));
            parse_property_constraints(items_eff, &data_model_value);

            tr181_path = yang_to_tr181_path(full_path);
            if (tr181_path) {
                char* parent_path = strdup(full_path);
                if (parent_path) {
                    char* last_dot = strrchr(parent_path, '.');
                    if (last_dot) {
                        *last_dot = '\0';  /* Truncate at last dot to get parent path */
                        
                        /* Register callback for parent object */
                        char* parent_tr181_path = yang_to_tr181_path(parent_path);
                        if (parent_tr181_path) {
                            callback_setter(parent_tr181_path, &cb_table);
                            /* Set NULL table handlers for primitive property callbacks */
                            cb_table.table_remove_row_handler = NULL;
                            cb_table.table_add_row_handler = NULL;
                            free(parent_tr181_path);
                        }
                    }
                    free(parent_path);
                }
                wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Registering property: %s (format=%d, permission=%d)\n", 
                        __func__, __LINE__, tr181_path, data_model_value.data_format, data_model_value.data_permission);
                bus_register_namespace(handle, tr181_path, bus_element_type_property, cb_table, data_model_value, 1);
                free(tr181_path);
            }
        }
        return;
    }

    /* 4) If type is object (but had no direct properties above),
       try to resolve any nested $ref and check again */
    if (schema_has_type(effective, "object")) {
        /* we've already tried follow_ref_if_any at top-level; if still no properties, treat as leaf object */
        wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Found leaf object at: %s\n", __func__, __LINE__, full_path);
        memset(&data_model_value, 0, sizeof(data_model_value));
        parse_property_constraints(effective, &data_model_value);
        data_model_value.data_format = bus_data_type_object;

        tr181_path = yang_to_tr181_path(full_path);
        if (tr181_path) {
            char* parent_path = strdup(full_path);
            if (parent_path) {
                char* last_dot = strrchr(parent_path, '.');
                if (last_dot) {
                    *last_dot = '\0';  /* Truncate at last dot to get parent path */
                    
                    /* Register callback for parent object */
                    char* parent_tr181_path = yang_to_tr181_path(parent_path);
                    if (parent_tr181_path) {
                        callback_setter(parent_tr181_path, &cb_table);
                        /* Set NULL table handlers for primitive property callbacks */
                        cb_table.table_remove_row_handler = NULL;
                        cb_table.table_add_row_handler = NULL;
                        free(parent_tr181_path);
                    }
                }
                free(parent_path);
            }
            wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Registering property: %s (format=%d, permission=%d)\n", 
                        __func__, __LINE__, tr181_path, data_model_value.data_format, data_model_value.data_permission);
            bus_register_namespace(handle, tr181_path, bus_element_type_property, cb_table, data_model_value, 1);
            free(tr181_path);
        }
        return;
    }

    /* 5) Fallback: primitive (string/number/boolean/enum) - register parent object callback */
    /* For primitive properties, register callback for the parent object, but keep full property path */
    memset(&data_model_value, 0, sizeof(data_model_value));
    parse_property_constraints(effective, &data_model_value);

    /* Extract parent object path by removing the last component for callback registration */
    char* parent_path = strdup(full_path);
    if (parent_path) {
        char* last_dot = strrchr(parent_path, '.');
        if (last_dot) {
            *last_dot = '\0';  /* Truncate at last dot to get parent path */

            /* Register callback for parent object */
            char* parent_tr181_path = yang_to_tr181_path(parent_path);
            if (parent_tr181_path) {
                callback_setter(parent_tr181_path, &cb_table);
                /* Set NULL table handlers for primitive property callbacks */
                cb_table.table_remove_row_handler = NULL;
                cb_table.table_add_row_handler = NULL;
                free(parent_tr181_path);
            }
        }
        free(parent_path);
    }

    /* Register the property itself with full path */
    tr181_path = yang_to_tr181_path(full_path);
    if (tr181_path) {
        wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Registering property: %s (format=%d, permission=%d)\n", 
                           __func__, __LINE__, tr181_path, data_model_value.data_format, data_model_value.data_permission);
        bus_register_namespace(handle, tr181_path, bus_element_type_property, cb_table, data_model_value, 1);
        free(tr181_path);
    }
}

/* Traverse object schema and process all "properties" */
static void traverse_schema(cJSON* root, cJSON* schema_node, const char* base_path, bus_handle_t *handle, bus_callback_setter_fn callback_setter)
{
    cJSON* effective = NULL;
    cJSON* props = NULL;
    cJSON* child = NULL;
    bus_name_string_t new_path = {0};

    if (!schema_node || !handle || !callback_setter) {
        return;
    }
    
    wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Traversing schema with base_path: %s\n", __func__, __LINE__, base_path ? base_path : "(null)");

    /* ensure we operate on resolved node (if schema_node is a wrapper with $ref) */
    effective = follow_ref_if_any(root, schema_node);
    if (!effective) {
        return;
    }

    props = cJSON_GetObjectItem(effective, "properties");
    if (!props || !cJSON_IsObject(props)) {
        wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: No properties found at: %s\n", __func__, __LINE__, base_path ? base_path : "(null)");
        return;
    }

    child = props->child;
    while (child) {
        if (child->string) {
            /* Handle NULL or empty base_path */
            if (base_path == NULL || strlen(base_path) == 0) {
                snprintf(new_path, sizeof(new_path), "%s", child->string);
            } else if (base_path[strlen(base_path) - 1] != '.') {
                snprintf(new_path, sizeof(new_path), "%s.%s", base_path, child->string);
            } else {
                snprintf(new_path, sizeof(new_path), "%s%s", base_path, child->string);
            }

            wifi_util_dbg_print(WIFI_DMCLI, "%s:%d: Processing property: %s\n", __func__, __LINE__, new_path);
            /* pass the child's schema node (not child->child) because child is a property pair */
            handle_property_node(root, new_path, child, handle, callback_setter);
        }
        child = child->next;
    }
}

/* Internal function to process parsed JSON schema */
static bool parse_and_register_schema_internal(bus_handle_t *handle, cJSON* root, const char* base_path,
    const char* search_key, bus_callback_setter_fn callback_setter)
{
    cJSON* props = NULL;
    cJSON* child = NULL;
    
    if (!handle || !root || !callback_setter) {
        return false;
    }
    
    /* Find element matching search key */
    props = cJSON_GetObjectItem(root, "properties");
    if (!props) {
        wifi_util_error_print(WIFI_DMCLI, "%s:%d: No properties found in schema root\n", __func__, __LINE__);
        return false;
    }
    
    wifi_util_info_print(WIFI_DMCLI, "%s:%d: Starting schema registration with base_path: %s, search_key: %s\n", 
                        __func__, __LINE__, base_path ? base_path : "(null)", search_key ? search_key : "(all)");
    
    child = props->child;
    while (child) {
        /* If search_key is NULL, process all children, otherwise find matching child */
        if (!search_key || (child->string && strstr(child->string, search_key) != NULL)) {
            traverse_schema(root, child, base_path, handle, callback_setter);
            if (search_key) {
                break;
            }
        }
        child = child->next;
    }
    
    return true;
}

/* Entry function to parse and register JSON schema from file */
static bool parse_json_schema_file(bus_handle_t *handle, const char *filename, const char *base_path, const char *search_key, bus_callback_setter_fn callback_setter)
{
    FILE* f = NULL;
    long size = 0;
    char* buf = NULL;
    size_t read_bytes = 0;
    cJSON* root = NULL;
    bool result = false;
    
    /* Load file */
    f = fopen(filename, "rb");
    if (!f) {
        wifi_util_error_print(WIFI_DMCLI, "%s:%d: Failed to open file: %s\n", __func__, __LINE__, filename);
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
    
    wifi_util_info_print(WIFI_DMCLI, "%s:%d: Loaded %zu bytes from %s\n", __func__, __LINE__, read_bytes, filename);
    
    root = cJSON_Parse(buf);
    free(buf);
    
    if (!root) {
        wifi_util_error_print(WIFI_DMCLI, "%s:%d: Failed to parse JSON file: %s\n", __func__, __LINE__, filename);
        return false;
    }
    
    /* Parse schema with provided parameters */
    result = parse_and_register_schema_internal(handle, root, base_path, search_key, callback_setter);
    
    cJSON_Delete(root);
    return result;
}

int parse_json_schema_and_register(bus_handle_t *handle, const char *json_schema_filename, 
                                   const char *base_path, const char *search_key,
                                   bus_callback_setter_fn callback_setter)
{
    bool result = false;
    
    if (!handle || !json_schema_filename || !callback_setter) {
        wifi_util_error_print(WIFI_DMCLI, "%s:%d: Invalid parameters\n", __func__, __LINE__);
        return RETURN_ERR;
    }
    
    if (!base_path || strlen(base_path) == 0) {
        wifi_util_info_print(WIFI_DMCLI, "%s:%d: Parsing JSON schema: %s with no base path\n", 
                            __func__, __LINE__, json_schema_filename);
    } else {
        wifi_util_info_print(WIFI_DMCLI, "%s:%d: Parsing JSON schema: %s with base path: %s\n", 
                            __func__, __LINE__, json_schema_filename, base_path);
    }
    
    result = parse_json_schema_file(handle, json_schema_filename, base_path, search_key, callback_setter);
    
    if (result) {
        wifi_util_info_print(WIFI_DMCLI, "%s:%d: Successfully parsed and registered schema\n", 
                            __func__, __LINE__);
        return RETURN_OK;
    } else {
        wifi_util_error_print(WIFI_DMCLI, "%s:%d: Failed to parse schema: %s\n", 
                             __func__, __LINE__, json_schema_filename);
        return RETURN_ERR;
    }
}

int parse_wfa_data_elements_schema(bus_handle_t *handle, const char *json_schema_filename)
{
    return parse_json_schema_and_register(handle, json_schema_filename, 
                                          "Device.WiFi.DataElements.Network", "Network",
                                          wfa_set_bus_callbackfunc_pointers);
}

int parse_native_dml_schema(bus_handle_t *handle, const char *json_schema_filename)
{
    // TODO depends on JSON format used for Native DM base_path may be "Device.WiFi"
    return parse_json_schema_and_register(handle, json_schema_filename, 
                                          NULL, NULL,
                                          set_bus_callbackfunc_pointers);
}
