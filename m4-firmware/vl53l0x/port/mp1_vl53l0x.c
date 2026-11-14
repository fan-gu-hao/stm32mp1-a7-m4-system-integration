#include "mp1_vl53l0x.h"
#include "vl53l0x_api.h"
#include <stdio.h>
#include <string.h>

static I2C_HandleTypeDef *tof_bus;
static VL53L0X_Dev_t tof_device;
static uint8_t tof_ready;

static const char *tof_reset_step = "NONE";
static struct {
    uint8_t valid;
    char operation;
    uint8_t reg;
    HAL_StatusTypeDef hal;
    uint32_t error;
    const char *step;
} tof_io_fault;

static VL53L0X_Error io_result(HAL_StatusTypeDef status,
                               char operation, uint8_t reg)
{
    /* 保留第一筆失敗，避免後續解除重設的操作蓋掉原因。 */
    if (status != HAL_OK && !tof_io_fault.valid) {
        tof_io_fault.valid = 1U;
        tof_io_fault.operation = operation;
        tof_io_fault.reg = reg;
        tof_io_fault.hal = status;
        tof_io_fault.error = HAL_I2C_GetError(tof_bus);
        tof_io_fault.step = tof_reset_step;
    }
    if (status == HAL_OK) return VL53L0X_ERROR_NONE;
    if (status == HAL_TIMEOUT) return VL53L0X_ERROR_TIME_OUT;
    return VL53L0X_ERROR_CONTROL_INTERFACE;
}

VL53L0X_Error VL53L0X_WriteMulti(VL53L0X_DEV dev, uint8_t index,
                                uint8_t *data, uint32_t count)
{
    if (!tof_bus || !dev || !data || count == 0U || count > 65535U)
        return VL53L0X_ERROR_INVALID_PARAMS;
    return io_result(HAL_I2C_Mem_Write(tof_bus, dev->I2cDevAddr,
        index, I2C_MEMADD_SIZE_8BIT, data, (uint16_t)count, 100U),
        'W', index);
}

VL53L0X_Error VL53L0X_ReadMulti(VL53L0X_DEV dev, uint8_t index,
                               uint8_t *data, uint32_t count)
{
    if (!tof_bus || !dev || !data || count == 0U || count > 65535U)
        return VL53L0X_ERROR_INVALID_PARAMS;
    memset(data, 0, count);
    return io_result(HAL_I2C_Mem_Read(tof_bus, dev->I2cDevAddr,
        index, I2C_MEMADD_SIZE_8BIT, data, (uint16_t)count, 100U),
        'R', index);
}

VL53L0X_Error VL53L0X_WrByte(VL53L0X_DEV dev, uint8_t index, uint8_t value)
{
    return VL53L0X_WriteMulti(dev, index, &value, 1U);
}

VL53L0X_Error VL53L0X_WrWord(VL53L0X_DEV dev, uint8_t index, uint16_t value)
{
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    return VL53L0X_WriteMulti(dev, index, bytes, sizeof(bytes));
}

VL53L0X_Error VL53L0X_WrDWord(VL53L0X_DEV dev, uint8_t index, uint32_t value)
{
    uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16),
                        (uint8_t)(value >> 8), (uint8_t)value};

    return VL53L0X_WriteMulti(dev, index, bytes, sizeof(bytes));
}

VL53L0X_Error VL53L0X_RdByte(VL53L0X_DEV dev, uint8_t index, uint8_t *value)
{
    return VL53L0X_ReadMulti(dev, index, value, 1U);
}

VL53L0X_Error VL53L0X_RdWord(VL53L0X_DEV dev, uint8_t index, uint16_t *value)
{
    uint8_t bytes[2];
    VL53L0X_Error status;

    if (!value) return VL53L0X_ERROR_INVALID_PARAMS;

    *value = 0;
    status = VL53L0X_ReadMulti(dev, index, bytes, sizeof(bytes));

    if (status == VL53L0X_ERROR_NONE)
        *value = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);

    return status;
}

VL53L0X_Error VL53L0X_RdDWord(VL53L0X_DEV dev, uint8_t index, uint32_t *value)
{
    uint8_t bytes[4];
    VL53L0X_Error status;

    if (!value) return VL53L0X_ERROR_INVALID_PARAMS;

    *value = 0;
    status = VL53L0X_ReadMulti(dev, index, bytes, sizeof(bytes));

    if (status == VL53L0X_ERROR_NONE)
        *value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
               | ((uint32_t)bytes[2] << 8) | bytes[3];

    return status;
}

VL53L0X_Error VL53L0X_UpdateByte(VL53L0X_DEV dev, uint8_t index,
                                uint8_t and_mask, uint8_t or_mask)
{
    uint8_t value;
    VL53L0X_Error status = VL53L0X_RdByte(dev, index, &value);

    if (status != VL53L0X_ERROR_NONE) return status;

    return VL53L0X_WrByte(dev, index, (value & and_mask) | or_mask);
}

/* API 僅由 M4 主迴圈呼叫，不在中斷中呼叫。 */
VL53L0X_Error VL53L0X_LockSequenceAccess(VL53L0X_DEV dev)
{
    (void)dev;
    return VL53L0X_ERROR_NONE;
}

VL53L0X_Error VL53L0X_UnlockSequenceAccess(VL53L0X_DEV dev)
{
    (void)dev;
    return VL53L0X_ERROR_NONE;
}

VL53L0X_Error VL53L0X_PollingDelay(VL53L0X_DEV dev)
{
    (void)dev;
    HAL_Delay(1U);
    return VL53L0X_ERROR_NONE;
}

