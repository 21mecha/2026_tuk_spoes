#include "main.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>  // ✅ atof 함수 사용을 위해 꼭 포함


uint8_t rx_index = 0;
float target_flow = 0.0f;       // 목표 유량 (%)

// PID parameters
double Kp = 30, Ki = 0, Kd = 2.0;
double Setpoint = 10.0;  // Example target temperature
double Input = 0, Output = 0;

// Thermistor constants
#define RT0 10000  // Resistance at T0
#define B 3988     // Beta coefficient
#define VCC 3.3    // Supply voltage
#define R 10000    // Fixed resistor value
#define T0 (25.0 + 273.15) // Reference temperature in Kelvin

// Constants
#define VREF 3.3
#define ADC_RESOLUTION 4095.0
#define MAX_PRESSURE_TORR 10.0

extern ADC_HandleTypeDef hadc1; // 써미스터(온도센서)
extern ADC_HandleTypeDef hadc2; // MFC (N2 : CH12, Ar: CH11)
extern ADC_HandleTypeDef hadc3; // 바라트론
extern DAC_HandleTypeDef hdac; //DAC1_CH1(PA4), DAC1_CH2(PA5)
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern UART_HandleTypeDef huart3;


// Moving average filter
#define NUM_READINGS 5
double readings[NUM_READINGS];
int readIndex = 0;
double total = 0, averageInput = 0;

// Function prototypes
double calculateTemperature(uint32_t adcValue);
double PID_Compute(double input);
void updatePWMOutput(double output);
void Debug_ADC_Read(void);

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_ADC3_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_DAC_Init(void);
double calculatePressure(uint32_t pressValue);




int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_ADC1_Init();
    MX_ADC2_Init();
    MX_ADC3_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_USART3_UART_Init();
    MX_DAC_Init();

    HAL_ADC_Start(&hadc1);
    HAL_ADC_Start(&hadc2);
    HAL_ADC_Start(&hadc3);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
    HAL_DAC_Start(&hdac, DAC_CHANNEL_2);

    HAL_Delay(500);  // UART 안정화
    HAL_DAC_SetValue(&hdac, DAC_CHANNEL_2, DAC_ALIGN_12B_R, 4095);  // 초기값

    char startMsg[] = "STM32 - 시스템 시작...\r\n";
    HAL_UART_Transmit(&huart3, (uint8_t*)startMsg, strlen(startMsg), 100);

    for (int i = 0; i < NUM_READINGS; i++) readings[i] = 0;

    char rx_buffer[32] = {0};

    while (1) {
        // ✅ 시리얼 수신 (GUI → STM32)
        if (HAL_UART_Receive(&huart3, (uint8_t*)rx_buffer, sizeof(rx_buffer) - 1, 10) == HAL_OK) {
            rx_buffer[32] = '\0';  // 안전한 종료 문자

            // 디버깅 출력: 받은 명령어 그대로 출력
            HAL_UART_Transmit(&huart3, (uint8_t*)rx_buffer, strlen(rx_buffer), 100);

            if (strncmp(rx_buffer, "FLOW_SET:", 9) == 0) {
                           float flow_percent = atof(&rx_buffer[9]);
                           if (strncmp(rx_buffer, "FLOW_SET:", 9) == 0)
                           {
                               float flow_percent = atof(&rx_buffer[9]);  // 예: 50.0
                               uint32_t dac_val = (uint32_t)((flow_percent / 100.0f) * 4095.0f);
                               HAL_DAC_SetValue(&hdac, DAC_CHANNEL_2, DAC_ALIGN_12B_R, dac_val);
                               HAL_DAC_Start(&hdac, DAC_CHANNEL_2);
                               char msg[64];
                               sprintf(msg, "DAC 설정: %.2f%% → %lu\r\n", flow_percent, dac_val);
                               HAL_UART_Transmit(&huart3, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);
                           }

                       }

            memset(rx_buffer, 0, sizeof(rx_buffer));  // 버퍼 초기화
        }

        // ✅ 온도 측정 (ADC1)
        HAL_ADC_Start(&hadc1);
        HAL_ADC_PollForConversion(&hadc1, HAL_MAX_DELAY);
        uint32_t adcValue = HAL_ADC_GetValue(&hadc1);

        total -= readings[readIndex];
        readings[readIndex] = adcValue;
        total += readings[readIndex];
        readIndex = (readIndex + 1) % NUM_READINGS;
        averageInput = total / NUM_READINGS;
        Input = calculateTemperature(averageInput);

        // ✅ 압력 측정 (ADC3)
        HAL_ADC_Start(&hadc3);
        HAL_ADC_PollForConversion(&hadc3, HAL_MAX_DELAY);
        uint32_t pressValue = HAL_ADC_GetValue(&hadc3);
        double pressure = calculatePressure(pressValue);

        // ✅ PID → PWM 출력
        Output = PID_Compute(Input);
        updatePWMOutput(Output);

        // ✅ 유량 측정 (ADC2)
        HAL_ADC_Start(&hadc2);
        HAL_ADC_PollForConversion(&hadc2, HAL_MAX_DELAY);
        uint32_t mfcValue = HAL_ADC_GetValue(&hadc2);
        float mfcVoltage = (mfcValue / 4095.0f) * 3.3f;
        float flowRate = (mfcVoltage / 3.3f) * 100.0f;

        // ✅ UART로 모든 센서값 전송
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Temperature: %.2f, Pressure: %.3f, Flow Rate: %.2f\r\n",
                 Input-10, pressure, flowRate);
        HAL_UART_Transmit(&huart3, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);

        HAL_Delay(400);
    }
}

