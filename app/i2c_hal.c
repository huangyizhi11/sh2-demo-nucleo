/*
 * Copyright 2018 Hillcrest Laboratories, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License and 
 * any applicable agreements you may have with Hillcrest Laboratories, Inc.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * I2C-based HALs for SH2 and DFU.
 */

#include "sh2_hal_init.h"
#include "sh2_hal.h"
#include "sh2_err.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_tim.h"
#include "stm32f4xx_hal_i2c.h"

#define CLKSEL0_PORT GPIOA
#define CLKSEL0_PIN  GPIO_PIN_8

#define RSTN_PORT GPIOB
#define RSTN_PIN  GPIO_PIN_4

#define BOOTN_PORT GPIOB
#define BOOTN_PIN  GPIO_PIN_5

#define PS0_WAKEN_PORT GPIOB
#define PS0_WAKEN_PIN  GPIO_PIN_10

#define PS1_PORT GPIOB
#define PS1_PIN  GPIO_PIN_0

#define INTN_PORT GPIOA
#define INTN_PIN GPIO_PIN_10

// Keep reset asserted this long.
// (Some targets have a long RC decay on reset.)
#define RESET_DELAY_US (10000)

// Wait up to this long to see first interrupt from SH
#define START_DELAY_US (2000000)

// Wait this long before assuming bootloader is ready
#define DFU_BOOT_DELAY_US (50000)

// How many bytes to read when reading the length field
#define READ_LEN (2)

// ------------------------------------------------------------------------
// Private types

enum BusState_e {
    BUS_INIT,
    BUS_IDLE,
    BUS_READING_LEN,
    BUS_GOT_LEN,
    BUS_READING_TRANSFER,
    BUS_WRITING,
    BUS_READING_DFU,
    BUS_WRITING_DFU,
};

#define ADDR_SH2_0 (0x4A)
#define ADDR_SH2_1 (0x4B)

#define ADDR_DFU_0 (0x28)
#define ADDR_DFU_1 (0x29)

// ------------------------------------------------------------------------
// Private data

static bool isOpen = false;

// Timer handle
TIM_HandleTypeDef tim2;

// I2C Peripheral, I2C1
I2C_HandleTypeDef i2c;

enum BusState_e i2cBusState;

volatile uint32_t rxTimestamp_uS;            // timestamp of INTN event

// Receive Buffer
static uint8_t rxBuf[SH2_HAL_MAX_TRANSFER_IN];      // data
static uint32_t rxBufLen;   // valid bytes stored in rxBuf (0 when buf empty)
static uint16_t payloadLen;

// Transmit buffer
static uint8_t txBuf[SH2_HAL_MAX_TRANSFER_OUT];

// True after INTN observed, until read starts
static bool rxDataReady;
static uint32_t discards = 0;

// I2C Addr (in 7 MSB positions)
static uint16_t i2cAddr;

// True between asserting reset and seeing first INTN assertion
static volatile bool inReset;

static sh2_Hal_t sh2Hal;
static sh2_Hal_t dfuHal;

// ------------------------------------------------------------------------
// Private methods

static void enableInts(void)
{
    // Enable INTN interrupt
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

    // Enable I2C interrupts
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
}

static void disableInts(void)
{
    // Disable I2C interrupts
    HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);

    // Disable INTN interrupt line
    HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
}

static void enableI2cInts(void)
{
    // Enable I2C interrupts
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
}

static void disableI2cInts(void)
{
    // Disable I2C interrupts
    HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
}

static void hal_init_gpio(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    
    /* Configure PS0_WAKEN */
    HAL_GPIO_WritePin(PS0_WAKEN_PORT, PS0_WAKEN_PIN, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = PS0_WAKEN_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(PS0_WAKEN_PORT, &GPIO_InitStruct);

    /* Configure PS1 */
    HAL_GPIO_WritePin(PS1_PORT, PS1_PIN, GPIO_PIN_SET);
    GPIO_InitStruct.Pin = PS1_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(PS1_PORT, &GPIO_InitStruct);

    /* Configure RSTN*/
    HAL_GPIO_WritePin(RSTN_PORT, RSTN_PIN, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = RSTN_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RSTN_PORT, &GPIO_InitStruct);

    /* Configure BOOTN */
    HAL_GPIO_WritePin(BOOTN_PORT, BOOTN_PIN, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = BOOTN_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOOTN_PORT, &GPIO_InitStruct);

    /*Configure GPIO pin : INTN */
    GPIO_InitStruct.Pin = INTN_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(INTN_PORT, &GPIO_InitStruct);

    /*Configure GPIO pin : CLKSEL0_PIN */
    /* Set CLKSEL0 to 0 : FSP200 should use crystal for timing. */
    HAL_GPIO_WritePin(CLKSEL0_PORT, CLKSEL0_PIN, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = CLKSEL0_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(CLKSEL0_PORT, &GPIO_InitStruct);

    /* EXTI interrupt init*/
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
}

static void hal_init_i2c(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    
    // Configure GPIO Pins for use with I2C
    // PB8 : I2C1_SCL
    // PB9 : I2C1_SDA 
    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // Peripheral clock enable
    __HAL_RCC_I2C1_CLK_ENABLE();

    i2c.Instance = I2C1;
    i2c.Init.ClockSpeed = 400000;
    i2c.Init.DutyCycle = I2C_DUTYCYCLE_2;
    i2c.Init.OwnAddress1 = 0;
    i2c.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLED;
    i2c.Init.OwnAddress2 = 0;
    i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLED;
    i2c.Init.NoStretchMode = I2C_NOSTRETCH_DISABLED;
    
    HAL_I2C_Init(&i2c);

    // Set Priority for I2C IRQ and enable
    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 5, 0);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, 5, 0);
}

