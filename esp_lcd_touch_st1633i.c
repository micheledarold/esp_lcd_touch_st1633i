#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_st1633i.h"

static const char *TAG = "st1633i";

// Registri e costanti ST1633i (Sitronix Touch IC Protocol A)
// NB: 0x10 e' l'"Advanced Touch Info" (gesture/prossimita'), NON un conteggio dita:
// la presenza di un tocco si legge dal bit Valid del blocco coordinate.
// 0x12: XY0 Coord High Byte -> bit7=Valid0, bit6-4=X0_H(3b), bit3=reserved, bit2-0=Y0_H(3b)
// 0x13: X0_L, 0x14: Y0_L, 0x15: reserved/strength
#define ST1633I_REG_XY0              0x12
#define ST1633I_REG_STATUS           0x01    // bit7-4: Error Code, bit3-0: Device Status (0x0=Normal, 0x1=Init, ...)
#define ST1633I_REG_IDLE_TIMEOUT     0x03    // secondi di inattivita' prima di entrare in Idle; 0xFF = disabilitato
#define ST1633I_IDLE_WATCHDOG_PERIOD 200     // ogni quante letture ri-verificare/forzare l'uscita da Idle

#define ST1633I_I2C_FAIL_RESET_THRESHOLD   5   // letture fallite di fila -> tenta un reset
#define ST1633I_RESET_ESCALATE_THRESHOLD   3   // reset falliti di fila -> escala a power-cycle

static esp_err_t st1633i_read_data(esp_lcd_touch_handle_t tp);
static bool st1633i_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t st1633i_del(esp_lcd_touch_handle_t tp);
static esp_err_t st1633i_reset(esp_lcd_touch_handle_t tp);
static esp_err_t st1633i_power_cycle(esp_lcd_touch_handle_t tp);
static esp_err_t st1633i_set_swap_xy(esp_lcd_touch_handle_t tp, bool swap);
static esp_err_t st1633i_get_swap_xy(esp_lcd_touch_handle_t tp, bool *swap);
static esp_err_t st1633i_set_mirror_x(esp_lcd_touch_handle_t tp, bool mirror);
static esp_err_t st1633i_get_mirror_x(esp_lcd_touch_handle_t tp, bool *mirror);
static esp_err_t st1633i_set_mirror_y(esp_lcd_touch_handle_t tp, bool mirror);
static esp_err_t st1633i_get_mirror_y(esp_lcd_touch_handle_t tp, bool *mirror);

