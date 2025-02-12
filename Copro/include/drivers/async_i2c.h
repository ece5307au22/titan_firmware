#ifndef _ASYNC_I2C_H
#define _ASYNC_I2C_H

#include <stdint.h>
#include <stdbool.h>

#include "hardware/i2c.h"

// PICO_CONFIG: PARAM_ASSERTIONS_ENABLED_ASYNC_I2C, Enable/disable assertions in the Async I2C module, type=bool, default=0, group=Copro
#ifndef PARAM_ASSERTIONS_ENABLED_ASYNC_I2C
#define PARAM_ASSERTIONS_ENABLED_ASYNC_I2C 0
#endif

// I2C Request Types
struct async_i2c_request;

/**
 * @brief Typedef for callback to i2c function
 */
typedef void (*async_i2c_cb_t)(const struct async_i2c_request *);
typedef void (*async_i2c_abort_cb_t)(const struct async_i2c_request *, uint32_t);
struct async_i2c_request {
    i2c_inst_t *i2c;
    uint8_t address;
    bool nostop;
    const uint8_t *tx_buffer;
    uint8_t *rx_buffer;
    uint16_t bytes_to_send;
    uint16_t bytes_to_receive;
    async_i2c_cb_t completed_callback;
    async_i2c_abort_cb_t failed_callback;
    const struct async_i2c_request *next_req_on_success;
    void* user_data;
};

/**
 * @brief Creates a read/write request.
 * First sends tx_size bytes from tx_buf and reads rx_size bytes into rx_buf.
 * tx_buf and rx_buf must not be modified while in_progress is true
 * fn_callback is called on a successful request
 */
#define ASYNC_I2C_READ_WRITE_REQ(i2c_inst, target_address, tx_buf, rx_buf, tx_size, rx_size, fn_callback) {\
    .i2c = i2c_inst, \
    .address = target_address, \
    .nostop = false, \
    .tx_buffer = tx_buf, \
    .rx_buffer = rx_buf, \
    .bytes_to_send = tx_size, \
    .bytes_to_receive = rx_size, \
    .completed_callback = fn_callback, \
    .failed_callback = NULL, \
    .next_req_on_success = NULL, \
    .user_data = NULL}

/**
 * @brief Creates a write request
 * Sends tx_size bytes from tx_buf
 * tx_buf must not be modified while in_progress is true
 * fn_callback is called on a successful request
 */
#define ASYNC_I2C_WRITE_REQ(hw_id, address, tx_buf, tx_size, fn_callback) {\
    .i2c = i2c_inst, \
    .address = target_address, \
    .nostop = false, \
    .tx_buffer = tx_buf, \
    .rx_buffer = NULL, \
    .bytes_to_send = tx_size, \
    .bytes_to_receive = 0, \
    .completed_callback = fn_callback, \
    .failed_callback = NULL, \
    .next_req_on_success = NULL \
    .user_data = NULL}

/**
 * @brief Creates a read request
 * Reads rx_size bytes into rx_buf.
 * rx_buf must not be modified while in_progress is true
 * fn_callback is called on a successful request
 */
#define ASYNC_I2C_READ_REQ(i2c_inst, target_address, rx_buf, rx_size, fn_callback) {\
    .i2c = i2c_inst, \
    .address = target_address, \
    .nostop = false, \
    .tx_buffer = NULL, \
    .rx_buffer = rx_buf, \
    .bytes_to_send = 0, \
    .bytes_to_receive = rx_size, \
    .completed_callback = fn_callback, \
    .failed_callback = NULL, \
    .next_req_on_success = NULL \
    .user_data = NULL}



/**
 * @brief Bool if async_i2c_init has been called
 */
extern bool async_i2c_initialized;

/**
 * @brief Queues an async i2c request
 * 
 * INITIALIZATION REQUIRED
 * INTERRUPT SAFE
 * 
 * @param request Struct if request is true
 * @param in_progress Pointer to be true while request is in progress (and buffers should not be modified)
 * Note that in_progress will be true while next_req_on_success is being processed as well
 */
void async_i2c_enqueue(const struct async_i2c_request *request, bool *in_progress);

/**
 * @brief Initialize async i2c and the corresponding i2c hardware
 * 
 * @param baudrate The data rate of the i2c bus in Hz
 * @param bus_timeout_ms The timeout from start of a transaction in ms
 */
void async_i2c_init(uint baudrate, uint bus_timeout_ms);

#endif