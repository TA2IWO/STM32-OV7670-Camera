/* STM32F746ZG + OV7670 grayscale camera streamer.
 * Requires the CubeMX-generated HAL, MSP and interrupt handlers.
 * See README.md for wiring and configuration.
 */
#include "main.h"
#include <stdio.h>
#include <string.h>

#define WIDTH 160U
#define HEIGHT 120U
#define CAMERA_ADDR 0x42U
#define BAUD_RATE 2000000U

DCMI_HandleTypeDef hdcmi;
DMA_HandleTypeDef hdma_dcmi;
ETH_HandleTypeDef heth;
I2C_HandleTypeDef hi2c1;
TIM_HandleTypeDef htim1;
UART_HandleTypeDef huart3;
PCD_HandleTypeDef hpcd_USB_OTG_FS;

static uint32_t frame[WIDTH * HEIGHT / 2U];
static volatile uint8_t frameReady, frameError;
static uint8_t rotation;
static GPIO_PinState buttonIdle, buttonLast, buttonStable;
static uint32_t buttonTick;

void SystemClock_Config(void);
void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);
void Error_Handler(void);

static void UART_Print(const char *msg)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 1000);
}

static void Button_Init(void)
{
    buttonIdle = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13);
    buttonLast = buttonStable = buttonIdle;
    buttonTick = HAL_GetTick();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
}

static void Button_Update(void)
{
    GPIO_PinState now = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13);
    uint32_t tick = HAL_GetTick();

    if (now != buttonLast) {
        buttonLast = now;
        buttonTick = tick;
    }
    if ((uint32_t)(tick - buttonTick) >= 30U && now != buttonStable) {
        buttonStable = now;
        if (buttonStable != buttonIdle) {
            rotation = (rotation + 1U) & 3U;
            HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14);
        }
    }
}

static void Camera_Write(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    if (HAL_I2C_Master_Transmit(&hi2c1, CAMERA_ADDR, data, 2, 100) != HAL_OK) {
        UART_Print("CAMERA WRITE ERROR\r\n");
        Error_Handler();
    }
}

static void Camera_Init(void)
{
    Camera_Write(0x12, 0x80);
    HAL_Delay(200);
    const uint8_t config[][2] = {
        {0x11,0x01},{0x12,0x04},{0x8C,0x00},{0x04,0x00},
        {0x40,0xD0},{0x3A,0x04},{0x0C,0x04},{0x3E,0x1A},
        {0x70,0x3A},{0x71,0x35},{0x72,0x22},{0x73,0xF2},
        {0xA2,0x02},{0x17,0x16},{0x18,0x04},{0x32,0xA4},
        {0x19,0x02},{0x1A,0x7A},{0x03,0x0A},{0x15,0x00},
        {0xB0,0x84},{0x13,0xE7},{0x4F,0xB3},{0x50,0xB3},
        {0x51,0x00},{0x52,0x3D},{0x53,0xA7},{0x54,0xE4}
    };
    for (unsigned i = 0; i < sizeof(config)/sizeof(config[0]); ++i)
        Camera_Write(config[i][0], config[i][1]);
    HAL_Delay(500);
}

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *handle)
{
    if (handle == &hdcmi) frameReady = 1;
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *handle)
{
    if (handle == &hdcmi) frameError = 1;
}

