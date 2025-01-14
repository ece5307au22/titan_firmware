#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/async_i2c.h"

#include <rcl/rcl.h>
#include <rclc_parameter/rclc_parameter.h>

#include "actuator_i2c/interface.h"
#include "basic_logger/logging.h"

#include "hw/actuator.h"
#include "drivers/safety.h"

#undef LOGGING_UNIT_NAME
#define LOGGING_UNIT_NAME "actuator_interface"

#define ACTUATOR_MAX_COMMANDS 8
#define ACTUATOR_I2C_BUS SENSOR_I2C_HW

#define ACTUATOR_POLLING_RATE_MS 300
#define ACTUATOR_MAX_STATUS_AGE_MS 1000

// ========================================
// I2C Command Generation/Processing
// ========================================

struct actuator_command_data;
// If response_cb returns true, then it will not free the request
typedef bool (*actuator_cmd_response_cb_t)(struct actuator_command_data*);
typedef struct actuator_command_data {
    actuator_i2c_cmd_t request;
    actuator_i2c_response_t response;
    struct async_i2c_request i2c_request;
    bool i2c_in_progress;
    actuator_cmd_response_cb_t response_cb;
    bool important_request;     // Set to true if the command being lost should be a fault
    bool in_use;
} actuator_cmd_data_t;

/**
 * @brief Common callback for completion of actuator i2c request
 * This checks the crc value of the response if there is one, and will call the request callback if one provided on successful completion
 * This also handles releasing of the request
 *
 * @param req
 */
static void actuator_command_done(__unused const struct async_i2c_request * req) {
    actuator_cmd_data_t* cmd = (actuator_cmd_data_t*)req->user_data;

    bool can_release_request = true;
    bool request_successful = true;
    if (cmd->i2c_request.bytes_to_receive > 0) {
        uint8_t crc_calc = actuator_i2c_crc8_calc_response(&cmd->response, cmd->i2c_request.bytes_to_receive);
        if (crc_calc != cmd->response.crc8) {
            LOG_WARN("CRC Mismatch: 0x%02x calculated, 0x%02x received", crc_calc, cmd->response.crc8)
            if (cmd->important_request) {
                safety_raise_fault(FAULT_ACTUATOR_FAIL);
            }

            request_successful = false;
        }
    }

    if (request_successful && cmd->response_cb){
        if (cmd->response_cb(cmd)){
            can_release_request = false;
        }
    }

    if (can_release_request) {
        cmd->in_use = false;
    }
}

/**
 * @brief Common failure callback for actuator i2c requests
 *
 * @param req
 * @param abort_data
 */
static void actuator_command_failed(__unused const struct async_i2c_request * req, uint32_t abort_data){
    actuator_cmd_data_t* cmd = (actuator_cmd_data_t*)req->user_data;
    if (cmd->important_request) {
        LOG_ERROR("Failed to send important actuator command %d: Abort Data 0x%x", cmd->request.cmd_id, abort_data);
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
    } else {
        LOG_WARN("Failed to send actuator command %d: Abort Data 0x%x", cmd->request.cmd_id, abort_data);
    }
    cmd->in_use = false;
}

/**
 * @brief Populates cmd with the required elements for an actuator i2c command
 *
 * @param cmd The command id for the request
 * @param cmd_id The callback to call on a successful response from the actuators. Can be NULL if no response is needed
 * @param response_cb The callback to call on a successful response from the actuators. Can be NULL if no response is needed
 * @param important If a failure of this command should be made immediately known
 */
static void actuator_populate_command(actuator_cmd_data_t* cmd, enum actuator_command cmd_id, actuator_cmd_response_cb_t response_cb, bool important) {
    cmd->i2c_request.i2c = ACTUATOR_I2C_BUS;
    cmd->i2c_request.address = ACTUATOR_I2C_ADDR;
    cmd->i2c_request.nostop = false;
    cmd->i2c_request.tx_buffer = (uint8_t*)(&cmd->request);
    cmd->i2c_request.rx_buffer = (uint8_t*)(&cmd->response);
    cmd->i2c_request.bytes_to_send = ACTUATOR_GET_CMD_SIZE(cmd_id);
    cmd->i2c_request.bytes_to_receive = ACTUATOR_GET_RESPONSE_SIZE(cmd_id);
    cmd->i2c_request.completed_callback = actuator_command_done;
    cmd->i2c_request.failed_callback = actuator_command_failed;
    cmd->i2c_request.next_req_on_success = NULL;
    cmd->i2c_request.user_data = cmd;

    cmd->i2c_in_progress = false;
    cmd->response_cb = response_cb;

    cmd->request.cmd_id = cmd_id;
    cmd->important_request = important;
}


