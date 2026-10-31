/**
 * @file audio.cpp
 *
 * @brief Startup sound on the display head's speaker
 */

#include "audio.h"

#include <Arduino.h>
#include <driver/i2s.h>

#include "es8311.h"

namespace audio {

    namespace {

        /**
         * The startup sound, linked into the firmware image by
         * board_build.embed_files in platformio.ini.
         *
         * Raw signed 16-bit mono PCM at 44.1 kHz - no header, no decoding. A
         * WAV header would only have to be parsed and skipped, and an MP3 would
         * need a decoder library and several hundred kilobytes of working
         * memory to save flash this board has 16 MB of.
         *
         * The asm() labels are objcopy's naming for an embedded binary: the
         * file path with every separator turned into an underscore. Verified
         * against the actual object file rather than assumed, because the whole
         * scheme falls apart silently if the symbol name is wrong.
         */
        extern "C" {
            extern const uint8_t startupPcmStart[] asm("_binary_assets_startup_pcm_raw_start");
            extern const uint8_t startupPcmEnd[] asm("_binary_assets_startup_pcm_raw_end");
        }

        constexpr i2s_port_t kI2sPort = I2S_NUM_0;
        constexpr uint32_t kSampleRate = 44100;

        // --- I2S pins (LCDwiki page for this module) ---------------------
        constexpr int kPinMclk = 4;
        constexpr int kPinBclk = 5;
        constexpr int kPinDout = 6;
        constexpr int kPinLrck = 7;

        /**
         * DMA depth. 8 buffers of 512 samples is about 93 ms of audio in
         * flight, which is far more headroom than loop() needs and is what lets
         * playback survive a slow screen redraw without a gap in the sound.
         */
        constexpr int kDmaBufCount = 8;
        constexpr int kDmaBufLen = 512;
        constexpr size_t kDmaDepthSamples = static_cast<size_t>(kDmaBufCount) * kDmaBufLen;

        /**
         * Silence clocked out before the amplifier is switched on, to let the
         * codec's DAC output settle at its idle level first. Switching a
         * class-D amplifier on across a moving DAC output is an audible thump.
         */
        constexpr size_t kPrimeSamples = kSampleRate / 50; // 20 ms

        /**
         * Silence clocked out after the sound, before the amplifier goes off.
         *
         * Must exceed the DMA depth: when the last real sample is handed to
         * i2s_write it has not been played yet, it is queued. Pushing at least
         * twice the DMA depth of silence is self-timing - the DMA has to have
         * consumed the first depth's worth to make room for the second, which
         * means the real audio is genuinely out of the speaker by then.
         */
        constexpr size_t kDrainSamples = kDmaDepthSamples * 2;

        /** Chunk size for each loop() pass. One DMA buffer's worth. */
        constexpr size_t kChunkSamples = kDmaBufLen;

        enum class Phase : uint8_t {
            Idle,
            Priming,  // silence, amplifier still down, DAC settling
            Playing,  // the actual sound
            Draining, // silence, waiting for the DMA to empty
        };

        Phase phase = Phase::Idle;

        bool ready = false;         // codec found and I2S installed
        size_t playCursor = 0;      // samples of the sound already handed over
        size_t silenceCursor = 0;   // samples of silence pushed in this phase
        uint8_t volume = 80;        // percent
        uint32_t playStartedMs = 0;
        uint32_t underruns = 0;     // passes where the DMA took nothing

        /** A block of zeroes to push during the prime and drain phases. */
        int16_t silenceBuffer[kChunkSamples] = {};

        size_t totalSamples() {
            return static_cast<size_t>(startupPcmEnd - startupPcmStart) / sizeof(int16_t);
        }

