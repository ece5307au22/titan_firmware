#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/exception.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "basic_logger/logging.h"

#include "safety/safety.h"

#undef LOGGING_UNIT_NAME
#define LOGGING_UNIT_NAME "safety"

#define SAFETY_WATCHDOG_SETUP_TIMER_MS  3000
#define SAFETY_WATCHDOG_ACTIVE_TIMER_MS  250

// ========================================
// External Interface Functions
// ========================================
// NOTE: These functions need to be defined in the code using the safety library

extern const int num_kill_switches;
extern struct kill_switch_state kill_switch_states[];

/**
 * @brief Called to set the fault led light
 * 
 * @param on Logic level of the light
 */
void safety_set_fault_led(bool on);

/**
 * @brief Callback for when the robot is killed. This should stop any actions that must stop when disabled
 */
void safety_kill_robot(void);

/**
 * @brief Callback for when the robot is enabled
 */
void safety_enable_robot(void);

/**
 * @brief Looks up fault id name for a given fault id
 * 
 * @param fault_id The fault id to lookup
 * @return const char* The fault name
 */
const char * safety_lookup_fault_id(uint32_t fault_id);


// ========================================
// Fault Management Functions
// ========================================

volatile uint32_t * const fault_list = &watchdog_hw->scratch[6];

void safety_raise_fault(uint32_t fault_id) {
    valid_params_if(SAFETY, fault_id <= MAX_FAULT_ID);

    if ((*fault_list & (1u<<fault_id)) == 0) {
        LOG_FAULT("Fault %s (%d) Raised", safety_lookup_fault_id(fault_id), fault_id);

        // To ensure the fault led doesn't get glitched on/off due to an untimely interrupt, interrupts will be disabled during
        // the setting of the fault state and the fault LED

        uint32_t prev_interrupt_state = save_and_disable_interrupts();
        
        *fault_list |= (1<<fault_id);
        safety_set_fault_led(true);
        
        restore_interrupts(prev_interrupt_state);
    }
}

void safety_lower_fault(uint32_t fault_id) {
    valid_params_if(SAFETY, fault_id <= MAX_FAULT_ID);

    if ((*fault_list & (1u<<fault_id)) != 0) {
        LOG_FAULT("Fault %s (%d) Lowered", safety_lookup_fault_id(fault_id), fault_id);
        
        // To ensure the fault led doesn't get glitched on/off due to an untimely interrupt, interrupts will be disabled during
        // the setting of the fault state and the fault LED

        uint32_t prev_interrupt_state = save_and_disable_interrupts();
        
        *fault_list &= ~(1u<<fault_id);
        safety_set_fault_led((*fault_list) != 0);
        
        restore_interrupts(prev_interrupt_state);
    }
}


// ========================================
// Kill Switch Management Functions
// ========================================

static absolute_time_t last_kill_switch_change;
static bool last_state_asserting_kill = true; // Start asserting kill

/**
 * @brief Local utility function to do common tasks for when the robot is killed
 * Called from any function that will kill the robot
 * 
 * This function should only be called when safety is initialized
 */
static void safety_local_kill_robot(void) {
    last_state_asserting_kill = true;
    last_kill_switch_change = get_absolute_time();

    safety_kill_robot();

    LOG_DEBUG("Disabling Robot");
}

/**
 * @brief Refreshes kill switches to check for any timeouts
 * It is responsible for re-enabling the robot after all of the kill switches have been released.
 * 
 * This function should only be called when safety is initialized
 */
static void safety_refresh_kill_switches(void) {
    absolute_time_t now = get_absolute_time();

    // Check all kill switches for asserting kill
    bool asserting_kill = false;
    int num_switches_enabled = 0;
    for (int i = 0; i < num_kill_switches; i++) {
        if (kill_switch_states[i].enabled) {
            num_switches_enabled++;

            // Kill if asserting kill or if timeout expired when requiring update
            if (kill_switch_states[i].asserting_kill || 
                    (kill_switch_states[i].needs_update && absolute_time_diff_us(now, kill_switch_states[i].update_timeout) < 0)) {
                asserting_kill = true;
                break;
            }
        }
    }

    // If no kill switches are enabled, force into kill as a precaution
    if (num_switches_enabled == 0) {
        asserting_kill = true;
    }

    // Update last state, and notify of kill if needed
    if (last_state_asserting_kill != asserting_kill) {
        last_state_asserting_kill = asserting_kill;

        if (asserting_kill) {
            safety_local_kill_robot();
        } else {
            LOG_DEBUG("Enabling Robot");
            last_kill_switch_change = get_absolute_time();
            safety_enable_robot();
        }
    }
}