static actuator_cmd_data_t *allocated_commands[ACTUATOR_MAX_COMMANDS];
static int num_allocated_commands = 0;
/**
 * @brief Generates a command with the specified command id and response callback.
 * CAN RETURN NULL IF UNABLE TO GET A REQUEST
 * IT IS THE CALLERS RESPONSIBILITY TO HANDLE THIS
 *
 * NOT INTERRUPT SAFE
 *
 * @param cmd_id The command id for the request
 * @param response_cb The callback to call on a successful response from the actuators. Can be NULL if no response is needed
 * @return actuator_cmd_data_t* The command allocated or NULL if a command could not be generated
 */
static actuator_cmd_data_t* actuator_generate_command(enum actuator_command cmd_id, actuator_cmd_response_cb_t response_cb) {
    actuator_cmd_data_t * cmd = NULL;
    // Skip the static allocation to save for non-interrupt safe requests
    for (int i = 0; i < num_allocated_commands; i++) {
        if (!allocated_commands[i]->in_use) {
            allocated_commands[i]->in_use = true;
            cmd = allocated_commands[i];
        }
    }
    if (!cmd && num_allocated_commands < ACTUATOR_MAX_COMMANDS) {
        cmd = malloc(sizeof(*cmd));
        cmd->in_use = true;
        allocated_commands[num_allocated_commands++] = cmd;
    }

    if (cmd) {
        actuator_populate_command(cmd, cmd_id, response_cb, true);
    }

    return cmd;
}

/**
 * @brief Calculates the crc for the command and sends it
 * If not using `actuator_generate_command` to allocate the request it is the responsibility of the caller to set in_use to true before sending.
 *
 * @param cmd The command to send
 */
static void actuator_send_command(actuator_cmd_data_t * cmd) {
    cmd->request.crc8 = actuator_i2c_crc8_calc_command(&cmd->request, cmd->i2c_request.bytes_to_send);

    async_i2c_enqueue(&cmd->i2c_request, &cmd->i2c_in_progress);
}

// ========================================
// Timing Management
// ========================================
struct timing_entry {
    uint16_t timing;
    bool set;
};
// The various timing_entry variables contain the cached timing values sent from ROS
// These will be sent to the actuator board whenever it needs timings
// In the event of a watchdog reset on the actuator board, it will request the timings and they will be sent from here
static struct timing_entry torpedo1_timings[ACTUATOR_NUM_TORPEDO_TIMINGS] = {0};
static struct timing_entry torpedo2_timings[ACTUATOR_NUM_TORPEDO_TIMINGS] = {0};
static struct timing_entry claw_timing = {0};
static struct timing_entry dropper_active_timing = {0};

static actuator_cmd_data_t set_timing_command = {.in_use = false, .i2c_in_progress = false};
// The missing_timings contains timings that need to be sent to the actuator board
// This will be set by the status message, but can also be set on parameter change
// Values can only be set to false on successful sending of message
static struct missing_timings_status missing_timings;