        /**
         * @brief Hand up to `samples` 16-bit samples to the I2S DMA.
         *
         * Never waits. Returns how many samples were actually accepted, which
         * is zero when the DMA is full - the normal case for most loop passes,
         * and not a problem.
         */
        size_t pushSamples(const int16_t* data, size_t samples) {
            size_t written = 0;

            const esp_err_t err = i2s_write(kI2sPort, data, samples * sizeof(int16_t), &written, 0);

            if (err != ESP_OK) {
                return 0;
            }

            return written / sizeof(int16_t);
        }

        /**
         * @brief Install the I2S driver as master, 16-bit mono at 44.1 kHz.
         *
         * Mono is sent as I2S_CHANNEL_FMT_ONLY_LEFT: the codec's DAC is mono
         * and the board has one speaker, so a stereo file would be two copies
         * of the same thing taking twice the flash.
         *
         * @return true if the driver installed and the pins were assigned.
         */
        bool installI2s() {
            i2s_config_t cfg = {};

            cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
            cfg.sample_rate = kSampleRate;
            cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
            cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
            cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
            cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
            cfg.dma_buf_count = kDmaBufCount;
            cfg.dma_buf_len = kDmaBufLen;
            cfg.tx_desc_auto_clear = true; // emit silence on underrun, not the last buffer again
            cfg.fixed_mclk = 0;

            // 256 x the sample rate, which is what the codec's clock registers
            // were configured for. Getting this wrong does not fail - it plays
            // at the wrong speed, or not at all.
            cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
            cfg.bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT;

            // The audio PLL gives an exact 44.1 kHz. The default clock source
            // can only approximate it, which for a 3 second chime is
            // inaudible - so if the PLL is unavailable, carry on without it
            // rather than lose the sound entirely.
            cfg.use_apll = true;

            esp_err_t err = i2s_driver_install(kI2sPort, &cfg, 0, nullptr);

            if (err != ESP_OK) {
                Serial.println("audio: I2S install failed with APLL, retrying without it");

                cfg.use_apll = false;
                err = i2s_driver_install(kI2sPort, &cfg, 0, nullptr);
            }

            if (err != ESP_OK) {
                Serial.print("audio: i2s_driver_install failed: ");
                Serial.println(esp_err_to_name(err));

                return false;
            }

            i2s_pin_config_t pins = {};

            pins.mck_io_num = kPinMclk;
            pins.bck_io_num = kPinBclk;
            pins.ws_io_num = kPinLrck;
            pins.data_out_num = kPinDout;

            // The board's microphone is wired to IO8 but not used here, so the
            // pin is left alone rather than claimed.
            pins.data_in_num = I2S_PIN_NO_CHANGE;

            err = i2s_set_pin(kI2sPort, &pins);

            if (err != ESP_OK) {
                Serial.print("audio: i2s_set_pin failed: ");
                Serial.println(esp_err_to_name(err));

                return false;
            }

            return true;
        }

        /**
         * @brief Map 0-100 onto the codec's DAC volume register.
         *
         * Register 0x32 is half a decibel per step, so stepping it one-for-one
         * with the percentage gives 0 dB at 100 and about -50 dB at 1. Half-dB
         * steps are close enough to how hearing works that this feels like a
         * normal volume control, which a linear amplitude scale would not.
         */
        void applyVolume() {
            if (volume == 0) {
                es8311::setMute(true);

                return;
            }

            es8311::setMute(false);
            es8311::setVolume(static_cast<uint8_t>(es8311::kVolume0dB - (100 - volume)));
        }

    } // namespace

    bool init() {
        ready = false;

        // Amplifier down first, before anything else can make a noise. IO1 is
        // an ordinary pin with no pull-up or pull-down, so until this runs the
        // amplifier's state is whatever the board leaves it at.
        es8311::ampEnable(false);

        if (!es8311::probe()) {
            Serial.println("audio: no ES8311 on the I2C bus - sound disabled");

            return false;
        }

        if (!es8311::begin()) {
            Serial.println("audio: ES8311 found but would not configure - sound disabled");

            return false;
        }

        if (!installI2s()) {
            es8311::suspend();

            return false;
        }

        applyVolume();

        ready = true;

        Serial.print("audio: ready, ");
        Serial.print(totalSamples());
        Serial.print(" samples of startup sound (");
        Serial.print(static_cast<float>(totalSamples()) / kSampleRate, 2);
        Serial.println("s)");

        return true;
    }

