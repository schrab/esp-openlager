
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include <ctype.h>


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

void __attribute__((noreturn)) led_panic(const char *msg) {
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

// Check for next available log file: [prefix]000.bbl, [prefix]001.bbl ...
void get_log_filename(char *filename, size_t len, const char *prefix) {
    if (prefix == NULL || strlen(prefix) == 0) {
        prefix = "log";
    }
    
    for (int i = 0; i < 1000; i++) {
        snprintf(filename, len, "%s/%s_%03d.bbl", MOUNT_POINT, prefix, i);
        struct stat st;
        if (stat(filename, &st) != 0) {
            // File does not exist, use this one (if ENOENT)
            if (errno == ENOENT) {
                return;
            }
            // Other error? Log it but maybe still try next?
            ESP_LOGW(TAG, "stat failed for %s: %s", filename, strerror(errno));
            // Should we continue or abort? Abort for EIO.
            if (errno == EIO) {
                led_panic("SD_ERR");
            }
            return;
        }
    }
    // If all full, overwrite log999.bbl or just fail? 
    // Original behavior: Panic "FILES"
    led_panic("FILES");
}

// Function to sanitize filename
// Replaces invalid chars with _. 
// If strict_83 is true, enforces 8.3 format (max 8 chars, alnum only, uppercase)
void sanitize_filename(char *name, bool strict_83) {
    char *src = name;
    char *dst = name;
    int count = 0;
    while (*src) {
        char c = *src;
        if (strict_83) {
            c = toupper((unsigned char)c);
            if (isalnum((unsigned char)c) || c == '_' || c == '-') {
                if (count < 8) {
                    *dst++ = c;
                    count++;
                }
            }
        } else {
            // LFN: Allow slightly more but safe chars
            if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') {
                *dst++ = c;
            } else {
                 // Replace spaces or others with _
                 *dst++ = '_';
            }
        }
        src++;
    }
    *dst = 0;
}

// Function to find "H Craft name:[Name]\n" in buffer
// Returns true if found and populates craft_name
bool find_craft_name(const uint8_t *buf, size_t len, char *craft_name, size_t max_name_len) {
    const char *tag = "H Craft name:";
    size_t tag_len = strlen(tag);
    
    // Simple search
    for (size_t i = 0; i < len - tag_len; i++) {
        if (memcmp(buf + i, tag, tag_len) == 0) {
            // Found tag, now capture name until newline
            size_t start = i + tag_len;
            size_t end = start;
            while (end < len && buf[end] != '\n' && buf[end] != '\r') {
                end++;
            }
            
            size_t name_len = end - start;
            if (name_len > 0) {
                if (name_len >= max_name_len) name_len = max_name_len - 1;
                memcpy(craft_name, buf + start, name_len);
                craft_name[name_len] = 0;
                // Sanitize for LFN initially
                sanitize_filename(craft_name, false);
                return strlen(craft_name) > 0;
            }
        }
    }
    return false;
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


    // Allocate buffer once
    io_buffer = (uint8_t *)malloc(IO_BUF_SIZE);
    if (!io_buffer) {
        led_panic("MALLOC");
    }

    // --- 5. Main Loop (Infinite Session Loop) ---
    // Loop forever: Wait for data -> Open File -> Log -> Close File -> Repeat
    while (1) {
        
        // --- A. Wait for Initial Data (ARMING) ---
        ESP_LOGI(TAG, "Waiting for initial data (ARMING)...");
        char craft_name[32] = {0};
        char filename[128];
        int initial_len = 0;
        int idle_counter = 0;
        
        while (initial_len <= 0) {
            // Heartbeat blink while waiting (Brief flash every 2s)
            // idle_counter increments every loop iteration (~100ms)
            // Blink ON at count 0, OFF at count 1. 2s period = 20 loops.
            if (idle_counter == 0) {
                 led_set(true); 
            } else if (idle_counter == 1) {
                 led_set(false);
            }
            
            // Every ~2s (20 loops), poke the SD card to keep it alive
            idle_counter++;
            if (idle_counter >= 20) {
                struct stat st;
                if (stat(MOUNT_POINT, &st) != 0) {
                     // Keep alive failed, not critical but good to know
                }
                idle_counter = 0;
            }

            size_t available = 0;
            uart_get_buffered_data_len(UART_NUM, &available);
            
            // If checking frequently (100ms), we won't overflow 64KB buffer quickly.
            // 2Mbps = 200KB/s. 100ms = 20KB. Buffer is 64KB. Safe.
            if (available > 0) {
                 initial_len = uart_read_bytes(UART_NUM, io_buffer, IO_BUF_SIZE, pdMS_TO_TICKS(100));
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        
        ESP_LOGI(TAG, "Received initial data: %d bytes (ARMED)", initial_len);
        
        // --- B. Determine Filename ---
        if (initial_len > 0) {
            if (find_craft_name(io_buffer, initial_len, craft_name, sizeof(craft_name))) {
                ESP_LOGI(TAG, "Found Craft Name: %s", craft_name);
                get_log_filename(filename, sizeof(filename), craft_name); 
            } else {
                ESP_LOGI(TAG, "Craft Name not found in first chunk.");
                get_log_filename(filename, sizeof(filename), "log");
            }
        } else {
             get_log_filename(filename, sizeof(filename), "log");
        }

        ESP_LOGI(TAG, "Opening file: %s", filename);

        // --- C. Open Log File ---
        FILE *f = fopen(filename, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open file %s: %s. retrying fallback...", filename, strerror(errno));
            get_log_filename(filename, sizeof(filename), "LOG");
            f = fopen(filename, "wb");
            if (f == NULL) {
                 ESP_LOGE(TAG, "Fallback failed: %s", strerror(errno));
                 led_panic("FOPEN");
            }
        }
        
        // Write the initial chunk
        if (initial_len > 0) {
            fwrite(io_buffer, 1, (size_t)initial_len, f);
        }
        
        // --- D. Logging Loop ---
        ESP_LOGI(TAG, "Logging started...");
        led_set(false); 
        
        int silence_timeout_ms = 0;
        
        while (1) {
            // Read from UART with short timeout (20ms)
            int len = uart_read_bytes(UART_NUM, io_buffer, IO_BUF_SIZE, pdMS_TO_TICKS(20));

            if (len > 0) {
                silence_timeout_ms = 0; // Reset timeout
                led_set(true); // Flash LED on write
                
                size_t written = fwrite(io_buffer, 1, (size_t)len, f);
                if (written != (size_t)len) {
                    ESP_LOGE(TAG, "Write error!");
                    led_panic("WRITE");
                }
                led_set(false);
            } else {
                // No data received
                silence_timeout_ms += 20;
                
                // If silent for > 2 seconds, assume DISARMED -> Close file
                if (silence_timeout_ms > 2000) {
                    ESP_LOGI(TAG, "No data for 2s (DISARMED). Closing file.");
                    break; // Break inner loop to close file
                }
                
                // Still flush occasionally during short pauses?
                if (silence_timeout_ms % 500 == 0) {
                    fflush(f);
                    fsync(fileno(f));
                }
            }
        }
        
        // --- E. Close File ---
        fclose(f);
        ESP_LOGI(TAG, "File closed. Returning to wait state.");
        
        // Loop back to A (Wait for Arming)
    }
}
