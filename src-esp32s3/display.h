/**
 * @file display.h
 *
 * @brief LovyanGFX configuration for the LCDwiki / Hosyond 4.0" ESP32-S3 board
 *
 * Panel and touch in one class, which is the reason for choosing LovyanGFX over
 * TFT_eSPI here: it drives the ST7796S and the FT6336U together, so there is one
 * place where the board's wiring is described instead of a display config header
 * plus a separate touch library.
 *
 * Board facts, from the LCDwiki page for this exact module - NOT assumed:
 *   LCD    ST7796S, 320x480 IPS, 4-line SPI
 *          CS IO10, DC IO46, SCK IO12, MOSI IO11, MISO IO13, backlight IO45
 *          RESET is tied to the ESP32-S3's own reset - there is no GPIO for it
 *   TOUCH  FT6336U over I2C: SDA IO16, SCL IO15, INT IO17, RST IO18
 *   MCU    ESP32-S3-N16R8, 16 MB flash + 8 MB OPI PSRAM
 *   UART0  RX IO43, TX IO44 - the header that goes to the Teensy's RX1/TX1
 *
 * Note the touch bus is a SEPARATE I2C bus from anything else; it is the only
 * thing on it. The machine's real I2C devices (encoder, pressure) live on the
 * Teensy, not here.
 */

#pragma once

#include <LovyanGFX.hpp>

namespace display {

    // --- Board pin map. Change nothing here without the LCDwiki page open. ---
    constexpr int kPinLcdCs = 10;
    constexpr int kPinLcdDc = 46;
    constexpr int kPinLcdSck = 12;
    constexpr int kPinLcdMosi = 11;
    constexpr int kPinLcdMiso = 13;
    constexpr int kPinLcdBacklight = 45;

    constexpr int kPinTouchSda = 16;
    constexpr int kPinTouchScl = 15;
    constexpr int kPinTouchInt = 17;
    constexpr int kPinTouchRst = 18;

    constexpr int kPanelWidth = 320;
    constexpr int kPanelHeight = 480;

    /** UART0 pins - the link to the Teensy. */
    constexpr int kPinLinkRx = 43;
    constexpr int kPinLinkTx = 44;

    /**
     * @brief LovyanGFX device description for this board.
     */
    class LGFX final : public lgfx::LGFX_Device {
            lgfx::Panel_ST7796 panel_;
            lgfx::Bus_SPI bus_;
            lgfx::Light_PWM light_;
            lgfx::Touch_FT5x06 touch_; // the FT6336U is in the FT5x06 family

        public:
            LGFX() {
                {
                    auto cfg = bus_.config();

                    cfg.spi_host = SPI2_HOST;
                    cfg.spi_mode = 0;
                    cfg.freq_write = 40000000; // conservative to start; this panel will take more
                    cfg.freq_read = 16000000;
                    cfg.spi_3wire = false;
                    cfg.use_lock = true;
                    cfg.dma_channel = SPI_DMA_CH_AUTO;
                    cfg.pin_sclk = kPinLcdSck;
                    cfg.pin_mosi = kPinLcdMosi;
                    cfg.pin_miso = kPinLcdMiso;
                    cfg.pin_dc = kPinLcdDc;

                    bus_.config(cfg);
                    panel_.setBus(&bus_);
                }

                {
                    auto cfg = panel_.config();

                    cfg.pin_cs = kPinLcdCs;

                    // No reset GPIO: this panel's reset is wired to the
                    // ESP32-S3's own reset line, so the display resets when the
                    // board does and the driver must not try to drive one.
                    cfg.pin_rst = -1;
                    cfg.pin_busy = -1;

                    cfg.panel_width = kPanelWidth;
                    cfg.panel_height = kPanelHeight;
                    cfg.offset_x = 0;
                    cfg.offset_y = 0;
                    cfg.offset_rotation = 0;
                    cfg.dummy_read_pixel = 8;
                    cfg.dummy_read_bits = 1;
                    cfg.readable = true;
                    cfg.invert = true;  // IPS panels on this board need inversion
                    cfg.rgb_order = false;
                    cfg.dlen_16bit = false;
                    cfg.bus_shared = false;

                    panel_.config(cfg);
                }

                {
                    auto cfg = light_.config();

                    cfg.pin_bl = kPinLcdBacklight;
                    cfg.invert = false; // high turns the backlight ON
                    cfg.freq = 44100;
                    cfg.pwm_channel = 7;

                    light_.config(cfg);
                    panel_.setLight(&light_);
                }

                {
                    auto cfg = touch_.config();

                    cfg.x_min = 0;
                    cfg.x_max = kPanelWidth - 1;
                    cfg.y_min = 0;
                    cfg.y_max = kPanelHeight - 1;
                    cfg.pin_int = kPinTouchInt;
                    cfg.pin_rst = kPinTouchRst;
                    cfg.bus_shared = false;
                    cfg.offset_rotation = 0;

                    cfg.i2c_port = 0;
                    cfg.i2c_addr = 0x38; // FT5x06 family default
                    cfg.pin_sda = kPinTouchSda;
                    cfg.pin_scl = kPinTouchScl;
                    cfg.freq = 400000;

                    touch_.config(cfg);
                    panel_.setTouch(&touch_);
                }

                setPanel(&panel_);
            }
    };

    /** The one display instance. */
    extern LGFX lcd;

    /**
     * @brief Bring up the panel and backlight.
     *
     * @return true if the panel initialised.
     */
    bool init();

    /**
     * @brief Draw the bring-up screen: panel info and a touch target.
     */
    void showBringupScreen();

    /**
     * @brief Poll touch and draw feedback. Call every loop pass.
     */
    void loop();

} // namespace display