esp_err_t esp_lcd_touch_new_i2c_st1633i(const esp_lcd_panel_io_handle_t io_handle,
                                        const esp_lcd_touch_config_t *config,
                                        esp_lcd_touch_handle_t *out_touch_handle)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t touch_handle = NULL;

    ESP_LOGI(TAG, "esp_lcd_touch_new_i2c_st1633i (component v%s)", ST1633I_COMPONENT_VERSION);

    ESP_GOTO_ON_FALSE(io_handle && config && out_touch_handle, ESP_ERR_INVALID_ARG, err, TAG, "Invalid arguments");

    // Alloca memoria per l'istanza principale dell'interfaccia esp_lcd_touch
    touch_handle = (esp_lcd_touch_handle_t)calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(touch_handle, ESP_ERR_NO_MEM, err, TAG, "Cancel alloc touch structure");

    // Lo spinlock non è a zero-stato: va inizializzato esplicitamente prima di usarlo
    portMUX_INITIALIZE(&touch_handle->data.lock);

    // Assegnazione dei parametri di configurazione e IO
    touch_handle->io = io_handle;
    touch_handle->read_data = st1633i_read_data;
    touch_handle->get_xy = st1633i_get_xy;
    touch_handle->del = st1633i_del;
    touch_handle->set_swap_xy = st1633i_set_swap_xy;
    touch_handle->get_swap_xy = st1633i_get_swap_xy;
    touch_handle->set_mirror_x = st1633i_set_mirror_x;
    touch_handle->get_mirror_x = st1633i_get_mirror_x;
    touch_handle->set_mirror_y = st1633i_set_mirror_y;
    touch_handle->get_mirror_y = st1633i_get_mirror_y;

    // Copia i parametri passati dall'applicazione (compresi i pin di IRQ e RESET)
    memcpy(&touch_handle->config, config, sizeof(esp_lcd_touch_config_t));

    // Gestione del pin di RESET se presente
    if (touch_handle->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << touch_handle->config.rst_gpio_num,
        };
        ret = gpio_config(&rst_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO config failed for RESET pin");
    }

    // Gestione del pin di alimentazione opzionale (vedi st1633i_recovery_config_t): solo
    // la configurazione dei pin qui, il power-cycle vero e proprio lo fa
    // st1633i_power_cycle() piu' avanti (stessa funzione riusata dal watchdog).
    st1633i_recovery_config_t *power_cfg = (st1633i_recovery_config_t *)touch_handle->config.driver_data;
    if (power_cfg && power_cfg->power_gpio_num != GPIO_NUM_NC) {
        gpio_config_t power_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << power_cfg->power_gpio_num,
        };
        ret = gpio_config(&power_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO config failed for POWER pin");
    }

    // Gestione del pin di INT/IRQ se presente. Il tipo di interrupt va impostato qui
    // affinché esp_lcd_touch_register_interrupt_callback() (nel componente base) possa
    // limitarsi ad abilitarlo con gpio_intr_enable().
    if (touch_handle->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, // Tipicamente Open-Drain attivo basso
            .intr_type = (touch_handle->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = 1ULL << touch_handle->config.int_gpio_num,
        };
        ret = gpio_config(&int_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO config failed for INT pin");
    }

    // Power-cycle + reset iniziale (se i pin sono configurati): garantisce un avvio
    // pulito del chip indipendentemente dallo stato in cui si trovava prima (es. dopo
    // un riavvio software senza perdita di alimentazione).
    ret = st1633i_power_cycle(touch_handle);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "ST1633i power-cycle/reset failed");

    // Disabilita l'Idle Mode: senza questo, il chip entra in Idle dopo alcuni secondi
    // di inattivita' e il Device Status nello Status Register (0x01) smette di
    // riportare "Normal". Viene riforzato anche periodicamente in st1633i_read_data().
    uint8_t idle_disable = 0xFF;
    esp_err_t idle_ret = esp_lcd_panel_io_tx_param(io_handle, ST1633I_REG_IDLE_TIMEOUT, &idle_disable, sizeof(idle_disable));
    if (idle_ret != ESP_OK) {
        ESP_LOGW(TAG, "Disabilitazione Idle Mode fallita (err=%s)", esp_err_to_name(idle_ret));
    }

    uint8_t chip_id = 0;
    ret = esp_lcd_panel_io_rx_param(io_handle, 0x00, &chip_id, sizeof(chip_id));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Chip ID register 0x00 = 0x%02X", chip_id);
    } else {
        ESP_LOGW(TAG, "ST1633i non risponde su I2C (err=%s): controlla alimentazione/reset/indirizzo del chip", esp_err_to_name(ret));
    }

    // L'handle viene esposto al chiamante solo a inizializzazione completata,
    // altrimenti in caso di errore successivo si lascerebbe un puntatore pendente.
    *out_touch_handle = touch_handle;
    return ESP_OK;

err:
    if (touch_handle) {
        free(touch_handle);
    }
    return ret;
}

static esp_err_t st1633i_read_data(esp_lcd_touch_handle_t tp)
{
    // Byte0: Valid0(bit7) | X0_H(bit6-4) | reserved(bit3) | Y0_H(bit2-0)
    // Byte1: X0_L, Byte2: Y0_L, Byte3: reserved/strength
    uint8_t buf[4];

    if (tp->io == NULL) return ESP_ERR_INVALID_ARG;

    // 1. Leggi il blocco coordinate (FUORI dalla sezione critica)
    static bool i2c_error_logged = false;
    // Watchdog di recovery: dopo ripetuti errori I2C consecutivi tenta un reset hardware;
    // se il solo reset non basta dopo piu' tentativi, escala a un power-cycle completo
    // (spegni/riaccendi l'alimentazione, poi reset) - stessa logica in due passi che il
    // vecchio driver standalone st1633i esponeva come funzioni separate.
    static int consecutive_i2c_failures = 0;
    static int reset_attempts_since_recovery = 0;

    // Soglie configurabili tramite driver_data (0 = usa il default del componente)
    st1633i_recovery_config_t *recovery_cfg = (st1633i_recovery_config_t *)tp->config.driver_data;
    uint16_t fail_reset_threshold = (recovery_cfg && recovery_cfg->i2c_fail_reset_threshold)
                                         ? recovery_cfg->i2c_fail_reset_threshold
                                         : ST1633I_I2C_FAIL_RESET_THRESHOLD;
    uint16_t reset_escalate_threshold = (recovery_cfg && recovery_cfg->reset_escalate_threshold)
                                             ? recovery_cfg->reset_escalate_threshold
                                             : ST1633I_RESET_ESCALATE_THRESHOLD;

    esp_err_t ret = esp_lcd_panel_io_rx_param(tp->io, ST1633I_REG_XY0, buf, sizeof(buf));
    if (ret != ESP_OK) {
        consecutive_i2c_failures++;
        if (!i2c_error_logged) {
            ESP_LOGW(TAG, "Lettura coordinate ST1633i fallita (err=%s)", esp_err_to_name(ret));
            i2c_error_logged = true;
        }
        if (consecutive_i2c_failures >= fail_reset_threshold) {
            if (reset_attempts_since_recovery >= reset_escalate_threshold) {
                ESP_LOGW(TAG, "Reset normale non ha risolto dopo %d tentativi: eseguo power-cycle completo",
                         reset_attempts_since_recovery);
                st1633i_power_cycle(tp);
                reset_attempts_since_recovery = 0;
            } else {
                reset_attempts_since_recovery++;
                ESP_LOGW(TAG, "%d errori I2C consecutivi: eseguo reset hardware (tentativo %d/%d)",
                         consecutive_i2c_failures, reset_attempts_since_recovery, reset_escalate_threshold);
                st1633i_reset(tp);
            }
            uint8_t idle_disable = 0xFF;
            esp_lcd_panel_io_tx_param(tp->io, ST1633I_REG_IDLE_TIMEOUT, &idle_disable, sizeof(idle_disable));
            consecutive_i2c_failures = 0;
        }
        return ret;
    }
    consecutive_i2c_failures = 0;
    reset_attempts_since_recovery = 0;
    if (i2c_error_logged) {
        ESP_LOGI(TAG, "ST1633i torna a rispondere su I2C");
        i2c_error_logged = false;
    }

    // Watchdog anti-Idle: su questo esemplare la sola disabilitazione dell'Idle Mode
    // fatta in fase di init non e' sufficiente a tenere il chip fuori da quello stato
    // nel tempo. Verifichiamo periodicamente il Device Status e, se non e' "Normal",
    // riforziamo la disabilitazione dell'Idle Timeout.
    static int watchdog_tick = 0;
    if ((watchdog_tick++ % ST1633I_IDLE_WATCHDOG_PERIOD) == 0) {
        uint8_t status_reg = 0;
        if (esp_lcd_panel_io_rx_param(tp->io, ST1633I_REG_STATUS, &status_reg, sizeof(status_reg)) == ESP_OK) {
            if ((status_reg & 0x0F) != 0x00) {
                ESP_LOGW(TAG, "ST1633i non in Normal (Device Status=0x%X): riabilito uscita da Idle", status_reg & 0x0F);
                uint8_t idle_disable = 0xFF;
                esp_lcd_panel_io_tx_param(tp->io, ST1633I_REG_IDLE_TIMEOUT, &idle_disable, sizeof(idle_disable));
            }
        }
    }

    bool valid = (buf[0] & 0x80) != 0;
    uint16_t x = 0, y = 0;
    uint8_t final_points = 0;

    // 2. Se il punto e' valido, ricava le coordinate dal blocco appena letto
    if (valid) {
        x = ((uint16_t)((buf[0] >> 4) & 0x07) << 8) | buf[1];
        y = ((uint16_t)(buf[0] & 0x07) << 8) | buf[2];

        // Inversione/swap assi tramite i flag standard esp_lcd_touch (configurabili a runtime)
        if (tp->config.flags.mirror_x) {
            x = tp->config.x_max - x;
        }
        if (tp->config.flags.mirror_y) {
            y = tp->config.y_max - y;
        }
        if (tp->config.flags.swap_xy) {
            uint16_t tmp = x;
            x = y;
            y = tmp;
        }

        final_points = 1;
    }

    // 3. Ora entra nella sezione critica SOLO per copiare i dati calcolati
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.coords[0].x = x;
    tp->data.coords[0].y = y;
    tp->data.points = final_points;
    portEXIT_CRITICAL(&tp->data.lock);

    // process_coordinates non va chiamata qui: la invoca gia' esp_lcd_touch_get_data() sui punti letti via get_xy
    return ESP_OK;
}

// Cambiato il tipo di ritorno in bool per rispettare la firma richiesta da esp_lcd_touch
static bool st1633i_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t saved_points = tp->data.points;
    bool valid = (saved_points > 0 && max_point_num > 0);

    if (valid) {
        *x = tp->data.coords[0].x;
        *y = tp->data.coords[0].y;
        if (strength) *strength = 0;
        *point_num = 1;
    } else {
        *point_num = 0;
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return valid;
}

static esp_err_t st1633i_set_swap_xy(esp_lcd_touch_handle_t tp, bool swap)
{
    tp->config.flags.swap_xy = swap;
    return ESP_OK;
}

static esp_err_t st1633i_get_swap_xy(esp_lcd_touch_handle_t tp, bool *swap)
{
    *swap = tp->config.flags.swap_xy;
    return ESP_OK;
}

static esp_err_t st1633i_set_mirror_x(esp_lcd_touch_handle_t tp, bool mirror)
{
    tp->config.flags.mirror_x = mirror;
    return ESP_OK;
}

static esp_err_t st1633i_get_mirror_x(esp_lcd_touch_handle_t tp, bool *mirror)
{
    *mirror = tp->config.flags.mirror_x;
    return ESP_OK;
}

static esp_err_t st1633i_set_mirror_y(esp_lcd_touch_handle_t tp, bool mirror)
{
    tp->config.flags.mirror_y = mirror;
    return ESP_OK;
}

static esp_err_t st1633i_get_mirror_y(esp_lcd_touch_handle_t tp, bool *mirror)
{
    *mirror = tp->config.flags.mirror_y;
    return ESP_OK;
}

static esp_err_t st1633i_reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        int reset_level = tp->config.levels.reset ? 1 : 0;
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, reset_level), TAG, "Reset pin assert failed");
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !reset_level), TAG, "Reset pin release failed");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

static esp_err_t st1633i_power_cycle(esp_lcd_touch_handle_t tp)
{
    st1633i_recovery_config_t *pwr = (st1633i_recovery_config_t *)tp->config.driver_data;
    if (pwr && pwr->power_gpio_num != GPIO_NUM_NC) {
        int off_level = pwr->power_off_level ? 1 : 0;
        gpio_set_level(pwr->power_gpio_num, off_level);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(pwr->power_gpio_num, !off_level);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return st1633i_reset(tp);
}

static esp_err_t st1633i_del(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    free(tp);
    return ESP_OK;
}
