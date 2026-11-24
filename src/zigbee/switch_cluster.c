#include "switch_cluster.h"
#include "base_components/relay.h"
#include "cluster_common.h"
#include "consts.h"
#include "device_config/nvm_items.h"
#include "hal/nvm.h"

#include "hal/printf_selector.h"
#include "hal/system.h"
#include "hal/tasks.h"
#include "relay_cluster.h"
#include "zigbee_commands.h"

const uint8_t multistate_out_of_service = 0;
const uint8_t multistate_flags          = 0;

static const uint16_t no_gesture             = 0;
static const uint8_t  single_click_supported = 1;

#define MULTISTATE_NOT_PRESSED     0
#define MULTISTATE_PRESS           1
#define MULTISTATE_LONG_PRESS      2
#define MULTISTATE_POSITION_ON     3
#define MULTISTATE_POSITION_OFF    4

// Match the #380 gesture event codes without changing the legacy edge codes.
static const uint16_t gesture_press[]          = { 5, 7, 10 };
static const uint16_t gesture_hold[]           = { MULTISTATE_LONG_PRESS, 8, 11 };
static const uint16_t gesture_release[]        = { 6, 9, 12 };
const uint16_t        multistate_num_of_states = 13;

#define MAX_GESTURE_PRESSES           3
#define DEFAULT_MAX_PRESS_COUNT       1
#define DEFAULT_CONFIRM_RELEASE_MS    200
#define MAX_CONFIRM_RELEASE_MS        2000
#define NO_ONOFF_COMMAND              0xff
#define SCENE_CONFIG_VERSION          2
#define SCENE_CONFIG_V1_SIZE          6

extern zigbee_relay_cluster relay_clusters[];
extern uint8_t relay_clusters_cnt;
extern zigbee_switch_cluster switch_clusters[];
extern uint8_t switch_clusters_cnt;

void switch_cluster_on_button_press(zigbee_switch_cluster *cluster);
void switch_cluster_on_button_release(zigbee_switch_cluster *cluster);
void switch_cluster_on_button_long_press(zigbee_switch_cluster *cluster);
static uint8_t switch_cluster_resolve_binding_on(const zigbee_switch_cluster *cluster);
static bool switch_cluster_send_binding_on(const zigbee_switch_cluster *cluster, uint8_t cmd_id);
static bool switch_cluster_has_valid_relay(
    const zigbee_switch_cluster *cluster);

zigbee_switch_cluster *switch_cluster_by_endpoint[10];

static void normalize_gesture_config(zigbee_switch_cluster *cluster) {
    if (cluster->max_press_count == 0) {
        cluster->max_press_count = DEFAULT_MAX_PRESS_COUNT;
    } else if (cluster->max_press_count > MAX_GESTURE_PRESSES) {
        cluster->max_press_count = MAX_GESTURE_PRESSES;
    }
    if (cluster->confirm_release_ms == 0) {
        cluster->confirm_release_ms = DEFAULT_CONFIRM_RELEASE_MS;
    } else if (cluster->confirm_release_ms > MAX_CONFIRM_RELEASE_MS) {
        cluster->confirm_release_ms = MAX_CONFIRM_RELEASE_MS;
    }
}

static void load_gesture_config(zigbee_switch_cluster *cluster) {
    uint8_t data[3];

    if (hal_nvm_read(NV_ITEM_SWITCH_GESTURE_CONFIG(cluster->switch_idx),
                     sizeof(data), data) == HAL_NVM_SUCCESS) {
        cluster->confirm_release_ms = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        cluster->max_press_count    = data[2];
    }
    normalize_gesture_config(cluster);
    cluster->saved_confirm_release_ms = cluster->confirm_release_ms;
    cluster->saved_max_press_count    = cluster->max_press_count;
}

