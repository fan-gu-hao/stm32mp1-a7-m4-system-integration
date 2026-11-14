#ifndef MP1_VL53L0X_H
#define MP1_VL53L0X_H

#include <stddef.h>
#include "stm32mp1xx_hal.h"

void MP1_VL53L0X_ReadOnce(I2C_HandleTypeDef *i2c,
                         char *reply, size_t capacity);

#endif
