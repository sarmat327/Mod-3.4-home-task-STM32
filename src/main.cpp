#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdio.h>

#define LED_PORT GPIOA
#define LED_PIN GPIO_PIN_1 /* TIM2_CH2, AF1 */

#define PWM_FREQ_HZ 1000U
#define TIM_TICK_HZ 1000000U  /* 16 MHz / PSC 16 */
#define TIM_FREQ_HZ 16000000U /* 16 MHz */
#define PWM_PSC 16U
#define PWM_ARR (TIM_TICK_HZ / PWM_FREQ_HZ - 1U)
#define NOTE_GAP_MS 50U

static size_t   note_idx;
static uint32_t phase_start;
static bool     in_gap;

typedef struct
{
    uint16_t freq_hz;     /* 0 = пауза */
    uint16_t duration_ms;
} Note;

static const Note melody[] = {
    /* Jingle bells, jingle bells, jingle all the way */
    {330, 400}, {330, 400}, {330, 800},
    {330, 400}, {330, 400}, {330, 800},
    {330, 400}, {392, 400}, {262, 600}, {294, 200}, {330, 1200},

    /* Oh what fun it is to ride in a one-horse open sleigh */
    {349, 400}, {349, 400}, {349, 600}, {349, 200},
    {349, 400}, {330, 400}, {330, 400}, {330, 200}, {330, 200},
    {330, 400}, {294, 400}, {294, 400}, {330, 400}, {294, 800}, {392, 800},

    /* Jingle bells (повтор початку) */
    {330, 400}, {330, 400}, {330, 800},
    {330, 400}, {330, 400}, {330, 800},
    {330, 400}, {392, 400}, {262, 600}, {294, 200}, {330, 1200},

    /* Oh what fun it is to ride in a one-horse open sleigh */
    {349, 400}, {349, 400}, {349, 600}, {349, 200},
    {349, 400}, {330, 400}, {330, 400}, {330, 200}, {330, 200},
    {392, 400}, {392, 400}, {349, 400}, {294, 400}, {262, 1200},

    {0, 2000},   /* пауза перед повтором */
};

#define MELODY_LEN (sizeof(melody) / sizeof(melody[0])) // автоматично розраховуємо довжину мелодії



/* ---------- globals (shared with ISR context) ---------- */
TIM_HandleTypeDef htim2;
UART_HandleTypeDef huart2;

/* ---------- helpers ---------- */

static void GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};

    g.Pin = LED_PIN;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(LED_PORT, &g);

    g.Pin = GPIO_PIN_2; /* USART2 TX */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &g);
}

static void UART2_Init(void)
{
    __HAL_RCC_USART2_CLK_ENABLE();

    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);
}

static void TIM2_Init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance = TIM2;
    htim2.Init.Prescaler = PWM_PSC - 1; /* 16 MHz / 16 = 1 MHz */
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = PWM_ARR; /* 1 MHz / 1000 = 1 kHz */
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    HAL_TIM_PWM_Init(&htim2);

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = DISABLE;
    HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_2);

    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
}

static void set_tone_freq(int freq_hz)
{
    if (0 == freq_hz)
    {
        HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_2);
        return;
    }

    uint32_t f_clk = TIM_FREQ_HZ;
    uint32_t psc = PWM_PSC - 1;
    uint32_t arr = (f_clk / (psc + 1)) / freq_hz - 1;

    __HAL_TIM_SET_AUTORELOAD(&htim2, arr);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, arr / 2);
    __HAL_TIM_SET_COUNTER(&htim2, 0);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
}
static void melody_update(void)

{
    uint32_t now = HAL_GetTick();

    if (!in_gap)
    {
        if (now - phase_start >= melody[note_idx].duration_ms)
        {
            set_tone_freq(0);          /* кінець ноти, короткий проміжок */
            in_gap = true;
            phase_start = now;
        }
    }
    else if (now - phase_start >= NOTE_GAP_MS)
    {
        note_idx = (note_idx + 1) % MELODY_LEN;  /* зациклення */
        in_gap = false;
        phase_start = now;
        set_tone_freq(melody[note_idx].freq_hz);
    }
}

static void melody_start(void) //початково інітимо потрібні нам змінні
{
    note_idx = 0;
    in_gap = false;
    phase_start = HAL_GetTick();
    set_tone_freq(melody[0].freq_hz);
}

void Error_Handler(void);

int main(void)
{
    HAL_Init();
    GPIO_Init();
    UART2_Init();
    TIM2_Init();
    melody_start();
    setvbuf(stdout, NULL, _IONBF, 0); /* no buffering: text appears immediately */

    while (1)
    {

         melody_update();

    }
}

#ifdef __cplusplus
extern "C"
{
#endif
    /* ---------------- Interrupt handlers ---------------- */

    void SysTick_Handler(void)
    {
        HAL_IncTick();
    }

    int _write(int fd, char *ptr, int len)
    {
        (void)fd;
        HAL_UART_Transmit(&huart2, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
        return len;
    }

#ifdef __cplusplus
}
#endif

void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
    }
}