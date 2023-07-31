/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_spiffs.h"

//#include "esp_adc/adc_oneshot.h"
//#include "esp_adc/adc_cali.h"
//#include "esp_adc/adc_cali_scheme.h"

#include "bsp/esp32_s3_korvo_2.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_ili9341.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_touch_tt21100.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "esp_codec_dev_defaults.h"
#include "bsp_err_check.h"

/* Battery voltage measurement is disabled, because it waits for update button component. */
#define BSP_BATTERY_ENABLED 0

/**
 * @brief I2S pinout
 *
 * Can be used for i2s_std_gpio_config_t and/or i2s_std_config_t initialization
 */
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

/**
 * @brief Mono Duplex I2S configuration structure
 */
#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

static const char *TAG = "S3-KORVO-2";

static esp_io_expander_handle_t io_expander = NULL; // IO expander tca9554 handle
//static adc_oneshot_unit_handle_t adc1_handle; // ADC1 handle; for USB voltage measurement
//static adc_cali_handle_t adc1_cali_handle; // ADC1 calibration handle
sdmmc_card_t *bsp_sdcard = NULL;    // Global uSD card handler
static esp_lcd_touch_handle_t tp;   // LCD touch handle
static lv_indev_t *disp_indev = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL;  /* Codec data interface */
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static bool i2c_initialized = false;

// This is just a wrapper to get function signature for espressif/button API callback
static uint8_t bsp_get_main_button(void *param);
static esp_err_t bsp_init_main_button(void *param);

static const button_config_t bsp_button_config[BSP_BUTTON_NUM] = {
    {
        .type = BUTTON_TYPE_ADC,
        .adc_button_config.adc_channel = ADC_CHANNEL_4, // ADC1 channel 4 is GPIO5
        .adc_button_config.button_index = BSP_BUTTON_REC,
        .adc_button_config.min = 2310, // middle is 2410mV
        .adc_button_config.max = 2510
    },
    {
        .type = BUTTON_TYPE_ADC,
        .adc_button_config.adc_channel = ADC_CHANNEL_4, // ADC1 channel 4 is GPIO5
        .adc_button_config.button_index = BSP_BUTTON_MUTE,
        .adc_button_config.min = 1880, // middle is 1980mV
        .adc_button_config.max = 2080
    },
    {
        .type = BUTTON_TYPE_ADC,
        .adc_button_config.adc_channel = ADC_CHANNEL_4, // ADC1 channel 4 is GPIO5
        .adc_button_config.button_index = BSP_BUTTON_PLAY,
        .adc_button_config.min = 1550, // middle is 1650mV
        .adc_button_config.max = 1750
    },
    {
        .type = BUTTON_TYPE_ADC,
        .adc_button_config.adc_channel = ADC_CHANNEL_4, // ADC1 channel 4 is GPIO5
        .adc_button_config.button_index = BSP_BUTTON_SET,
        .adc_button_config.min = 1010, // middle is 1110mV
        .adc_button_config.max = 1210
    },
    {
        .type = BUTTON_TYPE_ADC,
        .adc_button_config.adc_channel = ADC_CHANNEL_4, // ADC1 channel 4 is GPIO5
        .adc_button_config.button_index = BSP_BUTTON_VOLDOWN,
        .adc_button_config.min = 720, // middle is 820mV
        .adc_button_config.max = 920
    },
    {
        .type = BUTTON_TYPE_ADC,
        .adc_button_config.adc_channel = ADC_CHANNEL_4, // ADC1 channel 4 is GPIO5
        .adc_button_config.button_index = BSP_BUTTON_VOLUP,
        .adc_button_config.min = 280, // middle is 380mV
        .adc_button_config.max = 480
    },
    {
        .type = BUTTON_TYPE_CUSTOM,
        .custom_button_config.button_custom_init = bsp_init_main_button,
        .custom_button_config.button_custom_get_key_value = bsp_get_main_button,
        .custom_button_config.button_custom_deinit = NULL,
        .custom_button_config.active_level = 1,
        .custom_button_config.priv = (void *) BSP_BUTTON_MAIN,
    }
};

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized) {
        return ESP_OK;
    }

    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = BSP_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = CONFIG_BSP_I2C_CLK_SPEED_HZ
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_param_config(BSP_I2C_NUM, &i2c_conf));
    BSP_ERROR_CHECK_RETURN_ERR(i2c_driver_install(BSP_I2C_NUM, i2c_conf.mode, 0, 0, 0));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_driver_delete(BSP_I2C_NUM));
    i2c_initialized = false;
    return ESP_OK;
}


