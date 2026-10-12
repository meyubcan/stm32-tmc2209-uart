/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TMC_SYNC            0x05U
#define TMC_SLAVE_ADDR      0x00U    /* MS1 = MS2 = GND */
#define TMC_MASTER_ADDR     0xFFU    /* surucu cevabi bu adrese yollar */
#define TMC_WRITE_FLAG      0x80U

#define TMC_REG_GCONF       0x00U
#define TMC_REG_IFCNT       0x02U
#define TMC_REG_IOIN        0x06U
#define TMC_REG_IHOLD_IRUN  0x10U
#define TMC_REG_TPOWERDOWN  0x11U
#define TMC_REG_CHOPCONF    0x6CU
#define TMC_REG_DRV_STATUS  0x6FU

#define TMC_READ_ERROR      0xFFFFFFFFUL
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim4;

UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM4_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */
static void     log_print(const char *s);
static uint8_t  TMC_CRC(const uint8_t *data, uint8_t len);
static void     TMC_WriteReg(uint8_t reg, uint32_t value);
static uint32_t TMC_ReadReg(uint8_t reg);
static void     TMC_ReportStatus(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Terminal ciktisi USART3 uzerinden (115200 8N1) */
static void log_print(const char *s)
{
  HAL_UART_Transmit(&huart3, (uint8_t *)s, (uint16_t)strlen(s), 100);
}

/* TMC2209 datagram CRC'si: polinom x^8 + x^2 + x + 1, baytlar LSB'den islenir */
static uint8_t TMC_CRC(const uint8_t *data, uint8_t len)
{
  uint8_t crc = 0;

  for (uint8_t i = 0; i < len; i++) {
    uint8_t byte = data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if ((crc >> 7) ^ (byte & 0x01U))
        crc = (uint8_t)((crc << 1) ^ 0x07U);
      else
        crc = (uint8_t)(crc << 1);
      byte >>= 1;
    }
  }
  return crc;
}

/* Yazma datagrami 8 bayt: sync, adres, register | 0x80, 4 bayt veri (MSB once), CRC */
static void TMC_WriteReg(uint8_t reg, uint32_t value)
{
  uint8_t data[8];

  data[0] = TMC_SYNC;
  data[1] = TMC_SLAVE_ADDR;
  data[2] = reg | TMC_WRITE_FLAG;
  data[3] = (uint8_t)(value >> 24);
  data[4] = (uint8_t)(value >> 16);
  data[5] = (uint8_t)(value >> 8);
  data[6] = (uint8_t)(value);
  data[7] = TMC_CRC(data, 7);

  HAL_HalfDuplex_EnableTransmitter(&huart2);
  HAL_UART_Transmit(&huart2, data, 8, 100);
}

/* Okuma istegi 4 bayt: sync, adres, register, CRC. Surucu ayni hattan 8 baytla
 * cevap verir: sync, 0xFF, register, 4 bayt veri, CRC. Hat tek tel oldugu icin
 * istek gittikten sonra USART aliciya cevrilir. */
static uint32_t TMC_ReadReg(uint8_t reg)
{
  uint8_t req[4];
  uint8_t resp[8] = {0};
  char    line[80];

  req[0] = TMC_SYNC;
  req[1] = TMC_SLAVE_ADDR;
  req[2] = reg & 0x7FU;
  req[3] = TMC_CRC(req, 3);

  HAL_HalfDuplex_EnableTransmitter(&huart2);
  HAL_UART_Transmit(&huart2, req, 4, 100);

  HAL_HalfDuplex_EnableReceiver(&huart2);
  if (HAL_UART_Receive(&huart2, resp, 8, 200) != HAL_OK) {
    log_print("UART zaman asimi, surucu cevap vermedi\r\n");
    return TMC_READ_ERROR;
  }

  snprintf(line, sizeof(line), "Ham veri: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
           resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6], resp[7]);
  log_print(line);

  if (resp[0] != TMC_SYNC || resp[1] != TMC_MASTER_ADDR || resp[7] != TMC_CRC(resp, 7)) {
    log_print("Cevap bozuk (sync, adres ya da CRC tutmuyor)\r\n");
    return TMC_READ_ERROR;
  }

  return ((uint32_t)resp[3] << 24) | ((uint32_t)resp[4] << 16) |
         ((uint32_t)resp[5] << 8)  |  (uint32_t)resp[6];
}