static bool store_gesture_config(zigbee_switch_cluster *cluster) {
    uint8_t data[] = {
        (uint8_t)cluster->confirm_release_ms,
        (uint8_t)(cluster->confirm_release_ms >> 8),
        cluster->max_press_count,
    };

    if (hal_nvm_write(NV_ITEM_SWITCH_GESTURE_CONFIG(cluster->switch_idx),
                      sizeof(data), data) != HAL_NVM_SUCCESS) {
        printf("Switch %u: gesture config not saved\r\n", cluster->switch_idx);
        cluster->confirm_release_ms = cluster->saved_confirm_release_ms;
        cluster->max_press_count    = cluster->saved_max_press_count;
        return false;
    }
    cluster->saved_confirm_release_ms = cluster->confirm_release_ms;
    cluster->saved_max_press_count    = cluster->max_press_count;
    return true;
}

static void report_gesture(zigbee_switch_cluster *cluster, uint16_t value) {
    // Readback/periodic reports see zero; only explicit reports contain events.
    hal_zigbee_status_t status = hal_zigbee_send_report_attr(
        cluster->endpoint, ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
        ZCL_ATTR_MULTISTATE_INPUT_SWITCH_GESTURE,
        ZCL_DATA_TYPE_UINT16, &value, sizeof(value));

    if (status != HAL_ZIGBEE_OK && status != HAL_ZIGBEE_ERR_NOT_JOINED) {
        printf("Switch %u: gesture report failed (%u)\r\n", cluster->switch_idx, status);
    }
}

static uint32_t binding_config(const zigbee_switch_cluster *cluster) {
    return (uint32_t)cluster->binded_mode | ((uint32_t)cluster->action << 8) |
           ((uint32_t)cluster->relay_index << 16) | ((uint32_t)cluster->relay_mode << 24);
}

static bool single_click_binding_is_current(const zigbee_switch_cluster *cluster) {
    return cluster->single_click_binding_armed &&
           cluster->single_click_binding_config == binding_config(cluster);
}

static bool same_scenes(const switch_scene_config *a, const switch_scene_config *b) {
    return a->group_id == b->group_id && a->enabled == b->enabled &&
           a->double_id == b->double_id && a->triple_id == b->triple_id &&
           a->single_enabled == b->single_enabled && a->single_id == b->single_id;
}

static bool valid_scenes(const switch_scene_config *config) {
    return config->enabled <= 1 && config->single_enabled <= 1 &&
           config->group_id <= ZCL_MAX_GROUP_ID &&
           (!config->enabled || config->group_id != 0);
}

static void load_scenes(zigbee_switch_cluster *cluster) {
    uint8_t data[8];
    uint8_t item = NV_ITEM_SWITCH_SCENE_CONFIG(cluster->switch_idx);

    cluster->scenes = (switch_scene_config) { .double_id = 1, .triple_id = 2 };
    hal_nvm_status_t status  = hal_nvm_read(item, sizeof(data), data);
    bool             current = status == HAL_NVM_SUCCESS && data[0] == SCENE_CONFIG_VERSION;
    // Telink reports a different record length as NOT_FOUND, not a read error.
    bool legacy = status == HAL_NVM_NOT_FOUND &&
                  hal_nvm_read(item, SCENE_CONFIG_V1_SIZE, data) == HAL_NVM_SUCCESS &&
                  data[0] == 1;

    if (current || legacy) {
        switch_scene_config config = {
            .enabled        = data[1],
            .group_id       = (uint16_t)data[2] | ((uint16_t)data[3] << 8),
            .double_id      = data[4],
            .triple_id      = data[5],
            .single_enabled = current ? data[6] : 0,
            .single_id      = current ? data[7] : 0,
        };
        if (valid_scenes(&config)) {
            cluster->scenes = config;
        }
    }
    cluster->saved_scenes = cluster->scenes;
}

static void store_scenes(zigbee_switch_cluster *cluster) {
    const switch_scene_config *config = &cluster->scenes;

    if (same_scenes(config, &cluster->saved_scenes)) {
        return;
    }
    if (valid_scenes(config)) {
        uint8_t data[] = {
            SCENE_CONFIG_VERSION,      config->enabled,
            (uint8_t)config->group_id, (uint8_t)(config->group_id >> 8),
            config->double_id,         config->triple_id,
            config->single_enabled,    config->single_id,
        };
        if (hal_nvm_write(NV_ITEM_SWITCH_SCENE_CONFIG(cluster->switch_idx), sizeof(data),
                          data) == HAL_NVM_SUCCESS) {
            cluster->saved_scenes = cluster->scenes;
            cluster->scene_armed  = false;
            return;
        }
    }
    cluster->scenes = cluster->saved_scenes;
    printf("Switch %u: scene config not saved\r\n", cluster->switch_idx);
}