void safety_kill_switch_update(uint8_t switch_num, bool asserting_kill, bool needs_update){
    valid_params_if(SAFETY, switch_num < num_kill_switches);

    kill_switch_states[switch_num].asserting_kill = asserting_kill;
    kill_switch_states[switch_num].update_timeout = make_timeout_time_ms(KILL_SWITCH_TIMEOUT_MS);
    kill_switch_states[switch_num].needs_update = needs_update;
    kill_switch_states[switch_num].enabled = true;

    if (safety_initialized && asserting_kill) {
        safety_local_kill_robot();
    }
}

bool safety_kill_get_asserting_kill(void) {
    hard_assert_if(LIFETIME_CHECK, !safety_initialized);
    return last_state_asserting_kill;
}

absolute_time_t safety_kill_get_last_change(void) {
    hard_assert_if(LIFETIME_CHECK, !safety_initialized);
    return last_kill_switch_change;
}



// ========================================
// Watchdog Crash Reporting Functions
// ========================================

// Use of Watchdog Scratch Registers
// Can really only be considered valid if 
// scratch[0]: Last Crash Action
//  - UNKNOWN_SAFETY_PREINIT: Unknown, crashed after safety_setup
//  - UNKNOWN_SAFETY_ACTIVE: Unknown, crashed after safety_init
//  - PANIC: Set on panic function call
//     scratch[1]: Address of panic string (Won't be dereferenced for safety)
//  - HARD_FAULT: Set in the hard fault exception handler (Any unhandled exception raises this)
//     scratch[1]: Faulting Address
//  - ASSERT_FAIL: Set in assertion callback
//     scratch[1]: Faulting File String Address
//     scratch[2]: Faulting File Line
//  - IN_ROS_TRANSPORT_LOOP: Set while blocking for response from ROS agent
// scratch[3]: Watchdog Reset Counter (LSB order)
//     Byte 0: Total Watchdog Reset Counter
//     Byte 1: Panic Reset Counter
//     Byte 2: Hard Fault Reset Counter
//     Byte 3: Assertion Fail Reset Counter
// scratch[6]: Bitwise Fault List
// scratch[7]: Depth Sensor Backup Data
//     Default: Should be set to 0xFFFFFFFF on clean boot
//     Will be set during zeroing of the depth sensor

#define UNKNOWN_SAFETY_PREINIT  0x1035001
#define UNKNOWN_SAFETY_ACTIVE   0x1035002
#define PANIC                   0x1035003
#define HARD_FAULT              0x1035004
#define ASSERT_FAIL             0x1035005
#define IN_ROS_TRANSPORT_LOOP   0x1035006

// Very useful for debugging, prevents cpu from resetting while execution is halted for debugging
// However, this should be ideally be disabled when not debugging in the event something goes horribly wrong
#define PAUSE_WATCHDOG_ON_DEBUG 1

static volatile uint32_t *reset_reason_reg = &watchdog_hw->scratch[0];
static volatile uint32_t *reset_counter = &watchdog_hw->scratch[3];

// Defined in hard_fault_handler.S
extern void safety_hard_fault_handler(void);
static exception_handler_t original_hardfault_handler = NULL;

// Assertion Handling
extern void __real___assert_func(const char *file, int line, const char *func, const char *failedexpr);

void __wrap___assert_func(const char *file, int line, const char *func, const char *failedexpr) {
    *reset_reason_reg = ASSERT_FAIL;
    watchdog_hw->scratch[1] = (uint32_t) file;
    watchdog_hw->scratch[2] = (uint32_t) line;
    if ((*reset_counter & 0xFF000000) != 0xFF000000) {
        *reset_counter += 0x1000000;
    }

    // Remove the hard fault exception handler so it doesn't overwrite panic data when the breakpoint is hit
    if (original_hardfault_handler != NULL) {
        exception_restore_handler(HARDFAULT_EXCEPTION, original_hardfault_handler);
    }
    
    __real___assert_func(file, line, func, failedexpr);
}

/**
 * @brief Restores the hardfault handler to default
 * Used in panic functions to ensure that the hardfault handler doesn't overwrite the panic data when breakpoint is called after a panic
 */
void safety_restore_hardfault(void) {
    // Remove the hard fault exception handler so it doesn't overwrite panic data when the breakpoint is hit
    if (original_hardfault_handler != NULL) {
        exception_restore_handler(HARDFAULT_EXCEPTION, original_hardfault_handler);
    }
}

static bool had_watchdog_reboot = false;
static uint32_t last_reset_reason = 0;
static uint32_t last_fault_list = 0;
static uint32_t prev_scratch1 = 0;
static uint32_t prev_scratch2 = 0;

