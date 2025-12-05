#include "main.h"

// маски для 7-сегментного индикатора (dp gfedcba)
const uint8_t segment_map[] = {
	0b11000000, // 0
	0b11111001, // 1
	0b10100100, // 2
	0b10110000, // 3
	0b10011001, // 4
	0b10010010, // 5
	0b10000010, // 6
	0b11111000, // 7
	0b10000000, // 8
	0b10010000  // 9
};

volatile bool is_measure_allowed = true;

void delay(uint32_t n) {
	while (n-- > 0);
}

// настройка 64 МГц
void init_clk() {
	RCC->CR |= RCC_CR_HSION;
	while(!(RCC->CR & RCC_CR_HSIRDY)){};

	FLASH->ACR |= FLASH_ACR_PRFTBE;

	FLASH->ACR &= ~FLASH_ACR_LATENCY;
	FLASH->ACR |= FLASH_ACR_LATENCY_2;

	RCC->CFGR |= RCC_CFGR_HPRE_DIV1;

	RCC->CFGR |= RCC_CFGR_PPRE2_DIV1;

	RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;

	// PLLCLK = HSI/2 * 16 = 64 MHz
	RCC->CFGR &= ~RCC_CFGR_PLLSRC;
	RCC->CFGR |= RCC_CFGR_PLLMULL16;

	RCC->CR |= RCC_CR_PLLON;

	while((RCC->CR & RCC_CR_PLLRDY) == 0) {};

	RCC->CFGR &= ~RCC_CFGR_SW;
	RCC->CFGR |= RCC_CFGR_SW_PLL;

	while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL){};
}

void init_periph() {
	// тактирование порта C
	RCC->APB2ENR |= RCC_APB2ENR_IOPCEN | RCC_APB2ENR_IOPAEN;

	GPIOC->CRL = 0x0;
	GPIOC->CRL = ( // output 2 МГц
			GPIO_CRL_MODE0_1 | GPIO_CRL_MODE1_1 |
			GPIO_CRL_MODE2_1 | GPIO_CRL_MODE3_1 |
			GPIO_CRL_MODE4_1 | GPIO_CRL_MODE5_1 |
			GPIO_CRL_MODE6_1 | GPIO_CRL_MODE7_1
		);

	// очистка PC8-PC11
	GPIOC->CRH &= ~(
			GPIO_CRH_MODE8 | GPIO_CRH_CNF8 |
			GPIO_CRH_MODE9 | GPIO_CRH_CNF9 |
			GPIO_CRH_MODE10 | GPIO_CRH_CNF10 |
			GPIO_CRH_MODE11 | GPIO_CRH_CNF11
		);

	GPIOC->CRH |= (
			GPIO_CRH_MODE8 | GPIO_CRH_MODE9 |
			GPIO_CRH_MODE10 | GPIO_CRH_MODE11
		);
}

void init_ADC1() {
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
	RCC->CFGR |= RCC_CFGR_ADCPRE_DIV8; // частота 64 / 8 на ацп

	// 239.5 циклов на измерение
	// канал 0 - PA0
	ADC1->SMPR2 |= ADC_SMPR2_SMP0;
	ADC1->CR2 |= ADC_CR2_CONT; // непрерывные измерения
	ADC1->CR2 |= ADC_CR2_ADON;

	delay(5);
	ADC1->CR2 |= ADC_CR2_CAL;
	while (ADC1->CR2 & ADC_CR2_CAL);

	ADC1->CR2 |= ADC_CR2_ADON;
}

void init_TIM2() {
	RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

	//Частота APB1 для таймеров = APB1Clk * 2 = 32МГц * 2 = 64МГц
	TIM2->PSC = 64000-1; // предделитель частоты (64МГц/64000 = 1к)
	TIM2->ARR = 300-1; // модуль счёта таймера (300 мс)
	TIM2->DIER |= TIM_DIER_UIE;	 // прерывание по переполнению вкл
	TIM2->CR1 |= TIM_CR1_CEN;

	NVIC_EnableIRQ(TIM2_IRQn);
}

void TIM2_IRQHandler(void) {
	TIM2->SR &= ~TIM_SR_UIF; // сброс флага переполнения
	is_measure_allowed = true; // разрешение на чтение числа
}

void display_digit(uint8_t digit, bool has_point) {
	if (digit < 10) {
		uint8_t mask = segment_map[digit];
		if (has_point) {
			mask &= ~(1 << 7);
		}
		GPIOC->ODR = ((GPIOC->ODR >> 8) << 8) | mask;
	}
}

void display_number(float number) {
	if (number < 0) {
		return;
	}

	uint16_t num = (uint16_t) (number * 100);
	// пин разрядов c PC8
	uint8_t bit_offset = 8;
	for (int i = 0; i < 4; i++) {
		bool has_point = false;
		// разряды справа налево (3210)
		if (i == 2) {
			has_point = true;
		}
		display_digit(num % 10, has_point);
		GPIOC->ODR |= (1 << (i + bit_offset));
		delay(3000);
		GPIOC->ODR &= ~(1 << (i + bit_offset));
		num /= 10;
	}
}

// PC0-PC7 индикатор (abcdefg df)
// PC8-PC11 выбор индикатора 1-4
// PA0 ацп
int main() {
	init_clk();
	init_periph();
	init_ADC1();
	init_TIM2();

	float voltage = 0;
	// коэффициент для преобразования значений ацп в вольты
	const float coeff = 3.3 / 4095.0 * (121.71 + 9.79) / 9.79;

	while (1) {
		// через флаг, чтобы цифры на индикаторе не скакали часто
		if (is_measure_allowed) {
			is_measure_allowed = false;
			uint16_t raw_data = ADC1->DR;
			voltage = raw_data * coeff;
		}
		display_number(voltage);
	}
}
