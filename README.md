# esp_lcd_touch_st1633i

Driver ESP-IDF per il controller touch capacitivo I2C **Sitronix ST1633i**
(Touch IC Protocol A), implementato sopra l'interfaccia standard
`esp_lcd_touch` di Espressif. Compatibile "out of the box" con LVGL tramite
`lv_indev`.

## Cosa fa

- Legge il blocco coordinate del primo dito (single-touch) dal registro
  `0x12` (XY0 High/Low) e lo converte nel formato `esp_lcd_touch`.
- Supporta a runtime `swap_xy`, `mirror_x`, `mirror_y` tramite i flag
  standard di `esp_lcd_touch_config_t` (non serve Kconfig per questo).
- Gestisce il reset hardware tramite il pin `rst_gpio_num`, se configurato.
- Disabilita l'Idle Mode del chip all'inizializzazione e la riforza
  periodicamente (ogni `ST1633I_IDLE_WATCHDOG_PERIOD` letture) controllando
  il registro di stato `0x01`: su alcuni esemplari la sola disabilitazione
  iniziale non è sufficiente a impedire l'ingresso in Idle nel tempo.
- Logga un warning (una sola volta, non ad ogni polling) se il chip smette
  di rispondere su I2C, e un'informazione quando torna a rispondere.

## Dipendenze

```yaml
dependencies:
  idf: ">=6.0.1"
  esp_lcd_touch: "^1.0"
```

Nel `CMakeLists.txt` dell'app va aggiunto `esp_lcd_touch_st1633i` (e
`esp_lcd_touch`) a `PRIV_REQUIRES`/`REQUIRES`.

## Configurazione (Kconfig)

- `CONFIG_ESP_LCD_TOUCH_ST1633I_I2C_ADDRESS` — indirizzo I2C del chip
  (default `0x55`).

## Come integrarlo

1. Copiare la cartella del componente in `components/` (oppure vedi sotto
   per la dipendenza git automatica).
2. Creare l'IO handle I2C verso il touch, riusando la macro di configurazione
   fornita dal componente:

   ```c
   #include "esp_lcd_touch_st1633i.h"

   esp_lcd_panel_io_handle_t tp_io_handle = NULL;
   esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_ST1633I_CONFIG();
   tp_io_config.scl_speed_hz = 400000; // 400 kHz

   ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus_handle, &tp_io_config, &tp_io_handle));
   ```

3. Creare l'handle touch:

   ```c
   esp_lcd_touch_config_t tp_cfg = {
       .x_max = EXAMPLE_LCD_H_RES,
       .y_max = EXAMPLE_LCD_V_RES,
       .rst_gpio_num = GPIO_NUM_NC, // impostare se il RESET è cablato
       .int_gpio_num = GPIO_NUM_NC, // impostare se si vuole usare l'IRQ invece del polling
       .flags = {
           .swap_xy = 0,
           .mirror_x = 0,
           .mirror_y = 0,
       },
   };

   esp_lcd_touch_handle_t tp = NULL;
   ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_st1633i(tp_io_handle, &tp_cfg, &tp));
   ```

4. Collegarlo a LVGL come `lv_indev` di tipo `LV_INDEV_TYPE_POINTER`,
   leggendo le coordinate nella callback con `esp_lcd_touch_read_data(tp)` +
   `esp_lcd_touch_get_data(tp, ...)` (vedi `esp_lcd_touch.h`), oppure
   chiamare direttamente `esp_lcd_touch_read_data()` / `esp_lcd_touch_get_data()`
   in un task di polling proprio. Con `esp_lcd_touch` < 1.2 usare
   `esp_lcd_touch_get_coordinates()`, deprecata dalla 1.2.

## Note

- Solo single-touch (1 punto). Non implementa gesture/prossimità (registro `0x10`)
  né `get_track_id`: con `esp_lcd_touch` >= 1.2 il campo `track_id` resta a 0.
- La callback `process_coordinates` della config viene invocata da
  `esp_lcd_touch_get_data()`, non dal driver in `esp_lcd_touch_read_data()`.
- Se il chip non risponde su I2C in fase di init, l'inizializzazione non
  fallisce: viene solo loggato un warning (utile per bring-up quando
  alimentazione/reset non sono ancora cablati).