static void safety_print_last_reset_cause(void){
    if (had_watchdog_reboot) {
        char message[256];
        message[0] = '\0';

        snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "Watchdog Reset (Total Crashes: %d", (*reset_counter) & 0xFF);
        if ((*reset_counter) & 0xFF00) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, " - Panics: %d", ((*reset_counter) >> 8) & 0xFF);
        }
        if ((*reset_counter) & 0xFF0000) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, " - Hard Faults: %d", ((*reset_counter) >> 16) & 0xFF);
        }
        if ((*reset_counter) & 0xFF000000) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, " - Assert Fails: %d", ((*reset_counter) >> 24) & 0xFF);
        }

        if (last_fault_list != 0) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, ") (Faults: 0x%x", last_fault_list);
        }

        snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, ") - Reason: ");

        if (last_reset_reason == UNKNOWN_SAFETY_PREINIT) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "UNKNOWN_SAFETY_PREINIT");
        } else if (last_reset_reason == UNKNOWN_SAFETY_ACTIVE) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "UNKNOWN_SAFETY_ACTIVE");
        } else if (last_reset_reason == PANIC) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "PANIC (Message: 0x%08x, Call Address: 0x%08x)", prev_scratch1, prev_scratch2);
        } else if (last_reset_reason == HARD_FAULT) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "HARD_FAULT (Fault Address: 0x%08x)", prev_scratch1);
        } else if (last_reset_reason == ASSERT_FAIL) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "ASSERT_FAIL (File: 0x%08x Line: %d)", prev_scratch1, prev_scratch2);
        } else if (last_reset_reason == IN_ROS_TRANSPORT_LOOP) {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "ROS Agent Lost");
        } else {
            snprintf(message+strlen(message), sizeof(message)-strlen(message)-1, "Invalid Data in Reason Register");
        }
        LOG_INFO(message);
    } else {
        LOG_INFO("Clean boot");
    }
}

/**
 * @brief Does processing of the last reset cause and prints it to serial.
 * Also increments the total watchdog reset counter
 * 
 * Should only be called once and before reset reason is overwritten
 */
static void safety_process_last_reset_cause(void) {
    // Handle data in watchdog registers
    bool should_raise_fault = false;
    if (watchdog_enable_caused_reboot()) {
        last_reset_reason = *reset_reason_reg;
        last_fault_list = *fault_list;
        prev_scratch1 = watchdog_hw->scratch[1];
        prev_scratch2 = watchdog_hw->scratch[2];
        had_watchdog_reboot = true;
        
        if (last_reset_reason != IN_ROS_TRANSPORT_LOOP) {
            if (((*reset_counter) & 0xFF) != 0xFF) 
                *reset_counter = *reset_counter + 1;
            should_raise_fault = true;
        }

        if (*reset_counter != 0) {
            should_raise_fault = true;
        }

        // Clear any previous faults
        *fault_list = 0;
    } else {
        *reset_counter = 0;
        *fault_list = 0;
        watchdog_hw->scratch[7] = 0xFFFFFFFF;
        had_watchdog_reboot = false;
    }

    safety_print_last_reset_cause();
    if (should_raise_fault) {
        safety_raise_fault(FAULT_WATCHDOG_RESET);
    }
}

// ========================================
// Safety Limetime Functions
// ========================================

bool safety_initialized = false;
bool safety_is_setup = false;

void safety_setup(void) {
    hard_assert_if(LIFETIME_CHECK, safety_is_setup || safety_initialized);

    // Set hardfault handler
    original_hardfault_handler = exception_set_exclusive_handler(HARDFAULT_EXCEPTION, &safety_hard_fault_handler);

    safety_process_last_reset_cause();

    // Set reset reason
    *reset_reason_reg = UNKNOWN_SAFETY_PREINIT;
    safety_is_setup = true;

    // Enable slow watchdog while connecting
    watchdog_enable(SAFETY_WATCHDOG_SETUP_TIMER_MS, PAUSE_WATCHDOG_ON_DEBUG);
}

void safety_init(void) {
    hard_assert_if(LIFETIME_CHECK, !safety_is_setup || safety_initialized);

    safety_initialized = true;
    *reset_reason_reg = UNKNOWN_SAFETY_ACTIVE;

    // Populate the last kill switch change time to when safety is set up
    last_kill_switch_change = get_absolute_time();

    // Set tight watchdog timer for normal operation
    watchdog_enable(SAFETY_WATCHDOG_ACTIVE_TIMER_MS, PAUSE_WATCHDOG_ON_DEBUG);
}

void safety_tick(void) {
    hard_assert_if(LIFETIME_CHECK, !safety_is_setup);

    // Check for any kill switch timeouts
    if (safety_initialized) {
        safety_refresh_kill_switches();
    }

    watchdog_update();
}