/* In-place RGB565 (MSB first) -> 8-bit luminance. */
static void Convert_To_Grayscale(void)
{
    uint8_t *buf = (uint8_t *)frame;
    for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i) {
        uint16_t pixel = ((uint16_t)buf[i*2] << 8) | buf[i*2+1];
        uint32_t r = ((pixel >> 11) & 31U) * 255U / 31U;
        uint32_t g = ((pixel >> 5) & 63U) * 255U / 63U;
        uint32_t b = (pixel & 31U) * 255U / 31U;
        buf[i] = (uint8_t)((77U*r + 150U*g + 29U*b) >> 8);
    }
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    /* Avoid calling the cache-disable routine when D-cache is already off.
       Keep D-cache off for DMA buffer coherency in this example. */
    if (SCB->CCR & SCB_CCR_DC_Msk) {
        SCB_DisableDCache();
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_14;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = GPIO_PIN_13;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &gpio);

    huart3.Instance = USART3;
    huart3.Init.BaudRate = BAUD_RATE;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart3) != HAL_OK) Error_Handler();
    UART_Print("STM32 OK\r\n");

    /* TIM1 CH1: 72 MHz / 6 = 12 MHz camera XCLK, 50% duty. */
    htim1.Instance = TIM1;
    htim1.Init.Prescaler = 0;
    htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim1.Init.Period = 5;
    htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    if (HAL_TIM_Base_Init(&htim1) != HAL_OK) Error_Handler();
    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) Error_Handler();

    TIM_OC_InitTypeDef pwm = {0};
    pwm.OCMode = TIM_OCMODE_PWM1;
    pwm.Pulse = 3;
    pwm.OCPolarity = TIM_OCPOLARITY_HIGH;
    if (HAL_TIM_PWM_ConfigChannel(&htim1, &pwm, TIM_CHANNEL_1) != HAL_OK)
        Error_Handler();
    HAL_TIM_MspPostInit(&htim1);
    if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK) Error_Handler();
    UART_Print("XCLK OK\r\n");

    hi2c1.Instance = I2C1;
    hi2c1.Init.Timing = 0x00808CD2;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) Error_Handler();
    HAL_Delay(500);
    Camera_Init();
    UART_Print("CAMERA CONFIGURED\r\n");

    HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);

    hdcmi.Instance = DCMI;
    hdcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;
    hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
    hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_HIGH;
    hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
    hdcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;
    hdcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
    hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
    hdcmi.Init.ByteSelectMode = DCMI_BSM_ALL;
    hdcmi.Init.ByteSelectStart = DCMI_OEBS_ODD;
    hdcmi.Init.LineSelectMode = DCMI_LSM_ALL;
    hdcmi.Init.LineSelectStart = DCMI_OELS_ODD;
    if (HAL_DCMI_Init(&hdcmi) != HAL_OK) Error_Handler();
    HAL_NVIC_SetPriority(DCMI_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DCMI_IRQn);
    UART_Print("DCMI OK\r\n");

    Button_Init();
    UART_Print("CAMERA BUTTON READY\r\n");

    while (1) {
        uint8_t cmd;
        Button_Update();
        if (HAL_UART_Receive(&huart3, &cmd, 1, 10) != HAL_OK || cmd != 'c')
            continue;

        frameReady = frameError = 0;
        if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT,
                              (uint32_t)frame, sizeof(frame)/4U) != HAL_OK) {
            UART_Print("ERR!START\r\n");
            continue;
        }

        uint32_t start = HAL_GetTick();
        while ((!frameReady || __HAL_DMA_GET_COUNTER(&hdma_dcmi) != 0)
                && !frameError && HAL_GetTick() - start < 2000U) {
            Button_Update();
        }

        uint32_t remaining = __HAL_DMA_GET_COUNTER(&hdma_dcmi);
        uint8_t ok = frameReady && remaining == 0 && !frameError;
        HAL_DCMI_Stop(&hdcmi);

        if (!ok) {
            char msg[80];
            snprintf(msg, sizeof(msg),
                     "ERR!DMA remaining=%lu frame=%u error=%u\r\n",
                     (unsigned long)remaining, (unsigned)frameReady,
                     (unsigned)frameError);
            UART_Print(msg);
            continue;
        }

        Convert_To_Grayscale();
        Button_Update();
        UART_Print("GRY2");
        uint8_t angle = rotation;
        HAL_UART_Transmit(&huart3, &angle, 1, 1000);

        uint8_t *buf = (uint8_t *)frame;
        for (uint32_t i = 0; i < WIDTH * HEIGHT; i += 320U) {
            if (HAL_UART_Transmit(&huart3, buf + i, 320, 1000) != HAL_OK)
                break;
            Button_Update();
        }
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = 8;
    osc.PLL.PLLN = 72;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 3;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK)
        Error_Handler();
}

void Error_Handler(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_14;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif
