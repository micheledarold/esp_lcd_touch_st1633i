#ifndef ESP_LCD_TOUCH_ST1633I_H_
#define ESP_LCD_TOUCH_ST1633I_H_

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern extern "C" {
#endif

#define ESP_LCD_TOUCH_IO_I2C_ST1633I_ADDRESS          (0x55)

// Tenere allineato manualmente al campo "version" di idf_component.yml
#define ST1633I_COMPONENT_VERSION                     "0.2.1"

/**
 * @brief Configurazione opzionale del pin di alimentazione e delle soglie del watchdog
 *        di recovery, da passare tramite esp_lcd_touch_config_t::driver_data. Se non
 *        impostata (driver_data == NULL) o con power_gpio_num == GPIO_NUM_NC, il
 *        power-cycle e' disabilitato e il recovery usa solo il reset via rst_gpio_num.
 *        Le soglie a 0 usano i default interni del componente.
 */
typedef struct {
    gpio_num_t power_gpio_num;        /*!< Pin che alimenta il touch, o GPIO_NUM_NC se assente */
    unsigned int power_off_level : 1; /*!< Livello del pin quando il touch e' spento */
    uint16_t i2c_fail_reset_threshold; /*!< Letture I2C fallite consecutive prima di un reset; 0 = default */
    uint16_t reset_escalate_threshold; /*!< Reset falliti consecutivi prima di un power-cycle; 0 = default */
} st1633i_recovery_config_t;

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