static bool actuator_set_timing_general_cb(actuator_cmd_data_t *cmd);
static bool actuator_update_missing_timings_common(actuator_cmd_data_t *cmd) {
    if ((missing_timings.claw_open_timing || missing_timings.claw_close_timing) && claw_timing.set) {
        actuator_populate_command(cmd, ACTUATOR_CMD_CLAW_TIMING, actuator_set_timing_general_cb, true);
        cmd->request.data.claw_timing.open_time_ms = claw_timing.timing;
        cmd->request.data.claw_timing.close_time_ms = claw_timing.timing;

        missing_timings.claw_open_timing = false;
        missing_timings.claw_close_timing = false;
    }
    else if (missing_timings.dropper_active_timing && dropper_active_timing.set) {
        actuator_populate_command(cmd, ACTUATOR_CMD_DROPPER_TIMING, actuator_set_timing_general_cb, true);
        cmd->request.data.dropper_timing.active_time_ms = dropper_active_timing.timing;

        missing_timings.dropper_active_timing = false;
    }
    #define ELSE_IF_TORPEDO_TIMING_NEEDED(torp_num, coil_lower, coil_upper) \
        else if (missing_timings.torpedo##torp_num##_##coil_lower##_timing && torpedo##torp_num##_timings[ACTUATOR_TORPEDO_TIMING_##coil_upper##_TIME].set) { \
            actuator_populate_command(cmd, ACTUATOR_CMD_TORPEDO_TIMING, actuator_set_timing_general_cb, true); \
            cmd->request.data.torpedo_timing.torpedo_num = torp_num; \
            cmd->request.data.torpedo_timing.timing_type = ACTUATOR_TORPEDO_TIMING_##coil_upper##_TIME; \
            cmd->request.data.torpedo_timing.time_us = torpedo##torp_num##_timings[ACTUATOR_TORPEDO_TIMING_##coil_upper##_TIME].timing; \
            missing_timings.torpedo##torp_num##_##coil_lower##_timing = false; \
        }
    ELSE_IF_TORPEDO_TIMING_NEEDED(1, coil1_on,      COIL1_ON)
    ELSE_IF_TORPEDO_TIMING_NEEDED(1, coil1_2_delay, COIL1_2_DELAY)
    ELSE_IF_TORPEDO_TIMING_NEEDED(1, coil2_on,      COIL2_ON)
    ELSE_IF_TORPEDO_TIMING_NEEDED(1, coil2_3_delay, COIL2_3_DELAY)
    ELSE_IF_TORPEDO_TIMING_NEEDED(1, coil3_on,      COIL3_ON)
    ELSE_IF_TORPEDO_TIMING_NEEDED(2, coil1_on,      COIL1_ON)
    ELSE_IF_TORPEDO_TIMING_NEEDED(2, coil1_2_delay, COIL1_2_DELAY)
    ELSE_IF_TORPEDO_TIMING_NEEDED(2, coil2_on,      COIL2_ON)
    ELSE_IF_TORPEDO_TIMING_NEEDED(2, coil2_3_delay, COIL2_3_DELAY)
    ELSE_IF_TORPEDO_TIMING_NEEDED(2, coil3_on,      COIL3_ON)
    else {
        return false;
    }

    actuator_send_command(cmd);

    return true;
}


static bool actuator_set_timing_general_cb(actuator_cmd_data_t *cmd) {
    if (cmd->response.data.result != ACTUATOR_RESULT_SUCCESSFUL) {
        LOG_ERROR("Failed to set actuator timing (cmd %d)", cmd->request.cmd_id);
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
    }

    return actuator_update_missing_timings_common(cmd);
}

static void actuator_update_missing_timings(void) {
    if (set_timing_command.in_use) {
        return;     // Can't send timings when existing command in progress
    }
    set_timing_command.in_use = true;

    if (!actuator_update_missing_timings_common(&set_timing_command)) {
        set_timing_command.in_use = false;
    }
}