static void recall_gesture_scene(zigbee_switch_cluster *cluster) {
    if (!cluster->scene_armed || !single_click_binding_is_current(cluster) ||
        cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE ||
        !same_scenes(&cluster->scenes, &cluster->saved_scenes) ||
        cluster->press_count == 0 || cluster->press_count > cluster->max_press_count) {
        return;
    }

    const switch_scene_config *config = &cluster->scenes;
    uint8_t scene_id;
    switch (cluster->press_count) {
    case 1:
        if (!config->single_enabled) {
            return;
        }
        scene_id = config->single_id;
        break;
    case 2:
        scene_id = config->double_id;
        break;
    case 3:
        scene_id = config->triple_id;
        break;
    default:
        return;
    }
    uint8_t        payload[5];
    hal_zigbee_cmd cmd = build_recall_scene_cmd(cluster->endpoint, config->group_id,
                                                scene_id, payload);
    hal_zigbee_status_t status = hal_zigbee_send_to_bound_group(&cmd, config->group_id);

    if (status != HAL_ZIGBEE_OK) {
        printf("Switch %u: scene recall not sent (%u)\r\n", cluster->switch_idx, status);
    }
}

static void confirm_press(void *arg) {
    zigbee_switch_cluster *cluster = arg;

    if (cluster->press_count != 0) {
        // Recheck the live configuration even if the platform did not issue
        // an attribute callback (e.g. Telink Write Attributes Undivided).
        if (cluster->press_count == 1 && single_click_binding_is_current(cluster)) {
            bool has_onoff_binding = hal_zigbee_has_binding(cluster->endpoint, ZCL_CLUSTER_ON_OFF);
            // With no On/Off target, the scene can run on its own.
            bool ready = !has_onoff_binding ||
                         switch_cluster_send_binding_on(cluster, cluster->single_click_command);
            if (ready && cluster->single_click_command == ZCL_CMD_ONOFF_TOGGLE) {
                recall_gesture_scene(cluster);
            }
        } else if (cluster->press_count > 1) {
            recall_gesture_scene(cluster);
        }
        report_gesture(cluster, gesture_press[cluster->press_count - 1]);
        cluster->press_count = 0;
        cluster->single_click_binding_armed = false;
        cluster->scene_armed = false;
    }
}

static void cancel_press_sequence(zigbee_switch_cluster *cluster) {
    hal_tasks_unschedule(&cluster->confirm_press_task);
    cluster->press_count = 0;
    cluster->single_click_binding_armed = false;
    cluster->scene_armed = false;
}

static void sync_switch_indicator_led(zigbee_switch_cluster *cluster) {
    if (cluster->indicator_led == NULL) {
        return;
    }

    if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED &&
        switch_cluster_has_valid_relay(cluster)) {
        return;
    }

    led_off(cluster->indicator_led);
}

void update_switch_clusters() {
    for (int i = 0; i < switch_clusters_cnt; i++) {
        sync_switch_indicator_led(&switch_clusters[i]);
    }
}

static bool switch_cluster_has_valid_relay(const zigbee_switch_cluster *cluster) {
    return cluster->relay_index > 0 && cluster->relay_index <= relay_clusters_cnt;
}

static void switch_cluster_flash_indicator(zigbee_switch_cluster *cluster) {
    if (cluster->indicator_led == NULL) {
        return;
    }
    // Skip flash when relay is attached — the relay toggle itself changes the
    // indicator, and the blink would race with sync_indicator_led.
    if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED &&
        switch_cluster_has_valid_relay(cluster)) {
        return;
    }
    // Only flash when LED is idle (not in "not connected" forever-blink)
    if (cluster->indicator_led->blink_times_left == 0) {
        led_blink(cluster->indicator_led, 50, 50, 1);
    }
}

void switch_cluster_store_attrs_to_nv(zigbee_switch_cluster *cluster);
void switch_cluster_load_attrs_from_nv(zigbee_switch_cluster *cluster);
void switch_cluster_on_write_attr(zigbee_switch_cluster *cluster,
                                  uint16_t attribute_id);