esp_err_t bsp_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

    return esp_vfs_fat_sdmmc_mount(BSP_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
}

esp_err_t bsp_sdcard_unmount(void)
{
    return esp_vfs_fat_sdcard_unmount(BSP_MOUNT_POINT, bsp_sdcard);
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config, i2s_chan_handle_t *tx_channel, i2s_chan_handle_t *rx_channel)
{
    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    BSP_ERROR_CHECK_RETURN_ERR(i2s_new_channel(&chan_cfg, tx_channel, rx_channel));

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *p_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        p_i2s_cfg = i2s_config;
    }

    if (tx_channel != NULL) {
        BSP_ERROR_CHECK_RETURN_ERR(i2s_channel_init_std_mode(*tx_channel, p_i2s_cfg));
        BSP_ERROR_CHECK_RETURN_ERR(i2s_channel_enable(*tx_channel));
    }
    if (rx_channel != NULL) {
        BSP_ERROR_CHECK_RETURN_ERR(i2s_channel_init_std_mode(*rx_channel, p_i2s_cfg));
        BSP_ERROR_CHECK_RETURN_ERR(i2s_channel_enable(*rx_channel));
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = *rx_channel,
        .tx_handle = *tx_channel,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK(i2s_data_if, ESP_FAIL);

    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (i2s_tx_chan == NULL || i2s_rx_chan == NULL || i2s_data_if == NULL) {
        /* Initialize I2C and I2S */
        BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
        BSP_ERROR_CHECK_RETURN_ERR(bsp_audio_init(NULL, &i2s_tx_chan, &i2s_rx_chan));
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    BSP_NULL_CHECK(es8311_dev, NULL);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (i2s_tx_chan == NULL || i2s_rx_chan == NULL || i2s_data_if == NULL) {
        /* Initialize I2C and I2S */
        BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
        BSP_ERROR_CHECK_RETURN_ERR(bsp_audio_init(NULL, &i2s_tx_chan, &i2s_rx_chan));
    }
    assert(i2s_data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    BSP_NULL_CHECK(es7210_dev, NULL);

    esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es7210_dev_cfg);
}

esp_io_expander_handle_t bsp_io_expander_init(void)
{
    if (io_expander) {
        ESP_LOGD(TAG, "io_expander is initialized");
    } else {
        // Here we try to initialize TCA9554 first, if it fails, we try to initialize TCA9554A
        // *INDENT-OFF*
        if ((esp_io_expander_new_i2c_tca9554(BSP_I2C_NUM, BSP_IO_EXPANDER_I2C_ADDRESS_TCA9554, &io_expander) != ESP_OK) &&
            (esp_io_expander_new_i2c_tca9554(BSP_I2C_NUM, BSP_IO_EXPANDER_I2C_ADDRESS_TCA9554A, &io_expander) != ESP_OK)) {
            ESP_LOGE(TAG, "Failed to initialize IO expander");
            return NULL;
        }
        // *INDENT-OFF*
    }

    return io_expander;
}

// Bit number used to represent command and parameter
#define LCD_CMD_BITS           8
#define LCD_PARAM_BITS         8

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    assert(config != NULL && config->max_transfer_sz > 0);

    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_PCLK,
        .mosi_io_num = BSP_LCD_DATA0,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = config->max_transfer_sz,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = BSP_LCD_CS,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, ret_io), err, TAG, "New panel IO failed");

    ESP_LOGD(TAG, "Install LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST, // Shared with Touch reset
        .color_space = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ili9341(*ret_io, &panel_config, ret_panel), err, TAG, "New panel failed");

    BSP_NULL_CHECK(bsp_io_expander_init(), ESP_ERR_INVALID_STATE);
    ESP_GOTO_ON_ERROR(esp_io_expander_set_dir(io_expander, BSP_LCD_IO_CS, IO_EXPANDER_OUTPUT), err, TAG, "");
    ESP_GOTO_ON_ERROR(esp_io_expander_set_dir(io_expander, BSP_LCD_IO_RST, IO_EXPANDER_OUTPUT), err, TAG, "");
    ESP_GOTO_ON_ERROR(esp_io_expander_set_dir(io_expander, BSP_LCD_IO_BACKLIGHT, IO_EXPANDER_OUTPUT), err, TAG, "");
    ESP_GOTO_ON_ERROR(esp_io_expander_set_level(io_expander, BSP_LCD_IO_CS, 0), err, TAG, "");

    // Reset LCD
    ESP_GOTO_ON_ERROR(esp_io_expander_set_level(io_expander, BSP_LCD_IO_RST, 0), err, TAG, "");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_GOTO_ON_ERROR(esp_io_expander_set_level(io_expander, BSP_LCD_IO_RST, 1), err, TAG, "");
    vTaskDelay(pdMS_TO_TICKS(10));

    // Enable display
    ESP_GOTO_ON_ERROR(esp_io_expander_set_level(io_expander, BSP_LCD_IO_CS, 0), err, TAG, "");

    esp_lcd_panel_init(*ret_panel);
    esp_lcd_panel_mirror(*ret_panel, true, true);
    return ret;

