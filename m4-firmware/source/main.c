/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    OpenAMP/OpenAMP_TTY_echo/Inc/main.c
  * @author  MCD Application Team
  * @brief   Main program body.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "mp1_vl53l0x.h"
/* MP1_TOF_STREAM_V1 */
#include <stdio.h>
#include <string.h>
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE BEGIN Includes */
#include "openamp/rpmsg.h"
/* USER CODE END Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
IPCC_HandleTypeDef hipcc;


/* USER CODE BEGIN PV */
VIRT_UART_HandleTypeDef huart0;
VIRT_UART_HandleTypeDef huart1;
static uint8_t rpmsg0_ready = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_IPCC_Init(void);
/* USER CODE BEGIN PFP */
void VIRT_UART0_RxCpltCallback(VIRT_UART_HandleTypeDef *huart);
void VIRT_UART1_RxCpltCallback(VIRT_UART_HandleTypeDef *huart);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */
/* USER CODE BEGIN 0 */

/* MP1_TOF_STREAM_BEGIN */
static I2C_HandleTypeDef tof_i2c5;

static void TOF_Probe_Run(char *reply, size_t capacity)
{
    GPIO_InitTypeDef gpio = {0};
    HAL_StatusTypeDef result = HAL_OK;
    const char *stage = "CLOCK";
    uint8_t id[3] = {0};
    unsigned int scl = 2U;
    unsigned int sda = 2U;
    uint32_t started;
    uint32_t clock_hz;

    /* Read the shared clock configuration; do not change it. */
    clock_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_I2C35);

    if (clock_hz != 64000000U)
    {
        snprintf(reply, capacity,
                 "TOF,PROBE,CLOCK_ERROR,KCLK=%lu\n",
                 (unsigned long)clock_hz);
        return;
    }

    __HAL_RCC_HSEM_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /*
     * HSEM 0 protects GPIO configuration, including Linux pinctrl.
     * Use a bounded wait so a lock failure can be reported.
     */
    started = HAL_GetTick();

    while (HAL_HSEM_FastTake(0U) != HAL_OK)
    {
        if ((uint32_t)(HAL_GetTick() - started) >= 100U)
        {
            snprintf(reply, capacity,
                     "TOF,PROBE,GPIO_LOCK_TIMEOUT\n");
            return;
        }
    }

    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF4_I2C5;

    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_HSEM_Release(0U, 0U);

    /* Enable the M4-side peripheral clock gate. */
    __HAL_RCC_I2C5_CLK_ENABLE();

    tof_i2c5.Instance = I2C5;

    /*
     * 64 MHz input, standard-mode timing:
     * PRESC=7, SCLDEL=10, SDADEL=2, SCLH=39, SCLL=39.
     * High/low counter durations are 5 us each.
     * Actual SCL also includes filtering and signal edge delays.
     */
    tof_i2c5.Init.Timing = 0x70A22727U;
    tof_i2c5.Init.OwnAddress1 = 0U;
    tof_i2c5.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    tof_i2c5.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    tof_i2c5.Init.OwnAddress2 = 0U;
    tof_i2c5.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    tof_i2c5.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    tof_i2c5.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    stage = "INIT";
    log_info("TOF_PROBE_V1: initializing I2C5\r\n");
    result = HAL_I2C_Init(&tof_i2c5);
    if (result != HAL_OK)
        goto failed;

    stage = "ANALOG_FILTER";
    result = HAL_I2CEx_ConfigAnalogFilter(
        &tof_i2c5, I2C_ANALOGFILTER_ENABLE);
    if (result != HAL_OK)
        goto failed;

    stage = "DIGITAL_FILTER";
    result = HAL_I2CEx_ConfigDigitalFilter(&tof_i2c5, 0U);
    if (result != HAL_OK)
        goto failed;

    HAL_Delay(1U);

    scl = (unsigned int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_11);
    sda = (unsigned int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_12);

    stage = "BUS_IDLE";
    if (scl == 0U || sda == 0U)
    {
        result = HAL_BUSY;
        goto failed;
    }

    /*
     * VL53L0X 7-bit address: 0x29.
     * STM32 HAL expects the address shifted left once: 0x52.
     */
    stage = "ADDRESS_ACK";
    result = HAL_I2C_IsDeviceReady(
        &tof_i2c5, (0x29U << 1), 2U, 20U);
    if (result != HAL_OK)
        goto failed;

    stage = "READ_ID";
    result = HAL_I2C_Mem_Read(
        &tof_i2c5,
        (0x29U << 1),
        0xC0U,
        I2C_MEMADD_SIZE_8BIT,
        id,
        sizeof(id),
        20U);
    if (result != HAL_OK)
        goto failed;

    snprintf(reply, capacity,
             "TOF,PROBE,%s,KCLK=%lu,SCL=%u,SDA=%u,"
             "ID=0x%02X%02X,REV=0x%02X\n",
             (id[0] == 0xEEU && id[1] == 0xAAU)
                 ? "OK" : "ID_MISMATCH",
             (unsigned long)clock_hz,
             scl, sda,
             (unsigned int)id[0],
             (unsigned int)id[1],
             (unsigned int)id[2]);
    return;

