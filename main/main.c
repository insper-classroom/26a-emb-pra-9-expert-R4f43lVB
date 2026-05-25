#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "ssd1306.h"
#include "hc06.h"

// Tela OLED
ssd1306_t disp;

// PINS
#define LED_R 13
#define LED_G 14
#define LED_B 15
#define BTN_PIN 16
#define PIN_X 27
#define PIN_Y 26

// Semaforos
SemaphoreHandle_t xSemaphorePIN;
SemaphoreHandle_t xSemaphoreConnection;

typedef struct {
    int axis;
    int value;
} adc_t;

// Filas
QueueHandle_t xQueueRX;
QueueHandle_t xQueueTX;
QueueHandle_t xQueueADC;

// Callbacks
void gpio_callback(uint gpio, uint32_t events) {
    if (gpio == BTN_PIN) {
        xSemaphoreGiveFromISR(xSemaphorePIN, 0);
    }
}

// Tasks
void uart_rx_handler() {
    uart_getc(HC06_UART_ID);
        xSemaphoreGiveFromISR(xSemaphoreConnection, 0);
}

void init_uart_irq() {
    // Turn off FIFO's - we want to do this character by character
    uart_set_fifo_enabled(HC06_UART_ID, false);

    // Set up a RX interrupt
    // We need to set up the handler first
    // Select correct interrupt for the UART we are using
    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    // And set up and enable the interrupt handlers
    irq_set_exclusive_handler(UART_IRQ, uart_rx_handler);
    irq_set_enabled(UART_IRQ, true);

    // Now enable the UART to send interrupts - RX only
    uart_set_irq_enables(HC06_UART_ID, true, false);
}