void switch_cluster_report_action(zigbee_switch_cluster *cluster);

void switch_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                   uint16_t attribute_id) {
    switch_cluster_on_write_attr(switch_cluster_by_endpoint[endpoint],
                                 attribute_id);
}

void switch_cluster_add_scenes(zigbee_switch_cluster *cluster,
                               hal_zigbee_endpoint *endpoint) {
    hal_zigbee_cluster *cfg = hal_zigbee_find_cluster(endpoint, 1, endpoint->endpoint,
                                                      ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG);

    load_scenes(cluster);
    SETUP_ATTR(11, ZCL_ATTR_SWITCH_SCENES_ENABLED, ZCL_DATA_TYPE_BOOLEAN,
               ATTR_WRITABLE, cluster->scenes.enabled);
    SETUP_ATTR(12, ZCL_ATTR_SWITCH_SCENES_GROUP_ID, ZCL_DATA_TYPE_UINT16,
               ATTR_WRITABLE, cluster->scenes.group_id);
    SETUP_ATTR(13, ZCL_ATTR_SWITCH_SCENES_DOUBLE_ID, ZCL_DATA_TYPE_UINT8,
               ATTR_WRITABLE, cluster->scenes.double_id);
    SETUP_ATTR(14, ZCL_ATTR_SWITCH_SCENES_TRIPLE_ID, ZCL_DATA_TYPE_UINT8,
               ATTR_WRITABLE, cluster->scenes.triple_id);
    SETUP_ATTR(15, ZCL_ATTR_SWITCH_SCENES_SINGLE_ENABLED, ZCL_DATA_TYPE_BOOLEAN,
               ATTR_WRITABLE, cluster->scenes.single_enabled);
    SETUP_ATTR(16, ZCL_ATTR_SWITCH_SCENES_SINGLE_ID, ZCL_DATA_TYPE_UINT8,
               ATTR_WRITABLE, cluster->scenes.single_id);
    cfg->attribute_count += SWITCH_SCENE_ATTR_COUNT;
    endpoint->clusters[endpoint->cluster_count++] = (hal_zigbee_cluster) {
        .cluster_id = ZCL_CLUSTER_SCENES, .is_server = 0,
    };
    cluster->scene_client = true;
}