double calculatePressure(uint32_t pressValue) {
    double pressvoltage = (pressValue * VCC) / 4095.0;
    return pressvoltage;  // 선형 변환
}

void SendToPuTTY(const char* msg) {
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "%s\r", msg);
    HAL_UART_Transmit(&huart3, (uint8_t*)buffer, strlen(buffer), HAL_MAX_DELAY);
}

double calculateTemperature(uint32_t adcValue) {
    double VRT = ((adcValue) * VCC) / 4095.0;  // ADC to voltage
    double RT = (VRT * R) / (VCC - VRT);    // Voltage divider formula
    double lnRT = log(RT / RT0);
    double tempK = 1 / ((lnRT / B) + (1 / T0)); // Thermistor formula
    return tempK - 273.15;  // Convert to Celsius
}

double PID_Compute(double input) {
    static double lastError = 0, integral = 0;
    double error = Setpoint - input;
    integral += error;
    double derivative = error - lastError;
    lastError = error;

    double output = (Kp * error) + (Ki * integral) + (Kd * derivative);
    if (output > 100) output = 100;
    if (output < 0) output = 0;
    return output;
}

void updatePWMOutput(double output) {
    uint32_t pwmValue = (uint32_t)((output / 100.0) * __HAL_TIM_GET_AUTORELOAD(&htim1));

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pwmValue);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pwmValue);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, pwmValue);
}
static void MX_USART3_UART_Init(void) {
    __HAL_RCC_USART3_CLK_ENABLE();  // USART3 클럭 활성화

    huart3.Instance = USART3;
    huart3.Init.BaudRate = 115200;   // Baud rate 설정
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart3) != HAL_OK) {
        Error_Handler();
    }
}

static void MX_ADC1_Init(void) {
    ADC_ChannelConfTypeDef sConfig = {0};

    __HAL_RCC_ADC1_CLK_ENABLE();

    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode = DISABLE;
    hadc1.Init.ContinuousConvMode = ENABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        Error_Handler();
    }

    sConfig.Channel = ADC_CHANNEL_9;
    sConfig.Rank = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
        Error_Handler();
    }
}//PB1 온도 adc

// ✅ ADC2: PC2 (CHANNEL_12) MFC
static void MX_ADC2_Init(void)
{
     ADC_ChannelConfTypeDef sConfig = {0};
    __HAL_RCC_ADC2_CLK_ENABLE();

    hadc2.Instance = ADC2;
    hadc2.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
    hadc2.Init.Resolution = ADC_RESOLUTION_12B;
    hadc2.Init.ScanConvMode = DISABLE;
    hadc2.Init.ContinuousConvMode = ENABLE;
    hadc2.Init.DiscontinuousConvMode = DISABLE;
    hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc2.Init.NbrOfConversion = 1;

    if (HAL_ADC_Init(&hadc2) != HAL_OK)
        Error_Handler();

    sConfig.Channel = ADC_CHANNEL_12;  // PC2
    sConfig.Rank = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;

    if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
        Error_Handler();
}

