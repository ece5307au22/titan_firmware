#ifndef _DEPTH_SENSOR_H
#define _DEPTH_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

// PICO_CONFIG: PARAM_ASSERTIONS_ENABLED_DEPTH, Enable/disable assertions in the Depth Sensor module, type=bool, default=0, group=Copro
#ifndef PARAM_ASSERTIONS_ENABLED_DEPTH
#define PARAM_ASSERTIONS_ENABLED_DEPTH 0
#endif

/**
 * @brief Boolean for if depth is initialized.
 * This will be false until all calibration and zeroing is complete
 * This may never become true if the depth sensor fails to initialize
 */
extern bool depth_initialized;

/**
 * @brief Reads the value from the depth sensor
 * 
 * REQUIRES INITIALIZATION
 * 
 * @return double The raw depth reading
 */
double depth_read(void);

/**
 * @brief Returns the current temperature read from the depth sensor
 * 
 * REQUIRES INITIALIZATION
 * 
 * @return float 
 */
float depth_get_temperature(void);

/**
 * @brief Begins initialization of depth sensor
 * 
 * Note: The sensor is not initialized until depth_initialized is true
 * 
 */
void depth_init(void);

/**
 * @brief Returns if the depth reading is valid
 * 
 * @return true If depth_read will return a valid reading
 * @return false If depth has not been initialized or the current depth reading is stale
 */
bool depth_reading_valid(void);

#endif