void switch_cluster_add_to_endpoint(zigbee_switch_cluster *cluster,
                                    hal_zigbee_endpoint *endpoint) {
    switch_cluster_by_endpoint[endpoint->endpoint] = cluster;
    cluster->endpoint = endpoint->endpoint;
    switch_cluster_load_attrs_from_nv(cluster);
    load_gesture_config(cluster);
    cluster->confirm_press_task.handler = confirm_press;
    cluster->confirm_press_task.arg     = cluster;
    hal_tasks_init(&cluster->confirm_press_task);

    cluster->button->on_press =
        (ev_button_callback_t)switch_cluster_on_button_press;
    cluster->button->on_release =
        (ev_button_callback_t)switch_cluster_on_button_release;
    cluster->button->on_long_press =
        (ev_button_callback_t)switch_cluster_on_button_long_press;
    cluster->button->callback_param = cluster;

    SETUP_ATTR(0, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_TYPE, ZCL_DATA_TYPE_ENUM8,
               ATTR_READONLY, cluster->mode);
    SETUP_ATTR(1, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_ACTIONS,
               ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE, cluster->action);
    SETUP_ATTR(2, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE, ZCL_DATA_TYPE_ENUM8,
               ATTR_WRITABLE, cluster->mode);
    SETUP_ATTR(3, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_MODE,
               ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE, cluster->relay_mode);
    SETUP_ATTR(4, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_INDEX,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->relay_index);
    SETUP_ATTR(5, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LONG_PRESS_DUR,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
               cluster->button->long_press_duration_ms);
    SETUP_ATTR(6, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_LEVEL_MOVE_RATE,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->level_move_rate);
    SETUP_ATTR(7, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_BINDING_MODE,
               ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE, cluster->binded_mode);
    SETUP_ATTR(8, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MAX_PRESS_COUNT,
               ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->max_press_count);
    SETUP_ATTR(9, ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_CONFIRM_RELEASE_MS,
               ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE, cluster->confirm_release_ms);
    SETUP_ATTR(10, ZCL_ATTR_ONOFF_CONFIGURATION_SINGLE_CLICK_SUPPORTED,
               ZCL_DATA_TYPE_BOOLEAN, ATTR_READONLY, single_click_supported);

    // Configuration
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 11;
    endpoint->clusters[endpoint->cluster_count].attributes      = cluster->attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server       = 1;
    endpoint->cluster_count++;

    // Output ON OFF to bind to other devices
    endpoint->clusters[endpoint->cluster_count].cluster_id      = ZCL_CLUSTER_ON_OFF;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 0;
    endpoint->clusters[endpoint->cluster_count].attributes      = NULL;
    endpoint->clusters[endpoint->cluster_count].is_server       = 0;
    endpoint->cluster_count++;

    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 0,
                         ZCL_ATTR_MULTISTATE_INPUT_NUMBER_OF_STATES,
                         ZCL_DATA_TYPE_UINT16, ATTR_READONLY,
                         multistate_num_of_states);
    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 1,
                         ZCL_ATTR_MULTISTATE_INPUT_OUT_OF_SERVICE,
                         ZCL_DATA_TYPE_BOOLEAN, ATTR_READONLY,
                         multistate_out_of_service);
    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 2,
                         ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE,
                         ZCL_DATA_TYPE_UINT16, ATTR_READONLY,
                         cluster->multistate_state);
    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 3,
                         ZCL_ATTR_MULTISTATE_INPUT_STATUS_FLAGS,
                         ZCL_DATA_TYPE_BITMAP8, ATTR_READONLY, multistate_flags);
    SETUP_ATTR_FOR_TABLE(cluster->multistate_attr_infos, 4,
                         ZCL_ATTR_MULTISTATE_INPUT_SWITCH_GESTURE,
                         ZCL_DATA_TYPE_UINT16, ATTR_READONLY, no_gesture);

    // Output
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 5;
    endpoint->clusters[endpoint->cluster_count].attributes      =
        cluster->multistate_attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server = 1;
    endpoint->cluster_count++;

    // Output Level for other devices
    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_LEVEL_CONTROL;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 0;
    endpoint->clusters[endpoint->cluster_count].attributes      = NULL;
    endpoint->clusters[endpoint->cluster_count].is_server       = 0;
    endpoint->cluster_count++;
}