#define RC_RETURN_CHECK(fn) { rcl_ret_t temp_rc = fn; if((temp_rc != RCL_RET_OK)){return temp_rc;}}
rcl_ret_t actuator_create_parameters(rclc_parameter_server_t *param_server) {
    if (!actuator_initialized) {
        return RCL_RET_OK;
    }

    RC_RETURN_CHECK(rclc_add_parameter(param_server, "claw_timing_ms", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "dropper_active_timing_ms", RCLC_PARAMETER_INT));

	RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo1_coil1_on_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo1_coil1_2_delay_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo1_coil2_on_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo1_coil2_3_delay_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo1_coil3_on_timing_us", RCLC_PARAMETER_INT));

    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo2_coil1_on_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo2_coil1_2_delay_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo2_coil2_on_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo2_coil2_3_delay_timing_us", RCLC_PARAMETER_INT));
    RC_RETURN_CHECK(rclc_add_parameter(param_server, "torpedo2_coil3_on_timing_us", RCLC_PARAMETER_INT));

    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "claw_timing_ms", 4500));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "dropper_active_timing_ms", 250));

    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo1_coil1_on_timing_us", 23000));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo1_coil1_2_delay_timing_us", 250));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo1_coil2_on_timing_us", 15000));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo1_coil2_3_delay_timing_us", 250));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo1_coil3_on_timing_us", 13000));

    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo2_coil1_on_timing_us", 23000));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo2_coil1_2_delay_timing_us", 250));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo2_coil2_on_timing_us", 15000));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo2_coil2_3_delay_timing_us", 250));
    RC_RETURN_CHECK(rclc_parameter_set_int(param_server, "torpedo2_coil3_on_timing_us", 13000));

    return RCL_RET_OK;
}