failed:
    snprintf(reply, capacity,
             "TOF,PROBE,ERROR,STAGE=%s,HAL=%u,"
             "ERR=0x%08lX,KCLK=%lu,SCL=%u,SDA=%u\n",
             stage,
             (unsigned int)result,
             (unsigned long)HAL_I2C_GetError(&tof_i2c5),
             (unsigned long)clock_hz,
             scl, sda);
}

/* TOF_STREAM_V1: all sensor work runs in the M4 main loop. */
#define TOF_PERIOD_MS       100U
#define TOF_TX_RETRY_MS      20U
#define TOF_COMMAND_SIZE    32U
#define TOF_COMMAND_COUNT    4U
#define TOF_VERSION "TOF_STREAM_V1"

static char tof_commands[TOF_COMMAND_COUNT][TOF_COMMAND_SIZE];
static unsigned int tof_command_head, tof_command_tail, tof_command_count;
static char tof_line[TOF_COMMAND_SIZE];
static size_t tof_line_length;
static uint8_t tof_line_drop, tof_command_error;
static uint8_t tof_running, tof_bus_ready;
static uint32_t tof_next_due, tof_sequence, tof_tx_drop;
static char tof_control_tx[256], tof_data_tx[256];
static int tof_control_length, tof_data_length;
static uint32_t tof_control_attempt, tof_data_attempt;

static void TOF_CommandByte(uint8_t byte)
{
    if (byte == '\n') {
        if (tof_line_drop) {
            tof_command_error = 1U;
        } else {
            if (tof_line_length && tof_line[tof_line_length - 1U] == '\r')
                tof_line_length--;

            tof_line[tof_line_length] = '\0';

            if (tof_line_length) {
                if (tof_command_count < TOF_COMMAND_COUNT) {
                    memcpy(tof_commands[tof_command_tail], tof_line,
                           tof_line_length + 1U);
                    tof_command_tail =
                        (tof_command_tail + 1U) % TOF_COMMAND_COUNT;
                    tof_command_count++;
                } else {
                    tof_command_error = 2U;
                }
            }
        }

        tof_line_length = 0U;
        tof_line_drop = 0U;
        return;
    }

    if (tof_line_drop)
        return;

    if (byte == 0U || tof_line_length >= sizeof(tof_line) - 1U) {
        tof_line_drop = 1U;
        return;
    }

    tof_line[tof_line_length++] = (char)byte;
}

static void TOF_FlushTx(void)
{
    uint32_t now = HAL_GetTick();

    if (tof_control_length &&
        (uint32_t)(now - tof_control_attempt) >= TOF_TX_RETRY_MS) {
        tof_control_attempt = now;

        if (rpmsg_trysend(&huart1.ept, tof_control_tx,
                         tof_control_length) == tof_control_length)
            tof_control_length = 0;
    }

    if (rpmsg0_ready && tof_data_length &&
        (uint32_t)(now - tof_data_attempt) >= TOF_TX_RETRY_MS) {
        tof_data_attempt = now;

        if (rpmsg_trysend(&huart0.ept, tof_data_tx,
                         tof_data_length) == tof_data_length)
            tof_data_length = 0;
    }
}