/* DRV_STATUS registerini okur ve bitlerini terminale acik acik yazar */
static void TMC_ReportStatus(void)
{
  static const struct { uint8_t bit; const char *text; } flags[] = {
    { 0,  "[UYARI] OTPW  : isinma on uyarisi\r\n" },
    { 1,  "[HATA]  OT    : asiri isinma, surucu kapandi\r\n" },
    { 2,  "[HATA]  S2GA  : A fazi topraga kisa devre\r\n" },
    { 3,  "[HATA]  S2GB  : B fazi topraga kisa devre\r\n" },
    { 4,  "[HATA]  S2VSA : A fazi beslemeye kisa devre\r\n" },
    { 5,  "[HATA]  S2VSB : B fazi beslemeye kisa devre\r\n" },
    { 6,  "[UYARI] OLA   : A fazi acik devre\r\n" },
    { 7,  "[UYARI] OLB   : B fazi acik devre\r\n" },
    { 8,  "[BILGI] T120  : sicaklik 120 C ustunde\r\n" },
    { 9,  "[BILGI] T143  : sicaklik 143 C ustunde\r\n" },
    { 10, "[BILGI] T150  : sicaklik 150 C ustunde\r\n" },
    { 11, "[BILGI] T157  : sicaklik 157 C ustunde\r\n" },
    { 30, "[BILGI] STEALTH: StealthChop modunda\r\n" },
    { 31, "[BILGI] STST  : motor duruyor\r\n" },
  };
  const uint32_t fault_mask = 0x000000FEUL;   /* OT, kisa devre ve acik devre bitleri */
  char line[64];

  uint32_t drv = TMC_ReadReg(TMC_REG_DRV_STATUS);
  if (drv == TMC_READ_ERROR) {
    return;
  }

  snprintf(line, sizeof(line), "\r\n=== DRV_STATUS: 0x%08lX ===\r\n", (unsigned long)drv);
  log_print(line);

  for (uint32_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
    if (drv & (1UL << flags[i].bit)) {
      log_print(flags[i].text);
    }
  }

  /* CS_ACTUAL: surucunun o an uyguladigi akim kademesi, 0..31 */
  snprintf(line, sizeof(line), "CS_ACTUAL     : %lu / 31\r\n", (unsigned long)((drv >> 16) & 0x1FUL));
  log_print(line);

  if ((drv & fault_mask) == 0U) {
    log_print("[OK] Hata yok\r\n");
  }
  log_print("========================\r\n");
}
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

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM4_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  char buf[64];

  HAL_Delay(500);   /* surucu beslemesi otursun */

  /* Baglanti testi: IOIN okunabiliyorsa UART hatti calisiyor demektir */
  uint32_t ioin = TMC_ReadReg(TMC_REG_IOIN);
  snprintf(buf, sizeof(buf), "IOIN = 0x%08lX\r\n", (unsigned long)ioin);
  log_print(buf);

  uint32_t ifcnt_before = TMC_ReadReg(TMC_REG_IFCNT);

  /* GCONF: pdn_disable = 1 (PDN pini UART icin kullaniliyor),
   *        mstep_reg_select = 1 (mikro adim MS pinlerinden degil registerdan),
   *        en_SpreadCycle = 0 (StealthChop, sessiz mod) */
  TMC_WriteReg(TMC_REG_GCONF, (1UL << 6) | (1UL << 7));
  log_print("GCONF yazildi\r\n");

  /* CHOPCONF: MRES = 4 (16 mikro adim), intpol = 1, TOFF = 3, HSTRT = 5 */
  TMC_WriteReg(TMC_REG_CHOPCONF, (0x04UL << 24) | 0x10000053UL);
  log_print("CHOPCONF yazildi\r\n");

  /* IHOLD_IRUN: calisma akimi 18/31, bekleme akimi 5/31, gecis gecikmesi 6.
   * Gercek amper degeri karttaki sense direncine baglidir. */
  TMC_WriteReg(TMC_REG_IHOLD_IRUN, (6UL << 16) | (18UL << 8) | 5UL);
  log_print("IHOLD_IRUN yazildi: IRUN=18 IHOLD=5\r\n");

  /* TPOWERDOWN: motor durduktan yaklasik 0.4 s sonra bekleme akimina gec */
  TMC_WriteReg(TMC_REG_TPOWERDOWN, 20);

  /* IFCNT her gecerli yazmada bir artar. Dort yazma yapildi, fark 4 olmali. */
  HAL_Delay(10);
  uint32_t ifcnt_after = TMC_ReadReg(TMC_REG_IFCNT);
  if (ifcnt_before != TMC_READ_ERROR && ifcnt_after != TMC_READ_ERROR) {
    snprintf(buf, sizeof(buf), "IFCNT farki = %lu (4 olmali)\r\n",
             (unsigned long)((ifcnt_after - ifcnt_before) & 0xFFUL));
    log_print(buf);
  }

  log_print("TMC2209 hazir\r\n");

  /* PD13 ve PD14 surucunun EN ve DIR girislerine gidiyor, ikisi de 0'da tutulur:
   * surucu acik, yon sabit. */
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13 | GPIO_PIN_14, GPIO_PIN_RESET);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* STEP darbeleri TIM4 CH1'den (PD12), 1 kHz. 16 mikro adimda 1.8 derecelik
     * motor icin yaklasik 19 devir/dakika eder. */
    HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);
    HAL_Delay(1500);

    /* Durum motor donerken okunur, acik devre bitleri sadece o zaman anlamli */
    TMC_ReportStatus();

    HAL_Delay(1500);
    HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_1);
    HAL_Delay(2000);
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 84-1;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 1000-1 ;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 500;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */
  HAL_TIM_MspPostInit(&htim4);

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_HalfDuplex_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13|GPIO_PIN_14, GPIO_PIN_RESET);

  /*Configure GPIO pins : PD13 PD14 */
  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_14;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
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
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