#define IS_VALID_TIMING(num) ((num) > 0 && (num) < (1<<16))
bool actuator_handle_parameter_change(Parameter * param) {
    if (!actuator_initialized) {
        return false;
    }

    // All parameters are int types
    if (param->value.type != RCLC_PARAMETER_INT) {
        return false;
    }

    if (!strcmp(param->name.data, "claw_timing_ms")) {
        if (IS_VALID_TIMING(param->value.integer_value)) {
            claw_timing.timing = param->value.integer_value;
            claw_timing.set = true;

            missing_timings.claw_open_timing = true;
            missing_timings.claw_close_timing = true;
            actuator_update_missing_timings();

            return true;
        } else {
            return false;
        }
    } else if (!strcmp(param->name.data, "dropper_active_timing_ms")) {
        if (IS_VALID_TIMING(param->value.integer_value)) {
            dropper_active_timing.timing = param->value.integer_value;
            dropper_active_timing.set = true;

            missing_timings.dropper_active_timing = true;
            actuator_update_missing_timings();

            return true;
        } else {
            return false;
        }
    }
    #define ELSE_IF_TORPEDO_PARAMETER(torp_num, coil_lower, coil_upper) \
        else if (!strcmp(param->name.data, "torpedo" #torp_num "_" #coil_lower "_timing_us")) { \
            if (IS_VALID_TIMING(param->value.integer_value)) { \
                torpedo##torp_num##_timings[ACTUATOR_TORPEDO_TIMING_##coil_upper##_TIME].timing = param->value.integer_value; \
                torpedo##torp_num##_timings[ACTUATOR_TORPEDO_TIMING_##coil_upper##_TIME].set = true; \
                missing_timings.torpedo##torp_num##_##coil_lower##_timing = true; \
                actuator_update_missing_timings(); \
                return true; \
            } else { \
                return false; \
            } \
        }
    ELSE_IF_TORPEDO_PARAMETER(1, coil1_on,      COIL1_ON)
    ELSE_IF_TORPEDO_PARAMETER(1, coil1_2_delay, COIL1_2_DELAY)
    ELSE_IF_TORPEDO_PARAMETER(1, coil2_on,      COIL2_ON)
    ELSE_IF_TORPEDO_PARAMETER(1, coil2_3_delay, COIL2_3_DELAY)
    ELSE_IF_TORPEDO_PARAMETER(1, coil3_on,      COIL3_ON)
    ELSE_IF_TORPEDO_PARAMETER(2, coil1_on,      COIL1_ON)
    ELSE_IF_TORPEDO_PARAMETER(2, coil1_2_delay, COIL1_2_DELAY)
    ELSE_IF_TORPEDO_PARAMETER(2, coil2_on,      COIL2_ON)
    ELSE_IF_TORPEDO_PARAMETER(2, coil2_3_delay, COIL2_3_DELAY)
    ELSE_IF_TORPEDO_PARAMETER(2, coil3_on,      COIL3_ON)

    else {
        return false;
    }
}


// ========================================
// Actuator Board Monitoring
// ========================================

struct actuator_i2c_status actuator_last_status;

static actuator_cmd_data_t status_command = {.in_use = false, .i2c_in_progress = false};
static actuator_cmd_data_t kill_switch_update_command = {.in_use = false, .i2c_in_progress = false};
static absolute_time_t status_valid_timeout = {0};
static bool version_warning_printed = false;
static bool kill_switch_needs_refresh = false;

static bool actuator_kill_switch_update_callback(actuator_cmd_data_t * cmd) {
    assert(cmd == &kill_switch_update_command);

    if (cmd->response.data.result != ACTUATOR_RESULT_SUCCESSFUL) {
        LOG_ERROR("Non-successful kill switch update response %d", cmd->response.data.result);
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
    } else if (kill_switch_needs_refresh) {
        kill_switch_needs_refresh = false;
        kill_switch_update_command.request.data.kill_switch.asserting_kill = safety_kill_get_asserting_kill();
        kill_switch_update_command.in_use = true;
        actuator_send_command(&kill_switch_update_command);
        return true;
    }
    return false;
}

static bool actuator_status_callback(actuator_cmd_data_t * cmd) {
    struct actuator_i2c_status *status = &cmd->response.data.status;
    if (status->firmware_status.version_major != ACTUATOR_EXPECTED_FIRMWARE_MAJOR && status->firmware_status.version_major != ACTUATOR_EXPECTED_FIRMWARE_MINOR) {
        if (!version_warning_printed) {
            LOG_ERROR("Invalid firmware version found: %d.%d (%d.%d expected)", status->firmware_status.version_major, status->firmware_status.version_minor, ACTUATOR_EXPECTED_FIRMWARE_MAJOR, ACTUATOR_EXPECTED_FIRMWARE_MINOR);
            safety_raise_fault(FAULT_ACTUATOR_FAIL);
            version_warning_printed = true;
        }
    } else {
        memcpy(&actuator_last_status, status, sizeof(*status));
        status_valid_timeout = make_timeout_time_ms(ACTUATOR_MAX_STATUS_AGE_MS);

        if (safety_initialized) {
            if (!kill_switch_update_command.in_use) {
                kill_switch_needs_refresh = false;
                kill_switch_update_command.request.data.kill_switch.asserting_kill = safety_kill_get_asserting_kill();
                kill_switch_update_command.in_use = true;
                actuator_send_command(&kill_switch_update_command);
            }
            // Don't care about case if command not in use. If command is in use then a kill switch update just occurred, so it can skip the refresh
        }

        static_assert(sizeof(status->firmware_status.missing_timings) == sizeof(uint16_t));
        struct missing_timings_status status_missing_timings = status->firmware_status.missing_timings;
        uint16_t *raw_status_missing_timings = (uint16_t*)(&status_missing_timings);
        uint16_t *raw_cached_missing_timings = (uint16_t*)(&missing_timings);
        if (*raw_status_missing_timings) {
            *raw_cached_missing_timings |= *raw_status_missing_timings;
            actuator_update_missing_timings();
        }
    }
    return false;
}

static bool actuator_has_been_polled = false;
/**
 * @brief Alarm callback to poll the actuator board
 *
 * @param id The ID of the alarm that triggered the callback
 * @param user_data User provided data. This is NULL
 * @return int64_t If/How to restart the timer
 */
static int64_t actuator_poll_alarm_callback(__unused alarm_id_t id, __unused void *user_data) {
    if (actuator_has_been_polled) {
        if (!actuator_is_connected()){
            safety_raise_fault(FAULT_NO_ACTUATOR);
        } else {
            safety_lower_fault(FAULT_NO_ACTUATOR);
        }
    } else {
        actuator_has_been_polled = true;
    }

    if (status_command.in_use) {
        LOG_ERROR("Unable to poll actuator board, request still in progress");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
    } else {
        status_command.in_use = true;
        actuator_send_command(&status_command);
    }

    return ACTUATOR_POLLING_RATE_MS * 1000;
}

// ========================================
// Public Command Requests
// ========================================

static bool actuator_generic_result_cb(actuator_cmd_data_t * cmd) {
    if (cmd->response.data.result == ACTUATOR_RESULT_FAILED){
        if (cmd->important_request) {
            LOG_ERROR("Request %d returned failed result %d", cmd->request.cmd_id, cmd->response.data.result);
            safety_raise_fault(FAULT_ACTUATOR_FAIL);
        } else {
            LOG_WARN("Non-critical request %d returned failed result %d", cmd->request.cmd_id, cmd->response.data.result);
        }
    }
    return false;
}

void actuator_open_claw(void) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_OPEN_CLAW, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    actuator_send_command(cmd);
}