// Perform the relay action for ON position (position 1 in ZCL docs)
void switch_cluster_relay_action_on(zigbee_switch_cluster *cluster) {
    if (!switch_cluster_has_valid_relay(cluster))
        return;

    zigbee_relay_cluster *relay_cluster =
        &relay_clusters[cluster->relay_index - 1];

    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        relay_cluster_on(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        relay_cluster_off(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        relay_cluster_toggle(relay_cluster);
        break;
    }
}

// Perform the relay action for OFF position (position 2 in ZCL docs)
void switch_cluster_relay_action_off(zigbee_switch_cluster *cluster) {
    if (!switch_cluster_has_valid_relay(cluster))
        return;

    zigbee_relay_cluster *relay_cluster =
        &relay_clusters[cluster->relay_index - 1];

    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        relay_cluster_off(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        relay_cluster_on(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
        relay_cluster_toggle(relay_cluster);
        break;
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        relay_cluster_toggle(relay_cluster);
        break;
    }
}

static uint8_t switch_cluster_resolve_binding_on(const zigbee_switch_cluster *cluster) {
    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        return ZCL_CMD_ONOFF_ON;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        return ZCL_CMD_ONOFF_OFF;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        return ZCL_CMD_ONOFF_TOGGLE;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        if (switch_cluster_has_valid_relay(cluster)) {
            bool relay_on = relay_clusters[cluster->relay_index - 1].relay->on;
            bool sync     = cluster->action ==
                            ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC;
            return relay_on == sync ? ZCL_CMD_ONOFF_ON : ZCL_CMD_ONOFF_OFF;
        }
        return ZCL_CMD_ONOFF_TOGGLE;

    default:
        return NO_ONOFF_COMMAND;
    }
}

static bool switch_cluster_send_binding_on(const zigbee_switch_cluster *cluster, uint8_t cmd_id) {
    if (cmd_id == NO_ONOFF_COMMAND ||
        hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return false;
    }
    hal_zigbee_cmd c = build_onoff_cmd(cluster->endpoint, cmd_id);
    return hal_zigbee_send_cmd_to_bindings(&c) == HAL_ZIGBEE_OK;
}

// Send OnOff command based on ON position (position 1 in ZCL docs).
static void switch_cluster_binding_action_on(zigbee_switch_cluster *cluster) {
    switch_cluster_send_binding_on(cluster, switch_cluster_resolve_binding_on(cluster));
}

// Send OnOff command to binded device based on OFF position (position 2 in
// ZCL docs)
void switch_cluster_binding_action_off(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    uint8_t cmd_id;

    switch (cluster->action) {
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF:
        cmd_id = ZCL_CMD_ONOFF_OFF;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_OFFON:
        cmd_id = ZCL_CMD_ONOFF_ON;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE:
        cmd_id = ZCL_CMD_ONOFF_TOGGLE;
        break;

    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC:
    case ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_OPPOSITE:
        if (!switch_cluster_has_valid_relay(cluster)) {
            cmd_id = ZCL_CMD_ONOFF_TOGGLE;
        } else {
            zigbee_relay_cluster *relay_cluster =
                &relay_clusters[cluster->relay_index - 1];
            if (cluster->action ==
                ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SMART_SYNC)
                cmd_id = (relay_cluster->relay->on) ? ZCL_CMD_ONOFF_ON
                                                    : ZCL_CMD_ONOFF_OFF;
            else
                cmd_id = (relay_cluster->relay->on) ? ZCL_CMD_ONOFF_OFF
                                                    : ZCL_CMD_ONOFF_ON;
        }
        break;

    default:
        return;
    }

    hal_zigbee_cmd c = build_onoff_cmd(cluster->endpoint, cmd_id);
    hal_zigbee_send_cmd_to_bindings(&c);
}

void switch_cluster_level_stop(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    hal_zigbee_cmd c = build_level_stop_onoff_cmd(cluster->endpoint);
    hal_zigbee_send_cmd_to_bindings(&c);
}

void switch_cluster_level_control(zigbee_switch_cluster *cluster) {
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        return;
    }

    hal_zigbee_cmd c = build_level_move_onoff_cmd(cluster->endpoint,
                                                  cluster->level_move_direction,
                                                  cluster->level_move_rate);
    hal_zigbee_send_cmd_to_bindings(&c);

    if (cluster->level_move_direction == ZCL_LEVEL_MOVE_DOWN) {
        cluster->level_move_direction = ZCL_LEVEL_MOVE_UP;
    } else {
        cluster->level_move_direction = ZCL_LEVEL_MOVE_DOWN;
    }
}

void switch_cluster_on_button_press(zigbee_switch_cluster *cluster) {
    switch_cluster_flash_indicator(cluster);

    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Toggle does not support modes (RISE, SHORT, LONG)
        if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED) {
            switch_cluster_relay_action_on(cluster);
        }
        switch_cluster_binding_action_on(cluster);
        cluster->multistate_state = MULTISTATE_POSITION_ON;
        report_gesture(cluster, MULTISTATE_POSITION_ON);
        hal_zigbee_notify_attribute_changed(
            cluster->endpoint, ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
            ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
        return;
    }

    hal_tasks_unschedule(&cluster->confirm_press_task);
    if (cluster->press_count == 0) {
        cluster->single_click_binding_armed =
            cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SINGLE_CLICK;
        cluster->single_click_binding_config = binding_config(cluster);
        cluster->single_click_command        = NO_ONOFF_COMMAND;
        cluster->scene_armed = cluster->scene_client && cluster->saved_scenes.enabled &&
                               cluster->single_click_binding_armed;
    }
    if (cluster->press_count < cluster->max_press_count) {
        cluster->press_count++;
    }

    if (cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_RISE) {
        switch_cluster_relay_action_on(cluster);
    }

    if (cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_RISE) {
        switch_cluster_binding_action_on(cluster);
    }

    cluster->multistate_state = MULTISTATE_PRESS;
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
}

