
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

// Configuration from Kconfig
#define UART_NUM                UART_NUM_1
#define UART_BAUD_RATE          CONFIG_OPENLAGER_UART_BAUD
#define UART_RX_PIN             CONFIG_OPENLAGER_UART_RX_PIN
#define UART_TX_PIN             UART_PIN_NO_CHANGE // We don't transmit
#define UART_BUF_SIZE           (64 * 1024) // 64KB Buffer

#define MOUNT_POINT             "/sdcard"
#define SPI_MOSI_PIN            CONFIG_OPENLAGER_SPI_MOSI_PIN
#define SPI_MISO_PIN            CONFIG_OPENLAGER_SPI_MISO_PIN
#define SPI_CLK_PIN             CONFIG_OPENLAGER_SPI_CLK_PIN
#define SPI_CS_PIN              CONFIG_OPENLAGER_SPI_CS_PIN

#define LED_PIN                 CONFIG_OPENLAGER_LED_PIN

static const char *TAG = "OpenLager";

// Buffer for reading from UART and writing to SD
// 64KB to match UART buffer, but we read in chunks
#define IO_BUF_SIZE (16 * 1024) 
static uint8_t *io_buffer;

void led_init() {
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0); // Start OFF (assuming active high for now, check board)
    // NOTE: SuperMini LED is usually active LOW (0 = ON). 
    // Let's assume Active Low for "ON".
}

void led_set(bool on) {
    gpio_set_level(LED_PIN, on ? 0 : 1); // Active Low
}

void led_panic(const char *msg) {
    ESP_LOGE(TAG, "PANIC: %s", msg);
    while (1) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Check for next available log file: log000.txt, log001.txt ...
void get_log_filename(char *filename, size_t len) {
    for (int i = 0; i < 1000; i++) {
        snprintf(filename, len, "%s/log%03d.txt", MOUNT_POINT, i);
        struct stat st;
        if (stat(filename, &st) != 0) {
            // File does not exist, use this one
            return;
        }
    }
    // If all full, overwrite log999.txt or just fail? 
    // Original behavior: Panic "FILES"
    led_panic("FILES");
}

void app_main(void) {
    esp_err_t ret;

    // --- 1. Init LED ---
    led_init();
    led_set(true); // ON during init

    ESP_LOGI(TAG, "Initializing OpenLager ESP32-C3...");

    // --- 2. Mount SD Card via SPI ---
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    ESP_LOGI(TAG, "Initializing SD card");

    ESP_LOGI(TAG, "Using SPI peripheral");
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 20000; // 20MHz for now

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    // SPI2_HOST is FSPI on C3? No, C3 has SPI2_HOST (SPI2).
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        led_panic("SPI_BUS");
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SPI_CS_PIN;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
            led_panic("SD_MOUNT");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
            led_panic("SD_INIT");
        }
    }
    ESP_LOGI(TAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, card);

    // --- 3. Init UART ---
    ESP_LOGI(TAG, "Initializing UART");
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver, and get the queue.
    ret = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) led_panic("UART_DRV");

    ret = uart_param_config(UART_NUM, &uart_config);
    if (ret != ESP_OK) led_panic("UART_CFG");

    ret = uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) led_panic("UART_PIN");


    // --- 4. Open Log File ---
    char filename[64];
    get_log_filename(filename, sizeof(filename));
    ESP_LOGI(TAG, "Opening file: %s", filename);

    FILE *f = fopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        led_panic("FOPEN");
    }

    // Allocate buffer
    io_buffer = (uint8_t *)malloc(IO_BUF_SIZE);
    if (!io_buffer) {
        led_panic("MALLOC");
    }

    // --- 5. Main Loop ---
    ESP_LOGI(TAG, "Starting logging loop...");
    
    // We want to write in chunks.
    // If we receive data, we write it.
    // If we don't receive data for a while, we fsync?
    // High speed logging: 2Mbps ~= 200KB/s. 
    // IO_BUF_SIZE is 16KB. That fills in ~80ms.

    led_set(false); // Off when idle/working normally

    while (1) {
        // Read from UART
        // We handle the "chunking" by reading whatever is available up to buf size
        // blocking for a short time (e.g. 10 ticks).
        // If buffer fills up, SD write might be slow, so UART driver buffer (64K) handles the burst.
        
        // Wait up to 20ms for data.
        int len = uart_read_bytes(UART_NUM, io_buffer, IO_BUF_SIZE, pdMS_TO_TICKS(20));

        if (len > 0) {
            led_set(true); // Flash LED on write
            size_t written = fwrite(io_buffer, 1, len, f);
            if (written != len) {
                ESP_LOGE(TAG, "Write error!");
                led_panic("WRITE");
            }
            led_set(false);
        } else {
            // No data for 20ms, maybe sync?
            // Doing fsync too often is bad for performance/wear?
            // But good for safety.
            // Let's synced every 1s of inactivity? Or just let runtime handle it?
            // For now, let's just flush every time we have a timeout if we wrote something recently.
            // Actually, stdio buffering might hold data.
            // Let's force flush if we had a pause.
            fflush(f);
            fsync(fileno(f));
        }
    }
}