static void TOF_HandleCommand(const char *command)
{
    if (strcmp(command, "TOF,STOP") == 0) {
        tof_running = 0U;

        if (tof_data_length)
            tof_tx_drop++;

        tof_data_length = 0;

        snprintf(tof_control_tx, sizeof(tof_control_tx),
                 "TOF,ACK,STOP,FW=%s\n", TOF_VERSION);

    } else if (strcmp(command, "TOF,STATUS") == 0) {
        snprintf(tof_control_tx, sizeof(tof_control_tx),
                 "TOF,STATUS,RUN=%u,SEQ=%lu,TX_DROP=%lu,FW=%s\n",
                 (unsigned int)tof_running,
                 (unsigned long)tof_sequence,
                 (unsigned long)tof_tx_drop,
                 TOF_VERSION);

    } else if (strcmp(command, "TOF,START") == 0) {
        if (!rpmsg0_ready) {
            snprintf(tof_control_tx, sizeof(tof_control_tx),
                     "TOF,ACK,ERROR,REASON=SEND_READY_ON_RPMSG0\n");
        } else {
            if (!tof_bus_ready) {
                TOF_Probe_Run(tof_control_tx, sizeof(tof_control_tx));

                if (strncmp(tof_control_tx, "TOF,PROBE,OK,", 13U) != 0)
                    return;

                tof_bus_ready = 1U;
            }

            if (!tof_running)
                tof_next_due = HAL_GetTick();

            tof_running = 1U;

            snprintf(tof_control_tx, sizeof(tof_control_tx),
                     "TOF,ACK,START,PERIOD_MS=%u,FW=%s\n",
                     (unsigned int)TOF_PERIOD_MS, TOF_VERSION);
        }

    } else if (strcmp(command, "TOF,PROBE") == 0 ||
               strcmp(command, "TOF,RANGE") == 0) {
        if (tof_running) {
            snprintf(tof_control_tx, sizeof(tof_control_tx),
                     "TOF,ACK,ERROR,REASON=STOP_STREAM_FIRST\n");
        } else {
            TOF_Probe_Run(tof_control_tx, sizeof(tof_control_tx));

            tof_bus_ready =
                (strncmp(tof_control_tx, "TOF,PROBE,OK,", 13U) == 0);

            if (tof_bus_ready && strcmp(command, "TOF,RANGE") == 0) {
                MP1_VL53L0X_ReadOnce(&tof_i2c5, tof_control_tx,
                                    sizeof(tof_control_tx));

                if (strncmp(tof_control_tx, "TOF,RANGE,ERROR,", 16U) == 0)
                    tof_bus_ready = 0U;
            }
        }

    } else {
        snprintf(tof_control_tx, sizeof(tof_control_tx),
                 "TOF,ACK,ERROR,REASON=UNKNOWN_COMMAND\n");
    }
}

static void TOF_Stream_Process(void)
{
    TOF_FlushTx();

    if (!tof_control_length) {
        if (tof_command_error) {
            snprintf(tof_control_tx, sizeof(tof_control_tx),
                     "TOF,ACK,ERROR,REASON=%s\n",
                     tof_command_error == 1U
                         ? "BAD_LINE" : "COMMAND_QUEUE_FULL");

            tof_command_error = 0U;
            tof_control_length = (int)strlen(tof_control_tx);

        } else if (tof_command_count) {
            TOF_HandleCommand(tof_commands[tof_command_head]);

            tof_command_head =
                (tof_command_head + 1U) % TOF_COMMAND_COUNT;
            tof_command_count--;

            tof_control_length = (int)strlen(tof_control_tx);
        }

        if (tof_control_length)
            tof_control_attempt = HAL_GetTick() - TOF_TX_RETRY_MS;
    }

    TOF_FlushTx();

    uint32_t now = HAL_GetTick();

    if (!tof_running || (int32_t)(now - tof_next_due) < 0)
        return;

    char reading[192];
    const char *payload;
    uint32_t sample_ms = now;

    tof_next_due += TOF_PERIOD_MS;
    tof_sequence++;

    MP1_VL53L0X_ReadOnce(&tof_i2c5, reading, sizeof(reading));

    if (strncmp(reading, "TOF,RANGE,", 10U) == 0)
        payload = reading + 10U;
    else
        payload = "ERROR,STAGE=FORMAT,FW=" TOF_VERSION "\n";

    if (tof_data_length)
        tof_tx_drop++;

    int length = snprintf(
        tof_data_tx, sizeof(tof_data_tx),
        "TOF,DATA,SEQ=%lu,MS=%lu,TX_DROP=%lu,%s",
        (unsigned long)tof_sequence,
        (unsigned long)sample_ms,
        (unsigned long)tof_tx_drop,
        payload
    );

    if (length < 0 || (size_t)length >= sizeof(tof_data_tx)) {
        length = snprintf(tof_data_tx, sizeof(tof_data_tx),
                          "TOF,DATA,ERROR,STAGE=FORMAT,FW=%s\n",
                          TOF_VERSION);
        tof_running = 0U;
    }

    tof_data_length = length;
    tof_data_attempt = HAL_GetTick() - TOF_TX_RETRY_MS;

    /* A failed I2C/API call stops the stream. START permits a retry. */
    if (strncmp(payload, "ERROR,", 6U) == 0) {
        tof_running = 0U;
        tof_bus_ready = 0U;
    }

    /* Skip missed slots instead of issuing catch-up measurements. */
    now = HAL_GetTick();

    if ((int32_t)(now - tof_next_due) >= 0)
        tof_next_due = now + TOF_PERIOD_MS;

    TOF_FlushTx();
}

