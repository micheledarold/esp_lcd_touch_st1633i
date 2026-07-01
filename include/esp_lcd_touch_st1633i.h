#ifndef ESP_LCD_TOUCH_ST1633I_H_
#define ESP_LCD_TOUCH_ST1633I_H_

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern extern "C" {
#endif

#define ESP_LCD_TOUCH_IO_I2C_ST1633I_ADDRESS          (0x55)

/**
 * @brief Inizializza un nuovo handle esp_lcd_touch_handle_t per ST1633i
 * 
 * @param[in] io_handle I2C IO handle precedentemente creato con esp_lcd_new_panel_io_i2c()
 * @param[in] config Configurazione generica del touch panel
 * @param[out] out_touch_handle Handle dell'istanza touch restituita
 * @return esp_err_t ESP_OK in caso di successo
 */
esp_err_t esp_lcd_touch_new_i2c_st1633i(const esp_lcd_panel_io_handle_t io_handle, 
                                        const esp_lcd_touch_config_t *config, 
                                        esp_lcd_touch_handle_t *out_touch_handle);

#define ESP_LCD_TOUCH_IO_I2C_ST1633I_CONFIG()           \
    {                                                   \
        .scl_speed_hz = 100000,                         \
        .dev_addr = CONFIG_ESP_LCD_TOUCH_ST1633I_I2C_ADDRESS, \
        .control_phase_bytes = 1,                       \
        .dc_bit_offset = 0,                             \
        .lcd_cmd_bits = 8,                             \
        .flags =                                        \
        {                                               \
            .disable_control_phase = 1,                 \
        }                                               \
    }

#ifdef __cplusplus
}
#endif

#endif /* ESP_LCD_TOUCH_ST1633I_H_ */