err:
    if (*ret_panel) {
        esp_lcd_panel_del(*ret_panel);
    }
    if (*ret_io) {
        esp_lcd_panel_io_del(*ret_io);
    }
    spi_bus_free(BSP_LCD_SPI_NUM);
    return ret;
}

static lv_disp_t *bsp_display_lcd_init(void)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;
    const bsp_display_config_t bsp_disp_cfg = {
        .max_transfer_sz = BSP_LCD_DRAW_BUFF_SIZE * sizeof(uint16_t),
    };
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&bsp_disp_cfg, &panel_handle, &io_handle));

    esp_lcd_panel_disp_on_off(panel_handle, true);

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
        }
    };

    return lvgl_port_add_disp(&disp_cfg);
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM, &tp_io_config, &tp_io_handle), TAG, "");
    return esp_lcd_touch_new_i2c_tt21100(tp_io_handle, &tp_cfg, ret_touch);
}

static lv_indev_t *bsp_display_indev_init(lv_disp_t *disp)
{
    if (tp == NULL) {
        BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(NULL, &tp));
    }
    assert(tp);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };

    return lvgl_port_add_touch(&touch_cfg);
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    ESP_LOGW(TAG, "This board doesn't support to change brightness of LCD");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_display_backlight_off(void)
{
    BSP_NULL_CHECK(bsp_io_expander_init(), ESP_ERR_INVALID_STATE);
    return esp_io_expander_set_level(io_expander, BSP_LCD_IO_BACKLIGHT, 0);
}

esp_err_t bsp_display_backlight_on(void)
{
    BSP_NULL_CHECK(bsp_io_expander_init(), ESP_ERR_INVALID_STATE);
    return esp_io_expander_set_level(io_expander, BSP_LCD_IO_BACKLIGHT, 1);
}

lv_disp_t *bsp_display_start(void)
{
    lv_disp_t *disp = NULL;
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    BSP_ERROR_CHECK_RETURN_NULL(lvgl_port_init(&lvgl_cfg));

    BSP_NULL_CHECK(disp = bsp_display_lcd_init(), NULL);

    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(disp), NULL);

    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