    bool playStartupSound() {
        if (!ready || phase != Phase::Idle) {
            return false;
        }

        if (totalSamples() == 0) {
            Serial.println("audio: startup sound is empty - nothing embedded?");

            return false;
        }

        playCursor = 0;
        silenceCursor = 0;
        underruns = 0;
        playStartedMs = millis();
        phase = Phase::Priming;

        return true;
    }

    void loop() {
        if (phase == Phase::Idle) {
            return;
        }

        switch (phase) {
            case Phase::Priming: {
                const size_t remaining = kPrimeSamples - silenceCursor;
                const size_t want = remaining < kChunkSamples ? remaining : kChunkSamples;

                silenceCursor += pushSamples(silenceBuffer, want);

                if (silenceCursor >= kPrimeSamples) {
                    // DAC is settled and the clocks are running. Now it is safe
                    // to let the amplifier through.
                    es8311::ampEnable(true);

                    silenceCursor = 0;
                    phase = Phase::Playing;
                }

                break;
            }

            case Phase::Playing: {
                const size_t total = totalSamples();
                const size_t remaining = total - playCursor;
                const size_t want = remaining < kChunkSamples ? remaining : kChunkSamples;

                const auto* samples = reinterpret_cast<const int16_t*>(startupPcmStart);
                const size_t pushed = pushSamples(samples + playCursor, want);

                if (pushed == 0) {
                    ++underruns;
                }

                playCursor += pushed;

                if (playCursor >= total) {
                    phase = Phase::Draining;
                }

                break;
            }

            case Phase::Draining: {
                const size_t remaining = kDrainSamples - silenceCursor;
                const size_t want = remaining < kChunkSamples ? remaining : kChunkSamples;

                silenceCursor += pushSamples(silenceBuffer, want);

                if (silenceCursor >= kDrainSamples) {
                    // The sound has physically finished. Shut the amplifier
                    // down so it does not sit there hissing.
                    es8311::ampEnable(false);

                    phase = Phase::Idle;

                    Serial.print("audio: startup sound finished in ");
                    Serial.print(millis() - playStartedMs);
                    Serial.println(" ms");
                }

                break;
            }

            case Phase::Idle:
                break;
        }
    }

    bool isPlaying() {
        return phase != Phase::Idle;
    }

    void stop() {
        if (phase == Phase::Idle) {
            return;
        }

        es8311::ampEnable(false);
        i2s_zero_dma_buffer(kI2sPort);

        phase = Phase::Idle;
        playCursor = 0;
        silenceCursor = 0;
    }

    void setVolumePercent(uint8_t percent) {
        volume = percent > 100 ? 100 : percent;

        if (ready) {
            applyVolume();
        }
    }

    uint8_t volumePercent() {
        return volume;
    }

    void printStatus() {
        Serial.print("audio: ");

        if (!ready) {
            Serial.println("not available - no sound");
            es8311::printStatus();

            return;
        }

        const char* phaseName = "idle";

        switch (phase) {
            case Phase::Priming:
                phaseName = "priming";
                break;
            case Phase::Playing:
                phaseName = "playing";
                break;
            case Phase::Draining:
                phaseName = "draining";
                break;
            case Phase::Idle:
                break;
        }

        Serial.print(phaseName);
        Serial.print(", volume ");
        Serial.print(volume);
        Serial.print("%, ");
        Serial.print(totalSamples());
        Serial.print(" samples embedded at ");
        Serial.print(kSampleRate);
        Serial.println(" Hz mono");

        es8311::printStatus();
    }

} // namespace audio