static void hal_init_timer(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    
    // Prescale to get 1 count per uS
    uint32_t prescaler = (uint32_t)((HAL_RCC_GetPCLK2Freq() / 1000000) - 1);

    tim2.Instance = TIM2;
    tim2.Init.Period = 0xFFFFFFFF;
    tim2.Init.Prescaler = prescaler;
    tim2.Init.ClockDivision = 0;
    tim2.Init.CounterMode = TIM_COUNTERMODE_UP;

    HAL_TIM_Base_Init(&tim2);
    HAL_TIM_Base_Start(&tim2);
}

static void hal_init_hw(void)
{
    hal_init_timer();
    hal_init_gpio();
    hal_init_i2c();
}

static void bootn(bool state)
{
    HAL_GPIO_WritePin(BOOTN_PORT, BOOTN_PIN, 
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void rstn(bool state)
{
    HAL_GPIO_WritePin(RSTN_PORT, RSTN_PIN, 
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ps0_waken(bool state)
{
    HAL_GPIO_WritePin(PS0_WAKEN_PORT, PS0_WAKEN_PIN, 
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ps1(bool state)
{
    HAL_GPIO_WritePin(PS1_PORT, PS1_PIN, 
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint32_t timeNowUs(void)
{
    return __HAL_TIM_GET_COUNTER(&tim2);
}

static void delay_us(uint32_t t)
{
    uint32_t now = timeNowUs();
    uint32_t start = now;
    while ((now - start) < t)
    {
        now = timeNowUs();
    }
}

static void reset_delay_us(uint32_t t)
{
    uint32_t now = timeNowUs();
    uint32_t start = now;
    while (((now - start) < t) && (inReset))
    {
        now = timeNowUs();
    }
}

// ----------------------------------------------------------------------------------
// Callbacks for ISR, I2C Operations
// ----------------------------------------------------------------------------------

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *pI2c)
{
    // Read completed
    if (i2cBusState == BUS_READING_LEN)
    {
        // Len of payload is available, decide how long to do next read
        uint16_t len = (rxBuf[0] + (rxBuf[1] << 8)) & ~0x8000;
        if (len > sizeof(rxBuf))
        {
            // read only what will fit in rxBuf
            payloadLen = sizeof(rxBuf);
        }
        else
        {
            payloadLen = len;
        }
        i2cBusState = BUS_GOT_LEN;
    }
    else if (i2cBusState == BUS_READING_TRANSFER)
    {
        // rxBuf is now ready for client.
        rxBufLen = payloadLen;

        // Nothing left to do
        i2cBusState = BUS_IDLE;
    }
    else if (i2cBusState == BUS_READING_DFU)
    {
        // Transition back to idle state
        rxBufLen = payloadLen;
        i2cBusState = BUS_IDLE;
    }
}

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *i2c)
{
    if (i2cBusState == BUS_WRITING)
    {
        // Switch back to bus idle
        i2cBusState = BUS_IDLE;
    }
    else if (i2cBusState == BUS_WRITING_DFU)
    {
        // Switch back to bus idle
        i2cBusState = BUS_IDLE;
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t n)
{
    if (i2cBusState == BUS_INIT)
    {
        // No active hal, ignore this call, don't crash.
        return;
    }
    
    rxTimestamp_uS = timeNowUs();
    inReset = false;

    // Start read, if possible
    if (i2cBusState == BUS_IDLE)
    {
        if (rxBufLen > 0)
        {
            // Discard earlier payload!
            discards++;
            rxBufLen = 0;
        }
        
        // Read payload len
        i2cBusState = BUS_READING_LEN;
        HAL_I2C_Master_Receive_IT(&i2c, i2cAddr, rxBuf, READ_LEN);
    }
    else if (i2cBusState == BUS_GOT_LEN)
    {
        // Read payload
        i2cBusState = BUS_READING_TRANSFER;
        HAL_I2C_Master_Receive_IT(&i2c, i2cAddr, rxBuf, payloadLen);
    }
    else
    {
        // We can't start read immediately, set flag so it gets done later.
        rxDataReady = true;
    }
}

// Handle INTN Interrupt through STM32 HAL
// (It, in turn, calls HAL_GPIO_EXTI_Callback, above)
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_10);
}

// Handle I2C1 EV IRQ, passing it to STM32 HAL library
void I2C1_EV_IRQHandler(void)
{
    HAL_I2C_EV_IRQHandler(&i2c);
}

// Handle I2C1 ER IRQ, passing it to STM32 HAL library
void I2C1_ER_IRQHandler(void)
{
    HAL_I2C_ER_IRQHandler(&i2c);
}

// ------------------------------------------------------------------------
// SH2 HAL Methods

static int sh2_i2c_hal_open(sh2_Hal_t *self)
{
    if (isOpen)
    {
        return SH2_ERR;
    }

    i2cBusState = BUS_INIT;
    i2cAddr = ADDR_SH2_0 << 1;

    isOpen = true;

    // Init hardware peripherals
    hal_init_hw();

    // Hold in reset, not for DFU
    rstn(false);

    inReset = true;  // will change back to false when INTN serviced

    enableInts();

    // Delay for RESET_DELAY_US to ensure reset takes effect
    delay_us(RESET_DELAY_US);
    
    // transition to idle state
    i2cBusState = BUS_IDLE;

    // Clear rx, tx buffers
    rxBufLen = 0;
    rxDataReady = false;

    // To boot in SHTP-I2C mode, must have PS1=0, PS0=0.
    // PS1 is set via jumper.
    // PS0 will be 0 if PS0 jumper is 0 OR (PS1 jumper is 1 AND PS0_WAKEN sig is 0)
    // So we set PS0_WAKEN signal to 0 just in case PS1 jumper is in 1 position.
    ps0_waken(false);
    ps1(false);

    // Deassert BOOT, don't go into bootloader
    bootn(true);
    
    // Deassert reset
    rstn(1);

    // Wait for INTN to be asserted
    reset_delay_us(START_DELAY_US);

    return SH2_OK;
}

static void sh2_i2c_hal_close(sh2_Hal_t *self)
{
    // Hold sensor hub in reset
    rstn(false);
    bootn(true);

    i2cBusState = BUS_INIT;

    // Disable interrupts
    disableInts();
    
    // Deinit I2C peripheral
    HAL_I2C_DeInit(&i2c);
    
    // Deinit timer
    __HAL_TIM_DISABLE(&tim2);
    
    isOpen = false;
}

static int sh2_i2c_hal_read(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len, uint32_t *t)
{
    int retval = 0;
    
    disableInts();
    if (rxBufLen > 0)
    {
        // There is data to be had.
        if (len < rxBufLen)
        {
            // Client buffer too small!
            // Discard what was read
            rxBufLen = 0;
            retval = SH2_ERR_BAD_PARAM;
        }
        else
        {
            // Copy data to the client buffer
            memcpy(pBuffer, rxBuf, rxBufLen);
            retval = rxBufLen;
            rxBufLen = 0;
            *t = rxTimestamp_uS;
        }
    }
    enableInts();

    // if more data is ready, start reading it
    if (rxDataReady)
    {
        if ((i2cBusState == BUS_IDLE))
        {
            rxDataReady = false;
            i2cBusState = BUS_READING_LEN;
            HAL_I2C_Master_Receive_IT(&i2c, i2cAddr, rxBuf, READ_LEN);
        }
        else if ((i2cBusState == BUS_GOT_LEN))
        {
            rxDataReady = false;
            i2cBusState = BUS_READING_TRANSFER;
            HAL_I2C_Master_Receive_IT(&i2c, i2cAddr, rxBuf, payloadLen);
        }
    }

    return retval;
}

static int sh2_i2c_hal_write(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len)
{
    int retval = 0;
    
    // Validate parameters
    if ((pBuffer == 0) || (len == 0) || (len > sizeof(txBuf)))
    {
        return SH2_ERR_BAD_PARAM;
    }

    // Disable I2C Interrupt for a moment so busState can't change
    disableI2cInts();
    
    if (i2cBusState == BUS_IDLE)
    {
        i2cBusState = BUS_WRITING;

        // Set up write operation
        memcpy(txBuf, pBuffer, len);
        HAL_I2C_Master_Transmit_IT(&i2c, i2cAddr, txBuf, len);

        retval = len;
    }

    // re-enable interrupts
    enableI2cInts();
    
    return retval;
}

static uint32_t sh2_i2c_hal_getTimeUs(sh2_Hal_t *self)
{
    return timeNowUs();
}

// ------------------------------------------------------------------------
// DFU HAL Methods

static int dfu_i2c_hal_open(sh2_Hal_t *self)
{
    if (isOpen)
    {
        return SH2_ERR;
    }

    i2cBusState = BUS_INIT;
    i2cAddr = ADDR_DFU_0 << 1;
    isOpen = true;

    // Init hardware peripherals
    hal_init_hw();

    // Hold in reset, for DFU
    rstn(false);
    inReset = true;

    // Delay for RESET_DELAY_US to ensure reset takes effect
    delay_us(RESET_DELAY_US);
    
    // Clear rx, tx buffers
    rxBufLen = 0;
    rxDataReady = false;
    
    i2cBusState = BUS_IDLE;
    
    // Enable interrupts.
    enableI2cInts();

    // To boot in I2C mode, must have PS1=0, PS0=0.
    // PS1 is set via jumper.
    // PS0 will be 0 if PS0 jumper is 0 OR (PS1 jumper is 1 AND PS0_WAKEN sig is 0)
    // So we set PS0_WAKEN signal to 0 just in case PS1 jumper is in 1 position.
    ps0_waken(false);
    ps1(false);

    // Boot into DFU mode
    bootn(false);

    // deassert reset
    rstn(true);

    // Wait for bootloader to be ready
    delay_us(DFU_BOOT_DELAY_US);

    return SH2_OK;
}

static void dfu_i2c_hal_close(sh2_Hal_t *self)
{
    // Hold sensor hub in reset, for dfu
    rstn(false);
    
    // Disable interrupts
    disableInts();
    
    // Deinit I2C peripheral
    HAL_I2C_DeInit(&i2c);

    // Deinit timer
    __HAL_TIM_DISABLE(&tim2);
    
    isOpen = false;
}

static int dfu_i2c_hal_read(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len, uint32_t *t)
{
    int retval = 0;
    
    if ((i2cBusState != BUS_READING_DFU) && (rxBufLen > 0))
    {
        // There is data to be had.
        if (len < rxBufLen)
        {
            // Client buffer too small!
            // Discard what was read
            rxBufLen = 0;
            retval = SH2_ERR_BAD_PARAM;
        }
        else
        {
            // Copy data to the client buffer
            memcpy(pBuffer, rxBuf, rxBufLen);
            retval = rxBufLen;
            *t = rxTimestamp_uS;
            rxBufLen = 0;
        }
    }
    else
    {
        // Initiate read if none already in progress.
        if (i2cBusState == BUS_IDLE)
        {
            i2cBusState = BUS_READING_DFU;
            payloadLen = len;
            HAL_I2C_Master_Receive_IT(&i2c, i2cAddr, rxBuf, len);
        }
    }

    return retval;
}

static int dfu_i2c_hal_write(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len)
{
    int retval = 0;
    
    // Validate parameters
    if ((pBuffer == 0) || (len == 0) || (len > sizeof(txBuf)))
    {
        return SH2_ERR_BAD_PARAM;
    }

    disableI2cInts();
    
    if (i2cBusState == BUS_IDLE)
    {
        i2cBusState = BUS_WRITING_DFU;

        // Set up write operation
        memcpy(txBuf, pBuffer, len);
        HAL_I2C_Master_Transmit_IT(&i2c, i2cAddr, txBuf, len);

        retval = len;
    }
    
    // re-enable interrupts
    enableI2cInts();
    
    return retval;
}

static uint32_t dfu_i2c_hal_getTimeUs(sh2_Hal_t *self)
{
    return timeNowUs();
}

// ------------------------------------------------------------------------
// Public methods

sh2_Hal_t *sh2_hal_init(void)
{
    // Set up the HAL reference object for the client
    sh2Hal.open = sh2_i2c_hal_open;
    sh2Hal.close = sh2_i2c_hal_close;
    sh2Hal.read = sh2_i2c_hal_read;
    sh2Hal.write = sh2_i2c_hal_write;
    sh2Hal.getTimeUs = sh2_i2c_hal_getTimeUs;

    return &sh2Hal;
}

sh2_Hal_t *dfu_hal_init(void)
{
    // Set up the HAL reference object for the client
    dfuHal.open = dfu_i2c_hal_open;
    dfuHal.close = dfu_i2c_hal_close;
    dfuHal.read = dfu_i2c_hal_read;
    dfuHal.write = dfu_i2c_hal_write;
    dfuHal.getTimeUs = dfu_i2c_hal_getTimeUs;

    return &dfuHal;
}