void actuator_close_claw(void) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_CLOSE_CLAW, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    actuator_send_command(cmd);
}

void actuator_set_claw_timings(uint16_t open_time_ms, uint16_t close_time_ms) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_CLAW_TIMING, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    cmd->request.data.claw_timing.open_time_ms = open_time_ms;
    cmd->request.data.claw_timing.close_time_ms = close_time_ms;
    actuator_send_command(cmd);
}

void actuator_arm_torpedo(void) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_ARM_TORPEDO, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    actuator_send_command(cmd);
}

void actuator_disarm_torpedo(void) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_DISARM_TORPEDO, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    actuator_send_command(cmd);
}

void actuator_fire_torpedo(uint8_t torpedo_id) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_FIRE_TORPEDO, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    cmd->request.data.fire_torpedo.torpedo_num = torpedo_id;
    actuator_send_command(cmd);
}

void actuator_set_torpedo_timings(uint8_t torpedo_id, enum torpedo_timing_type timing_type, uint16_t time_us) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_TORPEDO_TIMING, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    cmd->request.data.torpedo_timing.torpedo_num = torpedo_id;
    cmd->request.data.torpedo_timing.timing_type = timing_type;
    cmd->request.data.torpedo_timing.time_us = time_us;
    actuator_send_command(cmd);
}

void actuator_drop_marker(uint8_t dropper_id) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_DROP_MARKER, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    cmd->request.data.drop_marker.dropper_num = dropper_id;
    actuator_send_command(cmd);
}

void actuator_clear_dropper_status(void) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_CLEAR_DROPPER_STATUS, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    actuator_send_command(cmd);
}

void actuator_set_dropper_timings(uint16_t active_time_ms) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_DROPPER_TIMING, actuator_generic_result_cb);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    cmd->request.data.dropper_timing.active_time_ms = active_time_ms;
    actuator_send_command(cmd);
}

void actuator_reset_actuators(void) {
    actuator_cmd_data_t* cmd = actuator_generate_command(ACTUATOR_CMD_RESET_ACTUATORS, NULL);
    if (!cmd) {
        LOG_ERROR("Failed to create request");
        safety_raise_fault(FAULT_ACTUATOR_FAIL);
        return;
    }
    actuator_send_command(cmd);
}

// ========================================
// Misc Public Methods
// ========================================

bool actuator_initialized = false;

bool actuator_is_connected(void) {
    if (actuator_initialized) {
        return absolute_time_diff_us(status_valid_timeout, get_absolute_time()) < 0;
    } else {
        return false;
    }
}

void actuator_kill_report_refresh(void) {
    if (!actuator_initialized) {
        return;
    }

    if (kill_switch_update_command.in_use) {
        kill_switch_needs_refresh = true;
    } else {
        kill_switch_needs_refresh = false;
        kill_switch_update_command.request.data.kill_switch.asserting_kill = safety_kill_get_asserting_kill();
        kill_switch_update_command.in_use = true;
        actuator_send_command(&kill_switch_update_command);
    }
}

void actuator_init(void){
    hard_assert_if(LIFETIME_CHECK, actuator_initialized);

    status_valid_timeout = get_absolute_time(); // Expire the last status immediately
    actuator_initialized = true;
    actuator_populate_command(&status_command, ACTUATOR_CMD_GET_STATUS, actuator_status_callback, false);
    actuator_populate_command(&kill_switch_update_command, ACTUATOR_CMD_KILL_SWITCH, actuator_kill_switch_update_callback, true);
    hard_assert(add_alarm_in_ms(ACTUATOR_POLLING_RATE_MS, &actuator_poll_alarm_callback, NULL, true) > 0);
}