/* MP1_TOF_STREAM_END */
/* USER CODE END 0 */
/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */


  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initialize the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
    if(IS_ENGINEERING_BOOT_MODE())
  {
    /* Configure the system clock */
    SystemClock_Config();
  }

  log_info("Cortex-M4 boot successful with STM32Cube FW version: v%ld.%ld.%ld \r\n",
                                            (long)((HAL_GetHalVersion() >> 24) & 0x000000FF),
                                            (long)((HAL_GetHalVersion() >> 16) & 0x000000FF),
                                            (long)((HAL_GetHalVersion() >> 8) & 0x000000FF));
  /*HW semaphore Clock enable*/
  __HAL_RCC_HSEM_CLK_ENABLE();
  BSP_LED_Init(LED7);
  BSP_LED_Off(LED7);

  /* USER CODE END Init */

  /* IPCC initialisation */
   MX_IPCC_Init();
  /* OpenAmp initialisation ---------------------------------*/
  MX_OPENAMP_Init(RPMSG_REMOTE, NULL);

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */

  /* USER CODE BEGIN 2 */
  /*
   * Create Virtual UART device
   * defined by a rpmsg channel attached to the remote device
   */
  log_info("Virtual UART0 OpenAMP-rpmsg channel creation\r\n");
  if (VIRT_UART_Init(&huart0) != VIRT_UART_OK) {
    log_err("VIRT_UART_Init UART0 failed.\r\n");
    Error_Handler();
  }

  log_info("Virtual UART1 OpenAMP-rpmsg channel creation\r\n");
  if (VIRT_UART_Init(&huart1) != VIRT_UART_OK) {
    log_err("VIRT_UART_Init UART1 failed.\r\n");
    Error_Handler();
  }

  /*Need to register callback for message reception by channels*/
  if(VIRT_UART_RegisterCallback(&huart0, VIRT_UART_RXCPLT_CB_ID, VIRT_UART0_RxCpltCallback) != VIRT_UART_OK)
  {
   Error_Handler();
  }
  if(VIRT_UART_RegisterCallback(&huart1, VIRT_UART_RXCPLT_CB_ID, VIRT_UART1_RxCpltCallback) != VIRT_UART_OK)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
    /* USER CODE BEGIN WHILE */
  while (1)
  {
    OPENAMP_check_for_message();
    TOF_Stream_Process();

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /**Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_MEDIUMHIGH);

  /**Initializes the CPU, AHB and APB busses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSE
                |RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS_DIG;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.HSIDivValue = RCC_HSI_DIV1;

  /**PLL1 Config
  */
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLL12SOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 3;
  RCC_OscInitStruct.PLL.PLLN = 81;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 1;
  RCC_OscInitStruct.PLL.PLLR = 1;
  RCC_OscInitStruct.PLL.PLLFRACV = 0x800;
  RCC_OscInitStruct.PLL.PLLMODE = RCC_PLL_FRACTIONAL;
  RCC_OscInitStruct.PLL.RPDFN_DIS = RCC_RPDFN_DIS_DISABLED;
  RCC_OscInitStruct.PLL.TPDFN_DIS = RCC_TPDFN_DIS_DISABLED;

    /**PLL2 Config
    */
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL2.PLLSource = RCC_PLL12SOURCE_HSE;
  RCC_OscInitStruct.PLL2.PLLM = 3;
  RCC_OscInitStruct.PLL2.PLLN = 66;
  RCC_OscInitStruct.PLL2.PLLP = 2;
  RCC_OscInitStruct.PLL2.PLLQ = 1;
  RCC_OscInitStruct.PLL2.PLLR = 1;
  RCC_OscInitStruct.PLL2.PLLFRACV = 0x1400;
  RCC_OscInitStruct.PLL2.PLLMODE = RCC_PLL_FRACTIONAL;
  RCC_OscInitStruct.PLL2.RPDFN_DIS = RCC_RPDFN_DIS_DISABLED;
  RCC_OscInitStruct.PLL2.TPDFN_DIS = RCC_TPDFN_DIS_DISABLED;

    /**PLL3 Config
    */
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL3.PLLSource = RCC_PLL3SOURCE_HSE;
  RCC_OscInitStruct.PLL3.PLLM = 2;
  RCC_OscInitStruct.PLL3.PLLN = 34;
  RCC_OscInitStruct.PLL3.PLLP = 2;
  RCC_OscInitStruct.PLL3.PLLQ = 17;
  RCC_OscInitStruct.PLL3.PLLR = 37;
  RCC_OscInitStruct.PLL3.PLLRGE = RCC_PLL3IFRANGE_1;
  RCC_OscInitStruct.PLL3.PLLFRACV = 0x1A04;
  RCC_OscInitStruct.PLL3.PLLMODE = RCC_PLL_FRACTIONAL;
  RCC_OscInitStruct.PLL3.RPDFN_DIS = RCC_RPDFN_DIS_DISABLED;
  RCC_OscInitStruct.PLL3.TPDFN_DIS = RCC_TPDFN_DIS_DISABLED;

    /**PLL4 Config
    */
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL4.PLLSource = RCC_PLL4SOURCE_HSE;
  RCC_OscInitStruct.PLL4.PLLM = 4;
  RCC_OscInitStruct.PLL4.PLLN = 99;
  RCC_OscInitStruct.PLL4.PLLP = 6;
  RCC_OscInitStruct.PLL4.PLLQ = 8;
  RCC_OscInitStruct.PLL4.PLLR = 8;
  RCC_OscInitStruct.PLL4.PLLRGE = RCC_PLL4IFRANGE_0;
  RCC_OscInitStruct.PLL4.PLLFRACV = 0;
  RCC_OscInitStruct.PLL4.PLLMODE = RCC_PLL_INTEGER;
  RCC_OscInitStruct.PLL4.RPDFN_DIS = RCC_RPDFN_DIS_DISABLED;
  RCC_OscInitStruct.PLL4.TPDFN_DIS = RCC_TPDFN_DIS_DISABLED;

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
  Error_Handler();
  }
  /**RCC Clock Config
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_ACLK
                |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                |RCC_CLOCKTYPE_PCLK3|RCC_CLOCKTYPE_PCLK4
                |RCC_CLOCKTYPE_PCLK5|RCC_CLOCKTYPE_MPU;
  RCC_ClkInitStruct.MPUInit.MPU_Clock = RCC_MPUSOURCE_PLL1;
  RCC_ClkInitStruct.MPUInit.MPU_Div = RCC_MPU_DIV2;
  RCC_ClkInitStruct.AXISSInit.AXI_Clock = RCC_AXISSOURCE_PLL2;
  RCC_ClkInitStruct.AXISSInit.AXI_Div = RCC_AXI_DIV1;
  RCC_ClkInitStruct.MCUInit.MCU_Clock = RCC_MCUSSOURCE_PLL3;
  RCC_ClkInitStruct.MCUInit.MCU_Div = RCC_MCU_DIV1;
  RCC_ClkInitStruct.APB4_Div = RCC_APB4_DIV2;
  RCC_ClkInitStruct.APB5_Div = RCC_APB5_DIV4;
  RCC_ClkInitStruct.APB1_Div = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2_Div = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB3_Div = RCC_APB3_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK)
  {
  Error_Handler();
  }

  /**Set the HSE division factor for RTC clock
  */
  __HAL_RCC_RTC_HSEDIV(24);
}

/**
  * @brief IPPC Initialization Function
  * @param None
  * @retval None
  */
static void MX_IPCC_Init(void)
{

  hipcc.Instance = IPCC;
  if (HAL_IPCC_Init(&hipcc) != HAL_OK)
  {
     Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* USER CODE BEGIN 4 */
void VIRT_UART0_RxCpltCallback(VIRT_UART_HandleTypeDef *huart)
{
    (void)huart;
    rpmsg0_ready = 1U;
}

void VIRT_UART1_RxCpltCallback(VIRT_UART_HandleTypeDef *huart)
{
    const uint8_t *data = huart->pRxBuffPtr;
    for (size_t i = 0U; i < huart->RxXferSize; i++)
        TOF_CommandByte(data[i]);
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @param  file: The file name as string.
  * @param  line: The line in file as a number.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  log_err("Error_Handler");
  BSP_LED_Off(LED7);
  while(1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t* file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  log_err("OOOps: file %s, line %d\r\n", __FILE__, __LINE__);
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