static void tx_task(void *p) {
    adc_t data;

    while (true) {
        if (xQueueReceive(xQueueTX, &data, portMAX_DELAY) == pdTRUE) {
            uart_putc_raw(HC06_UART_ID, data.axis);
            uart_putc_raw(HC06_UART_ID, data.value);
            uart_putc_raw(HC06_UART_ID, data.value >> 8);
            uart_putc_raw(HC06_UART_ID, -1);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void serial_task(void *p) {
    uint8_t ch;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            ch = (uint8_t)c;
            xQueueSend(xQueueTX, &ch, 0);
        }

        while (xQueueReceive(xQueueRX, &ch, 0) == pdTRUE) {
            putchar_raw(ch);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void x_task(void *p) {
    adc_t data;
    data.axis = 0;

    int soma = 0;
    int numeros[6] = {0, 0, 0, 0, 0, 0};
    while (true) {
        int result;
        adc_select_input(1);
        result = ((int)adc_read() - 2047) / 8;

        int velho = numeros[0];
        for (int i = 0; i < 5; i++)
            numeros[i] = numeros[i + 1];
        numeros[5] = result;
        soma += result - velho;
        int media = soma / 6;
        if (media > 50 || media < -50) {
            data.value = media;
            xQueueSend(xQueueTX, &data, pdMS_TO_TICKS(10));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void y_task(void *p) {
    adc_t data;
    data.axis = 1;

    int soma = 0;
    int numeros[6] = {0, 0, 0, 0, 0, 0};
    while (true) {
        int result;
        adc_select_input(0);
        result = ((int)adc_read() - 2047) / 8;

        int velho = numeros[0];
        for (int i = 0; i < 5; i++)
            numeros[i] = numeros[i + 1];
        numeros[5] = result;
        soma += result - velho;
        int media = soma / 6;
        if (media > 50 || media < -50) {
            data.value = media;
            xQueueSend(xQueueTX, &data, pdMS_TO_TICKS(10));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void uart_task(void *p) {
    adc_t data;

    while (true) {
        if (xQueueReceive(xQueueADC, &data, pdMS_TO_TICKS(50))) {
            uart_putc(HC06_UART_ID, data.axis);
            uart_putc(HC06_UART_ID, data.value);
            uart_putc(HC06_UART_ID, (data.value >> 8));
            uart_putc(HC06_UART_ID, -1);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void bluetooth_task(void *p) {
    char pin[5];

    while (true) {
        if (xSemaphoreTake(xSemaphorePIN, pdMS_TO_TICKS(10))) {
            srand(time_us_32());
            ssd1306_clear(&disp);
            ssd1306_draw_string(&disp, 8, 12, 1, "Configurando HC06");
            ssd1306_show(&disp);

            for (int i = 0; i < 4; i++) {
                pin[i] = '0' + rand() % 10;
            }
            pin[4] = '\x0';
            
            hc06_config("Jefferson", pin);
            
            ssd1306_clear(&disp);
            ssd1306_draw_string(&disp, 8, 12, 2, "PIN: ");
            ssd1306_draw_string(&disp, 64, 12, 2, pin);
            ssd1306_show(&disp);
            
            init_uart_irq();
            xTaskCreate(tx_task, "TX", 1024, NULL, 2, NULL);
            xTaskCreate(x_task, "Task X", 1024, NULL, 1, NULL);
            xTaskCreate(y_task, "Task Y", 1024, NULL, 1, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void pwm_task(void *p) {
    uint slice_r = pwm_gpio_to_slice_num(LED_R);
    const uint chan_r = pwm_gpio_to_channel(LED_R);

    uint slice_g = pwm_gpio_to_slice_num(LED_G);
    // const uint chan_g = pwm_gpio_to_channel(LED_G);
    
    uint slice_b = pwm_gpio_to_slice_num(LED_B);
    const uint chan_b = pwm_gpio_to_channel(LED_B);
    
    static int flag_state = 0;

    while (true) {
        if (xSemaphoreTake(xSemaphoreConnection, pdMS_TO_TICKS(20))) {
            flag_state = !flag_state;
            // printf("valor do state: %d\n", flag_state);
        }

        if (!flag_state) {
            pwm_set_gpio_level(LED_G, 0);
            for (int i = 0; i <= 255; i++) {
                pwm_set_chan_level(slice_r, chan_r, i);
                pwm_set_chan_level(slice_b, chan_b, i);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            for (int i = 255; i >= 0; i--) {
                pwm_set_chan_level(slice_r, chan_r, i);
                pwm_set_chan_level(slice_b, chan_b, i);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } else {
            pwm_set_gpio_level(LED_R, 0);
            pwm_set_gpio_level(LED_G, 255);
            pwm_set_gpio_level(LED_B, 0);
            
            pwm_set_wrap(slice_r, 255);
            pwm_set_wrap(slice_g, 255);
            pwm_set_wrap(slice_b, 255);
            
            pwm_set_enabled(slice_r, true);
            pwm_set_enabled(slice_g, true);
            pwm_set_enabled(slice_b, true);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Funcoes auxiliares
void gpio_config(void) {
    gpio_init(BTN_PIN);
    gpio_set_dir(BTN_PIN, GPIO_IN);
    gpio_pull_up(BTN_PIN);
    gpio_set_irq_enabled_with_callback(BTN_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
}

void init_uart_hc06(void) {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);

    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));

    int __unused actual = uart_set_baudrate(HC06_UART_ID, HC06_BAUD_RATE);

    uart_set_hw_flow(HC06_UART_ID, false, false);

    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

void oled_init(void) {
    i2c_init(i2c1, 400000);

    gpio_set_function(2, GPIO_FUNC_I2C);
    gpio_set_function(3, GPIO_FUNC_I2C);

    gpio_pull_up(2);
    gpio_pull_up(3);

    disp.external_vcc = false;

    ssd1306_init(&disp, 128, 32, 0x3C, i2c1);

    ssd1306_clear(&disp);
    ssd1306_show(&disp);
}

void init_pwm(void) {
    gpio_set_function(LED_R, GPIO_FUNC_PWM);
    gpio_set_function(LED_G, GPIO_FUNC_PWM);
    gpio_set_function(LED_B, GPIO_FUNC_PWM);
}

// Funcao principal
int main(void) {
    stdio_init_all();
    adc_init();

    adc_gpio_init(PIN_Y);
    adc_gpio_init(PIN_X);

    gpio_config();
    init_uart_hc06();
    init_pwm();
    oled_init();

    ssd1306_clear(&disp);
    ssd1306_draw_string(&disp, 8, 12, 2, "Aguardando");
    ssd1306_show(&disp);
    ssd1306_clear(&disp);

    xSemaphorePIN = xSemaphoreCreateBinary();
    xSemaphoreConnection = xSemaphoreCreateBinary();

    xQueueTX = xQueueCreate(32, sizeof(adc_t));

    xTaskCreate(bluetooth_task, "Task PIN", 256, NULL, 1, NULL);
    xTaskCreate(pwm_task, "PWM Task", 128, NULL, 1, NULL);

    vTaskStartScheduler();

    // Should never reach here
    for (;;)
        ;
}