void switch_cluster_on_button_release(zigbee_switch_cluster *cluster) {
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Only flash on release for toggles,
        // for momentary flash on press only
        switch_cluster_flash_indicator(cluster);
    }

    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Toggle does not support modes (RISE, SHORT, LONG)
        if (cluster->relay_mode != ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED) {
            switch_cluster_relay_action_off(cluster);
        }
        switch_cluster_binding_action_off(cluster);
        cluster->multistate_state = MULTISTATE_POSITION_OFF;
        report_gesture(cluster, MULTISTATE_POSITION_OFF);
        hal_zigbee_notify_attribute_changed(
            cluster->endpoint, ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
            ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
        return;
    }

    unsigned hold_idx = 0;
    while (hold_idx < MAX_GESTURE_PRESSES &&
           gesture_hold[hold_idx] != cluster->multistate_state) {
        hold_idx++;
    }
    if (hold_idx == MAX_GESTURE_PRESSES) {
        if (cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_SHORT) {
            switch_cluster_relay_action_on(cluster);
        }
        if (cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT) {
            switch_cluster_binding_action_on(cluster);
        } else if (cluster->press_count == 1 && single_click_binding_is_current(cluster)) {
            // Resolve after the local relay action, just as SHORT does. The
            // bound command must not depend on relay changes during the wait.
            cluster->single_click_command = switch_cluster_resolve_binding_on(cluster);
        }
    } else {
        // This is end of long press, send zcl_level stop
        switch_cluster_level_stop(cluster);
        report_gesture(cluster, gesture_release[hold_idx]);
        cluster->press_count = 0;
        cluster->single_click_binding_armed = false;
        cluster->scene_armed = false;
    }

    cluster->multistate_state = MULTISTATE_NOT_PRESSED;
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);

    if (cluster->press_count > cluster->max_press_count) {
        cluster->press_count = cluster->max_press_count;
    }
    if (cluster->press_count != 0) {
        if (cluster->max_press_count == 1) {
            confirm_press(cluster);
        } else if (!hal_tasks_try_schedule(&cluster->confirm_press_task,
                                           cluster->confirm_release_ms)) {
            // Don't emit an early single or merge this sequence into a later one.
            cluster->press_count = 0;
            cluster->single_click_binding_armed = false;
            cluster->scene_armed = false;
            printf("Switch %u: gesture timer unavailable\r\n", cluster->switch_idx);
        }
    }
}

void switch_cluster_on_button_long_press(zigbee_switch_cluster *cluster) {
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        // Toggle does not support modes (RISE, SHORT, LONG)
        return;
    }

    if (cluster->relay_mode == ZCL_ONOFF_CONFIGURATION_RELAY_MODE_LONG) {
        if (switch_cluster_has_valid_relay(cluster)) {
            relay_cluster_toggle(&relay_clusters[cluster->relay_index - 1]);
        }
    }

    if (cluster->binded_mode == ZCL_ONOFF_CONFIGURATION_BINDED_MODE_LONG) {
        switch_cluster_binding_action_on(cluster);
    }

    switch_cluster_level_control(cluster);

    uint8_t count = cluster->press_count;
    if (count > cluster->max_press_count) {
        count = cluster->max_press_count;
    }
    cluster->multistate_state = gesture_hold[count != 0 ? count - 1 : 0];
    report_gesture(cluster, cluster->multistate_state);
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
}

void synchronize_multistate_state(zigbee_switch_cluster *cluster) {
    if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE) {
        if (cluster->button->pressed) {
            cluster->multistate_state = MULTISTATE_POSITION_ON;
        } else {
            cluster->multistate_state = MULTISTATE_POSITION_OFF;
        }
    } else {
        if (cluster->button->long_pressed) {
            cluster->multistate_state = MULTISTATE_LONG_PRESS;
        } else if (cluster->button->pressed) {
            cluster->multistate_state = MULTISTATE_PRESS;
        } else {
            cluster->multistate_state = MULTISTATE_NOT_PRESSED;
        }
    }
    hal_zigbee_notify_attribute_changed(cluster->endpoint,
                                        ZCL_CLUSTER_MULTISTATE_INPUT_BASIC,
                                        ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE);
}

