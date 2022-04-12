#ifndef _BACKPLANE_BREAKOUT_BOARD_H
#define _BACKPLANE_BREAKOUT_BOARD_H

#include "boards/rp2040_eval_board.h"

#ifdef UWRT_BOARD_DEFINED
#error Multiple board types defined
#endif
#define UWRT_BOARD_DEFINED

// I2C Bus
#define CONNECTOR_I2C_HW        i2c1
#define CONNECTOR_SDA_PIN       26
#define CONNECTOR_SCL_PIN       27

// Torpedos
#define COIL_1_PIN          23
#define COIL_2_PIN          24
#define COIL_3_PIN          21
#define TORP_SEL_1_PIN      20
#define TORP_SEL_2_PIN      18
#define TORP_ARM_PIN        16

// Claw
#define CLAW_ENABLE_PIN     25
#define CLAW_FORWARD_PIN    29
#define CLAW_REVERSE_PIN    22

// Dropper
#define DROPPER_1_PIN       17
#define DROPPER_2_PIN       19


// On-Board LED Pin
#define FAULT_LED_PIN       BUILTIN_LED1_PIN


#endif