void bsp_display_rotate(lv_disp_t *disp, lv_disp_rot_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

esp_err_t bsp_voltage_init(void)
{
#if BSP_BATTERY_ENABLED
    // Init ADC1
    const adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    BSP_ERROR_CHECK_RETURN_ERR(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    // Init ADC1 channels
    const adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_11,
    };
    BSP_ERROR_CHECK_RETURN_ERR(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_5, &config));

    // ESP32-S3 supports Curve Fitting calibration scheme
    const adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    BSP_ERROR_CHECK_RETURN_ERR(adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle));
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Battery voltage measurement is disabled, because it waits for update button component.");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int bsp_voltage_battery_get(void)
{
#if BSP_BATTERY_ENABLED
    int voltage, adc_raw;

    assert(adc1_handle);
    BSP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_5, &adc_raw), -1);
    BSP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &voltage), -1);
    return voltage * BSP_BATTERY_VOLTAGE_DIV;
#else
    ESP_LOGW(TAG, "Battery voltage measurement is disabled, because it waits for update button component.");
    return -1;
#endif
}

esp_err_t bsp_leds_init(void)
{
    BSP_NULL_CHECK(bsp_io_expander_init(), ESP_ERR_INVALID_STATE);
    BSP_ERROR_CHECK_RETURN_ERR(esp_io_expander_set_dir(io_expander, BSP_LED_RED, IO_EXPANDER_OUTPUT));
    BSP_ERROR_CHECK_RETURN_ERR(esp_io_expander_set_dir(io_expander, BSP_LED_BLUE, IO_EXPANDER_OUTPUT));
    BSP_ERROR_CHECK_RETURN_ERR(esp_io_expander_set_level(io_expander, BSP_LED_RED, true));
    BSP_ERROR_CHECK_RETURN_ERR(esp_io_expander_set_level(io_expander, BSP_LED_BLUE, true));
    return ESP_OK;
}

esp_err_t bsp_led_set(const bsp_led_t led_io, const bool on)
{
    BSP_ERROR_CHECK_RETURN_ERR(esp_io_expander_set_level(io_expander, led_io, !on));
    return ESP_OK;
}

esp_err_t bsp_button_init(const bsp_button_t btn)
{
    return ESP_OK;
}

bool bsp_button_get(const bsp_button_t btn)
{
    if (btn == BSP_BUTTON_MAIN) {
#if (CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS > 0)
        uint8_t home_btn_val = 0x00;
        assert(tp);

        esp_lcd_touch_get_button_state(tp, 0, &home_btn_val);
        return home_btn_val ? true : false;
#else
        ESP_LOGE(TAG, "Button main is inaccessible");
        return false;
#endif
    }

    return false;
}

static uint8_t bsp_get_main_button(void *param)
{
    assert(tp);
    ESP_ERROR_CHECK(esp_lcd_touch_read_data(tp));
#if (CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS > 0)
    uint8_t home_btn_val = 0x00;
    esp_lcd_touch_get_button_state(tp, 0, &home_btn_val);
    return home_btn_val ? true : false;
#else
    ESP_LOGE(TAG, "Button main is inaccessible");
    return false;
#endif
}

static esp_err_t bsp_init_main_button(void *param)
{
    if (tp == NULL) {
        BSP_ERROR_CHECK_RETURN_ERR(bsp_touch_new(NULL, &tp));
    }
    return ESP_OK;
}

esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size)
{
    esp_err_t ret = ESP_OK;
    if ((btn_array_size < BSP_BUTTON_NUM) ||
            (btn_array == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (btn_cnt) {
        *btn_cnt = 0;
    }
    for (int i = 0; i < BSP_BUTTON_NUM; i++) {
        btn_array[i] = iot_button_create(&bsp_button_config[i]);
        if (btn_array[i] == NULL) {
            ret = ESP_FAIL;
            break;
        }
        if (btn_cnt) {
            (*btn_cnt)++;
        }
    }
    return ret;
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}