static VL53L0X_Error wait_model_id(uint8_t expected)
{
    uint32_t started = HAL_GetTick();

    do {
        uint8_t value;
        VL53L0X_Error status = VL53L0X_RdByte(&tof_device,
            VL53L0X_REG_IDENTIFICATION_MODEL_ID, &value);

        if (status != VL53L0X_ERROR_NONE) return status;
        if (value == expected) return VL53L0X_ERROR_NONE;

        HAL_Delay(1U);
    } while ((uint32_t)(HAL_GetTick() - started) < 100U);

    return VL53L0X_ERROR_TIME_OUT;
}

/* M4 重新啟動時，感測器不一定有斷電，因此先執行軟體重置。 */
static VL53L0X_Error sensor_reset(void)
{
    VL53L0X_Error status, release_status;
    tof_reset_step = "PAGE";
    status = VL53L0X_WrByte(&tof_device, 0xFF, 0x00);
    if (status != VL53L0X_ERROR_NONE) return status;

    tof_reset_step = "ASSERT";
    status = VL53L0X_WrByte(&tof_device,
        VL53L0X_REG_SOFT_RESET_GO2_SOFT_RESET_N, 0x00);
    if (status != VL53L0X_ERROR_NONE) return status;

    tof_reset_step = "WAIT_ZERO";
    status = wait_model_id(0x00);
    HAL_Delay(1U);

    tof_reset_step = "RELEASE";
    release_status = VL53L0X_WrByte(&tof_device,
        VL53L0X_REG_SOFT_RESET_GO2_SOFT_RESET_N, 0x01);
    if (release_status != VL53L0X_ERROR_NONE) return release_status;
    if (status != VL53L0X_ERROR_NONE) {
        tof_reset_step = "WAIT_ZERO";
        return status;
    }

    tof_reset_step = "WAIT_ID";
    /* Allow the sensor to boot before the first ID transaction. */
    HAL_Delay(5U);
    status = wait_model_id(0xEE);
    HAL_Delay(1U);
    if (status == VL53L0X_ERROR_NONE) tof_reset_step = "NONE";
    return status;
}

#define TOF_STEP(name, call) do { \
    stage = (name); status = (call); \
    if (status != VL53L0X_ERROR_NONE) goto failed; \
} while (0)

void MP1_VL53L0X_ReadOnce(I2C_HandleTypeDef *i2c,
                         char *reply, size_t capacity)
{
    memset(&tof_io_fault, 0, sizeof(tof_io_fault));
    tof_reset_step = "NONE";
    const char *stage = "BIND";
    VL53L0X_Error status = VL53L0X_ERROR_INVALID_PARAMS;
    VL53L0X_RangingMeasurementData_t measurement = {0};
    uint32_t spad_count;
    uint8_t aperture, vhv, phase;

    if (!reply || capacity == 0U) return;
    if (!i2c) goto failed;

    tof_bus = i2c;

    if (!tof_ready) {
        memset(&tof_device, 0, sizeof(tof_device));

        /* 7 位元位址 0x29，左移一次後交給 HAL。 */
        tof_device.I2cDevAddr = 0x52;
        tof_device.comms_type = 1;
        tof_device.comms_speed_khz = 100;

        TOF_STEP("RESET", sensor_reset());

        /* 對應官方 API 的 USE_I2C_2V8 設定。 */
        TOF_STEP("IO_2V8", VL53L0X_UpdateByte(&tof_device,
            VL53L0X_REG_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV, 0xFE, 0x01));

        TOF_STEP("DATA_INIT", VL53L0X_DataInit(&tof_device));

        TOF_STEP("STATIC_INIT", VL53L0X_StaticInit(&tof_device));

        TOF_STEP("REF_CAL", VL53L0X_PerformRefCalibration(
            &tof_device, &vhv, &phase));

        TOF_STEP("SPAD", VL53L0X_PerformRefSpadManagement(
            &tof_device, &spad_count, &aperture));

        TOF_STEP("MODE", VL53L0X_SetDeviceMode(
            &tof_device, VL53L0X_DEVICEMODE_SINGLE_RANGING));

        TOF_STEP("BUDGET", VL53L0X_SetMeasurementTimingBudgetMicroSeconds(
            &tof_device, 33000U));

        tof_ready = 1U;
    }

    TOF_STEP("RANGE", VL53L0X_PerformSingleRangingMeasurement(
        &tof_device, &measurement));

    if (measurement.RangeStatus == 0U) {
        snprintf(reply, capacity,
            "TOF,RANGE,OK,MM=%u,RANGE_STATUS=0,FW=TOF_STREAM_V1\n",
            (unsigned int)measurement.RangeMilliMeter);
    } else {
        snprintf(reply, capacity,
            "TOF,RANGE,INVALID,RANGE_STATUS=%u,FW=TOF_STREAM_V1\n",
            (unsigned int)measurement.RangeStatus);
    }

    return;

failed:
    tof_ready = 0U;

    snprintf(reply, capacity,
        "TOF,RANGE,ERROR,STAGE=%s,API=%d,STEP=%s,"
        "OP=%c,REG=0x%02X,HAL=%u,I2C=0x%08lX,FW=TOF_STREAM_V1\n",
        stage, (int)status,
        tof_io_fault.valid ? tof_io_fault.step : tof_reset_step,
        tof_io_fault.valid ? tof_io_fault.operation : '-',
        (unsigned int)tof_io_fault.reg,
        (unsigned int)tof_io_fault.hal,
        (unsigned long)tof_io_fault.error);
}

#undef TOF_STEP
