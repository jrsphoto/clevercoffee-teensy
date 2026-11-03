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

        // --- I2S pins ----------------------------------------------------
        //
        // MCLK, BCLK and LRCK are as the LCDwiki page lists them. THE TWO DATA
        // PINS ARE NOT WHAT THE PAGE APPEARS TO SAY, and this cost an evening:
        // it calls IO6 the "bit output data signal" and IO8 the "bit input data
        // signal" without saying whose output and whose input. Read from the
        // ESP32's side - the obvious reading for a board pinout - playback goes
        // out on IO6. That produces exactly nothing: silence, with a correctly
        // configured codec and correct clocks, which is the worst kind of
        // failure because every diagnostic looks healthy.
        //
        // The page is naming the pins from the CODEC's side, the way codec
        // datasheets label DSDIN and ASDOUT. So playback goes out on IO8 (the
        // codec's input) and IO6 is the microphone coming back. Confirmed by
        // ear: a 1 kHz tone on IO6 is silent, the same tone on IO8 plays.
        constexpr int kPinMclk = 4;
        constexpr int kPinBclk = 5;
        constexpr int kPinLrck = 7;
        constexpr int kPinDataToCodec = 8;   // codec DSDIN - playback
        constexpr int kPinDataFromCodec = 6; // codec ASDOUT - microphone, unused

        int dataOutPin = kPinDataToCodec;

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
            Tone,     // continuous test tone, runs until stopped
        };

        Phase phase = Phase::Idle;

        bool ready = false;         // codec found and I2S installed
        size_t playCursor = 0;      // samples of the sound already handed over
        size_t silenceCursor = 0;   // samples of silence pushed in this phase
        /**
         * Default volume, in percent of the 0-100 scale below. 100 is 0 dB at
         * the codec, i.e. no digital attenuation at all.
         *
         * Set from a listening test, and it needs the full 0 dB because of what
         * is actually in the file: the chime peaks at -3 dBFS but its RMS is
         * only -16.9 dBFS, and the body of it sits near -13 dBFS. Peak
         * normalisation flatters a sound like this - a short transient reaches
         * full scale while everything you actually hear is 10 dB below it.
         *
         * Any attenuation here comes straight off something already quiet, and
         * at -22 dB it was inaudible across a room. If more volume is ever
         * wanted, it has to come from compressing the file, not from this.
         */
        uint8_t volume = 100;       // percent
        uint32_t playStartedMs = 0;
        uint32_t lastPlayMs = 0;    // how long the last sound actually took

        /**
         * Interleaving scratch buffer: one stereo frame is two 16-bit samples.
         *
         * The source material is mono but it is sent as stereo with the same
         * sample in both slots. That is not waste, it is insurance: which slot
         * a mono codec listens to is a configuration detail, and duplicating
         * the sample means the DAC gets audio whichever one it picks. The flash
         * cost is nil because the stored file is still mono - only this
         * in-memory buffer is stereo.
         */
        int16_t stereoBuffer[kChunkSamples * 2] = {};

        /** Phase accumulator for the test tone, in samples. */
        uint32_t tonePhase = 0;
        uint32_t toneHz = 1000;

        size_t totalSamples() {
            return static_cast<size_t>(startupPcmEnd - startupPcmStart) / sizeof(int16_t);
        }

        /**
         * @brief Hand up to `frames` mono samples to the I2S DMA as stereo.
         *
         * Never waits. Returns how many mono samples were actually accepted,
         * which is zero when the DMA is full - the normal case for most loop
         * passes, and not a problem.
         *
         * @param mono  source samples, or nullptr for silence
         * @param frames how many to send, capped at kChunkSamples
         */
        size_t pushFrames(const int16_t* mono, size_t frames) {
            if (frames > kChunkSamples) {
                frames = kChunkSamples;
            }

            for (size_t i = 0; i < frames; ++i) {
                const int16_t sample = mono ? mono[i] : 0;

                stereoBuffer[i * 2] = sample;     // left
                stereoBuffer[i * 2 + 1] = sample; // right
            }

            size_t written = 0;

            const esp_err_t err = i2s_write(kI2sPort, stereoBuffer, frames * 2 * sizeof(int16_t), &written, 0);

            if (err != ESP_OK) {
                return 0;
            }

            // Back from bytes to mono samples: two 16-bit slots per frame.
            return written / (2 * sizeof(int16_t));
        }

        /**
         * @brief Fill a chunk with a test tone and hand it to the DMA.
         *
         * A diagnostic, not a feature. A steady full-scale tone separates "the
         * audio path does not work" from "the sound file is wrong", which a
         * three second chime cannot do.
         */
        size_t pushTone(size_t frames) {
            if (frames > kChunkSamples) {
                frames = kChunkSamples;
            }

            static int16_t toneChunk[kChunkSamples];

            for (size_t i = 0; i < frames; ++i) {
                const float t = static_cast<float>(tonePhase + i) / static_cast<float>(kSampleRate);

                // -13 dBFS, which is the startup sound's RMS level rather
                // than its peak. Matching peaks would make the tone far louder
                // than the chime, because a continuous sine sits 10 dB closer
                // to its own peak than music does - the first version of this
                // was genuinely painful at full volume.
                toneChunk[i] = static_cast<int16_t>(7300.0f * sinf(2.0f * PI * static_cast<float>(toneHz) * t));
            }

            const size_t pushed = pushFrames(toneChunk, frames);

            tonePhase += pushed;

            return pushed;
        }

        /**
         * @brief Install the I2S driver as master, 16-bit stereo at 44.1 kHz.
         *
         * Stereo rather than I2S_CHANNEL_FMT_ONLY_LEFT, with the mono source
         * duplicated into both slots. Two reasons, both learned the hard way:
         * a mono codec's choice of slot is a configuration detail that is easy
         * to get wrong, and the single-slot modes also change the bit clock to
         * frame clock ratio, which has to agree with the divider programmed
         * into the codec. Plain stereo is the textbook case - 32 bit clocks per
         * frame, frame clock equal to the sample rate - and it is what the
         * codec's clock registers here were worked out for.
         *
         * @return true if the driver installed and the pins were assigned.
         */
        bool installI2s() {
            i2s_config_t cfg = {};

            cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
            cfg.sample_rate = kSampleRate;
            cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
            cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
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
            pins.data_out_num = dataOutPin;

            // Whichever data pin is not being used for playback is the
            // microphone's; left alone rather than claimed.
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
         * Total span of the volume control, in decibels below 0 dB.
         *
         * NOT the codec's full range, deliberately. Register 0x32 is half a
         * decibel per step, so mapping percent one-for-one gave -50 dB at 1%
         * and -30 dB at 40% - which on this speaker is silence. The whole
         * usable range was squeezed into the top fifth of the control, and
         * turning it down a few steps made the machine seem broken rather than
         * quiet.
         *
         * 24 dB spreads the useful part across the whole travel: 100% is 0 dB,
         * 50% is about -12 dB and still clearly audible, and 0% mutes.
         */
        constexpr float kVolumeSpanDb = 24.0f;

        /** @brief Map 0-100 onto the codec's DAC volume register. */
        void applyVolume() {
            if (volume == 0) {
                es8311::setMute(true);

                return;
            }

            es8311::setMute(false);

            // Register steps are 0.5 dB, hence the doubling.
            const float belowFullDb = kVolumeSpanDb * (1.0f - (static_cast<float>(volume) / 100.0f));
            const int steps = static_cast<int>((belowFullDb * 2.0f) + 0.5f);

            es8311::setVolume(static_cast<uint8_t>(es8311::kVolume0dB - steps));
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

                silenceCursor += pushFrames(nullptr, want);

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
                playCursor += pushFrames(samples + playCursor, want);

                if (playCursor >= total) {
                    phase = Phase::Draining;
                }

                break;
            }

            case Phase::Tone: {
                // Runs until stop(). Nothing times it out on purpose: a
                // diagnostic that switches itself off is a diagnostic you have
                // to keep restarting while you probe.
                pushTone(kChunkSamples);

                break;
            }

            case Phase::Draining: {
                const size_t remaining = kDrainSamples - silenceCursor;
                const size_t want = remaining < kChunkSamples ? remaining : kChunkSamples;

                silenceCursor += pushFrames(nullptr, want);

                if (silenceCursor >= kDrainSamples) {
                    // The sound has physically finished. Shut the amplifier
                    // down so it does not sit there hissing.
                    es8311::ampEnable(false);

                    phase = Phase::Idle;
                    lastPlayMs = millis() - playStartedMs;

                    // Worth printing: this is the one number that proves the
                    // whole chain is clocking correctly. If it comes back far
                    // short of the sound's real length, the DMA drained faster
                    // than real time and the sample rate is wrong somewhere.
                    Serial.print("audio: startup sound finished in ");
                    Serial.print(lastPlayMs);
                    Serial.println(" ms");
                }

                break;
            }

            case Phase::Idle:
                break;
        }
    }

    bool startTone(uint32_t hz) {
        if (!ready) {
            return false;
        }

        stop();

        toneHz = hz == 0 ? 1000 : hz;
        tonePhase = 0;
        phase = Phase::Tone;

        // Straight on, no prime: this is a diagnostic, and a click at the
        // start of a test tone tells us something useful anyway.
        es8311::ampEnable(true);

        return true;
    }

    void forceAmp(bool on) {
        es8311::ampEnable(on);
    }

    bool setDataPin(int pin) {
        if (pin != kPinDataToCodec && pin != kPinDataFromCodec) {
            Serial.printf("audio: data pin must be %d or %d\n", kPinDataToCodec, kPinDataFromCodec);

            return false;
        }

        stop();

        if (ready) {
            i2s_driver_uninstall(kI2sPort);
            ready = false;
        }

        dataOutPin = pin;

        if (!installI2s()) {
            return false;
        }

        ready = true;

        Serial.printf("audio: I2S playback data now on IO%d\n", dataOutPin);

        return true;
    }

    int dataPin() {
        return dataOutPin;
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
            case Phase::Tone:
                phaseName = "test tone";
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
        Serial.print(" Hz mono, data out on IO");
        Serial.println(dataOutPin);

        if (lastPlayMs > 0) {
            Serial.print("audio: last playback took ");
            Serial.print(lastPlayMs);
            Serial.print(" ms (");
            Serial.print(static_cast<float>(totalSamples()) / kSampleRate, 2);
            Serial.println("s of audio plus prime and drain)");
        }

        es8311::printStatus();
    }

} // namespace audio