// baratron ADC
static void MX_ADC3_Init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    __HAL_RCC_ADC3_CLK_ENABLE();  // ✅ ADC3 클럭 활성화

    hadc3.Instance = ADC3;
    hadc3.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
    hadc3.Init.Resolution = ADC_RESOLUTION_12B;
    hadc3.Init.ScanConvMode = DISABLE;
    hadc3.Init.ContinuousConvMode = ENABLE;
    hadc3.Init.DiscontinuousConvMode = DISABLE;
    hadc3.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc3.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc3.Init.NbrOfConversion = 1;

    if (HAL_ADC_Init(&hadc3) != HAL_OK)
    {
        Error_Handler();
    }

    // PC0 = ADC3_IN10
    sConfig.Channel = ADC_CHANNEL_10;  // PC0
    sConfig.Rank = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;

    if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MX_DAC_Init(void) //PA4
{
  DAC_ChannelConfTypeDef sConfig = {0};

  __HAL_RCC_DAC_CLK_ENABLE();  // DAC 클럭 활성화

  hdac.Instance = DAC;
  if (HAL_DAC_Init(&hdac) != HAL_OK)
  {
    Error_Handler();
  }

  // ✅ DAC 채널 설정 추가 (중요!)
  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
}


static void MX_TIM1_Init(void) {
    TIM_OC_InitTypeDef sConfigOC = {0};

    __HAL_RCC_TIM1_CLK_ENABLE();

    htim1.Instance = TIM1;
    htim1.Init.Prescaler = 84 - 1;
    htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim1.Init.Period = 1000 - 1;
    htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
        Error_Handler();
    }

    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = 500; // 초기 듀티 사이클 50%
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) {
        Error_Handler();
    }

    HAL_TIM_MspPostInit(&htim1);
}
static void MX_TIM2_Init(void) {
    __HAL_RCC_TIM2_CLK_ENABLE();
    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};
    TIM_OC_InitTypeDef sConfigOC = {0};

    htim2.Instance = TIM2;
    htim2.Init.Prescaler = 84 - 1;
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = 1000 - 1;
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&htim2) != HAL_OK) {
        Error_Handler();
    }

    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = 500; // 초기 듀티 사이클 50%
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) {
        Error_Handler();
    }
}
static void MX_TIM3_Init(void) {
    __HAL_RCC_TIM3_CLK_ENABLE();  // TIM3 클럭 활성화

    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};
    TIM_OC_InitTypeDef sConfigOC = {0};

    // TIM3 기본 설정
    htim3.Instance = TIM3;
    htim3.Init.Prescaler = 84 - 1;           // 프리스케일러 (84MHz 기준으로 1MHz로 설정)
    htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim3.Init.Period = 1000 - 1;            // PWM 주기 (1kHz)
    htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    // PWM 초기화
    if (HAL_TIM_PWM_Init(&htim3) != HAL_OK) {
        Error_Handler();
    }

    // PWM 출력 설정
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = 500;                   // 초기 듀티 사이클 (50%)
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

    // TIM3 채널 1 설정
    if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) {
        Error_Handler();
    }

    // TIM3 후처리 (GPIO 초기화 등)
    HAL_TIM_MspPostInit(&htim3);
}
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    // 클럭 활성화
    __HAL_RCC_GPIOD_CLK_ENABLE();  // USART3 (PD8, PD9)
    __HAL_RCC_GPIOC_CLK_ENABLE();  // ADC 입력 (PC2)
    __HAL_RCC_GPIOA_CLK_ENABLE();  // 필요시 확장 (예: DAC, 버튼 등)

    // USART3 TX (PD8) + RX (PD9) 설정
    GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    // ADC 입력 (PC2) 설정
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

   // ADC 입력 (PC0) 설정
    GPIO_InitStruct.Pin = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_4;
      GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
      GPIO_InitStruct.Pull = GPIO_NOPULL;
      HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}


void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  // PWR 클럭 활성화 및 전압 스케일 설정 (F4 시리즈에서 필요함)
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  // HSE 설정: 외부 크리스털(HSE_ON) 또는 외부 클럭(HSE_BYPASS)
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS; // or RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;

  // PLL 설정 (적절히 조정)
  RCC_OscInitStruct.PLL.PLLM = 4;   // or 8;
  RCC_OscInitStruct.PLL.PLLN = 168; // or 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  // 클럭 설정
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;

  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

}

void Error_Handler(void) {
    // 간단히 무한 루프를 통해 디버깅 가능
    while (1) {

    }
}


/*통합 동작

ADC1 (PB1) → 온도 센서 (써미스터)

ADC2 (PC2) → MFC 유량 센서

ADC3 (PC0) → 바라트론 압력 측정

DAC1 (PA5) → MFC 세트포인트 출력

TIM1~3 채널 1 → PID 결과에 따른 PWM 제어

UART3 (PD8/PD9) → PuTTY 디버깅 출력

Moving Average 필터 → 온도 센서 필터링

PID_Compute() → PID 제어 계산

updatePWMOutput() → PWM 듀티 반영

calculatePressure() → 압력 값 계산

Debug_ADC_Read() → MFC 아날로그 정보 UART 전송*/