void switch_cluster_on_write_attr(zigbee_switch_cluster *cluster,
                                  uint16_t attribute_id) {
    if (attribute_id >= ZCL_ATTR_SWITCH_SCENES_ENABLED &&
        attribute_id <= ZCL_ATTR_SWITCH_SCENES_SINGLE_ID) {
        if (cluster->scene_client) {
            store_scenes(cluster);
        }
        return;
    }
    if (attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MAX_PRESS_COUNT ||
        attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_CONFIRM_RELEASE_MS) {
        normalize_gesture_config(cluster);
        if (cluster->confirm_release_ms == cluster->saved_confirm_release_ms &&
            cluster->max_press_count == cluster->saved_max_press_count) {
            return;
        }
        if (store_gesture_config(cluster)) {
            cluster->single_click_binding_armed = false;
            cluster->scene_armed = false;
            if (!cluster->button->pressed) {
                cancel_press_sequence(cluster);
            }
        }
        return;
    }
    printf("Index at write attr: %d\r\n", cluster->switch_idx);
    if (attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_RELAY_INDEX) {
        if (relay_clusters_cnt == 0) {
            cluster->relay_index = 0;
        } else if (cluster->relay_index < 1 || cluster->relay_index > relay_clusters_cnt) {
            cluster->relay_index = 1;
        }
    }
    if (!single_click_binding_is_current(cluster)) {
        cluster->single_click_binding_armed = false;
    }
    if (attribute_id == ZCL_ATTR_ONOFF_CONFIGURATION_SWITCH_MODE) {
        cancel_press_sequence(cluster);
        synchronize_multistate_state(cluster);
        if (cluster->mode == ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY_NC) {
            cluster->button->pressed_when_high = 1;
        } else {
            cluster->button->pressed_when_high = 0;
        }
    }
    switch_cluster_store_attrs_to_nv(cluster);
}

zigbee_switch_cluster_config nv_config_buffer;

void switch_cluster_store_attrs_to_nv(zigbee_switch_cluster *cluster) {
    nv_config_buffer.action      = cluster->action;
    nv_config_buffer.mode        = cluster->mode;
    nv_config_buffer.relay_index = cluster->relay_index;
    nv_config_buffer.relay_mode  = cluster->relay_mode;
    nv_config_buffer.button_long_press_duration =
        cluster->button->long_press_duration_ms;
    nv_config_buffer.level_move_rate = cluster->level_move_rate;
    nv_config_buffer.binded_mode     = cluster->binded_mode;
    hal_nvm_write(NV_ITEM_SWITCH_CLUSTER_DATA(cluster->switch_idx),
                  sizeof(zigbee_switch_cluster_config),
                  (uint8_t *)&nv_config_buffer);
}

void switch_cluster_load_attrs_from_nv(zigbee_switch_cluster *cluster) {
    hal_nvm_status_t st = hal_nvm_read(
        NV_ITEM_SWITCH_CLUSTER_DATA(cluster->switch_idx),
        sizeof(zigbee_switch_cluster_config), (uint8_t *)&nv_config_buffer);

    if (st != HAL_NVM_SUCCESS) {
        printf("No switch config in NV, using defaults\r\n");
        return;
    }
    cluster->action      = nv_config_buffer.action;
    cluster->mode        = nv_config_buffer.mode;
    cluster->relay_index = nv_config_buffer.relay_index;
    cluster->relay_mode  = nv_config_buffer.relay_mode;
    cluster->button->long_press_duration_ms =
        nv_config_buffer.button_long_press_duration;
    cluster->level_move_rate = nv_config_buffer.level_move_rate;
    cluster->binded_mode     = nv_config_buffer.binded_mode;

    // Validate relay_index to prevent out-of-bounds access
    if (relay_clusters_cnt == 0) {
        cluster->relay_index = 0;
    } else if (cluster->relay_index < 1 || cluster->relay_index > relay_clusters_cnt) {
        printf("Invalid relay_index %d in NV, resetting to default\r\n",
               cluster->relay_index);
        cluster->relay_index = cluster->switch_idx + 1;
    }
}
