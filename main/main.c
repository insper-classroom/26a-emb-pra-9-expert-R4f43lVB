#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>

#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "ssd1306.h"
#include "hc06.h"


// Tela OLED
ssd1306_t disp;

// Pinos da Tela
const uint GPIO_14 = 14;
const uint GPIO_15 = 15;
const uint GPIO_16 = 16;
const uint GPIO_17 = 17;

// PINS
#define PIN_X 27
#define PIN_Y 26
#define BTN_PIN 16

#define UART_ID uart0
#define BAUD_RATE 115200

// Semaforos
SemaphoreHandle_t xSemaphorePIN;

typedef struct adc {
    int axis;
    int value;
} adc_t;

typedef struct config_data {
    char *name;
    char *pin;
} config_data_t;

// Filas
QueueHandle_t xQueuePIN;
QueueHandle_t xQueueConfig;
QueueHandle_t xQueueRX;
QueueHandle_t xQueueTX;
QueueHandle_t xQueueADC;

void uart_rx_handler() {
    uint8_t ch = uart_getc(HC06_UART_ID);
    xQueueSendFromISR(xQueueRX, &ch, 0);
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
    uint8_t ch;
    while (true) {
        if (xQueueReceive(xQueueTX, &ch, portMAX_DELAY) == pdTRUE) {
            uart_putc_raw(HC06_UART_ID, ch);
        }
    }
}

static void serial_task(void* p) {
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
    adc_gpio_init(PIN_X);

    adc_t data;
    data.axis = 0;

    int soma = 0;
    int numeros[5] = {0, 0, 0, 0, 0};
    while (true) {
        int result;
        adc_select_input(1);
        result = ((int) adc_read() - 2047) / 8;

        int velho = numeros[0];
        for (int i = 0; i < 4; i++) numeros[i] = numeros[i+1];
        numeros[4] = result;
        soma += result - velho;
        int media = soma / 5;
        if (media > 50 || media < -50) {
            data.value = media;
            xQueueSend(xQueueADC, &data, pdMS_TO_TICKS(50));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void y_task(void *p) {
    adc_gpio_init(PIN_Y);

    adc_t data;
    data.axis = 1;

    int soma = 0;
    int numeros[5] = {0, 0, 0, 0, 0};
    while (true) {
        int result;
        adc_select_input(0);
        result = ((int) adc_read() - 2047) / 8;

        int velho = numeros[0];
        for (int i = 0; i < 4; i++) numeros[i] = numeros[i+1];
        numeros[4] = result;
        soma += result - velho;
        int media = soma / 5;
        if (media > 50 || media < -50) {
            data.value = media;
            xQueueSend(xQueueADC, &data, pdMS_TO_TICKS(50));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void uart_task(void *p) {
    adc_t data;

    while (true) {
        if (xQueueReceive(xQueueADC, &data, pdMS_TO_TICKS(50))) {
            uart_putc(UART_ID, data.axis);
            uart_putc(UART_ID, data.value);
            uart_putc(UART_ID, (data.value >> 8));
            uart_putc(UART_ID, -1);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void pin_task(void *p) {
    config_data_t data;
    data.name = "RAFA";

    char pin[5];
    srand(time_us_32());
    while (true) {
        if (xSemaphoreTake(xSemaphorePIN, pdMS_TO_TICKS(10))) {
            for (int i = 0; i < 4; i++) {
                pin[i] = '0' + rand() % 10;
            }
            pin[4] = '\x0';
            data.pin = pin;
            
            ssd1306_clear(&disp);
            ssd1306_draw_string(&disp, 64, 16, 1, pin);
            ssd1306_show(&disp);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void btn_callback(uint gpio, uint32_t events) {
    if (gpio == BTN_PIN && events == GPIO_IRQ_EDGE_FALL) {
        xSemaphoreGiveFromISR(xSemaphorePIN, 0);
    } 
}


// Funcoes auxiliares
void init_buttons(void) {
    gpio_init(BTN_PIN);
    gpio_set_dir(BTN_PIN, GPIO_IN);
    gpio_pull_up(BTN_PIN);
    gpio_set_irq_enabled_with_callback(BTN_PIN, GPIO_IRQ_EDGE_FALL, true, &btn_callback);
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

void gpio_config(void) {
    
    gpio_init(GPIO_14);
    gpio_set_dir(GPIO_14, GPIO_IN);
    gpio_pull_up(GPIO_14);

    gpio_init(GPIO_15);
    gpio_set_dir(GPIO_15, GPIO_IN);
    gpio_pull_up(GPIO_15);
    
    gpio_init(GPIO_16);
    gpio_set_dir(GPIO_16, GPIO_IN);
    gpio_pull_up(GPIO_16);
    
    gpio_init(GPIO_17);
    gpio_set_dir(GPIO_17, GPIO_IN);
    gpio_pull_up(GPIO_17);
}

int main(void) {
    stdio_init_all();

    init_buttons();
    init_uart_hc06();
    
    gpio_config();
    oled_init();

    ssd1306_clear(&disp);
    ssd1306_draw_string(&disp, 8, 12, 1, "Iniciando...");
    ssd1306_show(&disp);
    sleep_ms(1000);
    ssd1306_clear(&disp);

    xSemaphorePIN = xSemaphoreCreateBinary();

    xQueuePIN = xQueueCreate(32, sizeof(char[5]));
    xQueueConfig = xQueueCreate(32, sizeof(config_data_t));
    xQueueRX = xQueueCreate(32, sizeof(uint8_t));
    xQueueTX = xQueueCreate(32, sizeof(uint8_t));
    xQueueADC = xQueueCreate(32, sizeof(adc_t));

    xTaskCreate(pin_task, "Task do PIN", 256, NULL, 1, NULL);
    xTaskCreate(tx_task, "TX", 512, NULL, 2, NULL);
    xTaskCreate(serial_task, "Serial", 1024, NULL, 1, NULL);

    vTaskStartScheduler();

    // Should never reach here
    for (;;)
        ;
}
