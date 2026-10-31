/**
 * @file linkProtocol.h
 *
 * @brief Wire format for the Teensy <-> ESP32-S3 link. SHARED BY BOTH BUILDS.
 *
 * One source of truth for the frame format, so the two firmwares cannot drift
 * apart. Both PlatformIO environments pull this in from lib/.
 *
 * DESIGN RULES, in order of importance:
 *
 *  1. THE LINK IS NEVER A SAFETY INPUT. The Teensy owns the machine. It sends
 *     telemetry whether or not anyone is listening, and it completes a shot
 *     normally with this cable unplugged. Nothing in this file may ever be
 *     allowed to gate an actuator. Link state exists to grey out a display, and
 *     for nothing else.
 *
 *  2. Human-readable ASCII. A protocol you can read in a terminal and type by
 *     hand is one you can debug when it misbehaves; a binary one needs a working
 *     decoder before you can see anything at all. The cost is a few bytes at
 *     10 Hz on a private wire, which is free.
 *
 *  3. Framed and checksummed. Noise from a pump and solenoids a few centimetres
 *     away will corrupt bytes eventually. A corrupt frame must be detectably
 *     corrupt, not silently plausible - a mangled setpoint is worse than no
 *     setpoint.
 *
 * FRAME FORMAT (NMEA-like, deliberately - it is familiar and proven):
 *
 *     $<type>:<body>*<cksum>\n
 *
 *   $        start. Anything before it is discarded, so the parser resynchronises
 *            by itself after noise or a half-sent frame.
 *   <type>   one character, see FrameType below.
 *   <body>   comma-separated key=value pairs. No spaces.
 *   *        separator.
 *   <cksum>  two uppercase hex digits: XOR of every byte between $ and *,
 *            exclusive.
 *   \n       terminator.
 *
 * Example telemetry from the Teensy:
 *     $T:bt=93.4,gt=88.1,fl=0.00,vol=0.0,st=idle,flt=none*2F
 *
 * Example command from the head:
 *     $C:brew=start*6B
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Namespace is 'linkproto', not 'link': newlib declares a POSIX link(2) on the
// ESP32, and a namespace of that name collides with it.
namespace linkproto {

    constexpr char kFrameStart = '$';
    constexpr char kChecksumSep = '*';
    constexpr char kFrameEnd = '\n';

    /** Longest frame we will build or accept, excluding the terminator. */
    constexpr size_t kMaxFrame = 192;

    /**
     * Frame types. One character so a frame can be classified before parsing.
     */
    enum class FrameType : char {
        Telemetry = 'T', ///< Teensy -> head. Machine state. Unsolicited, periodic.
        Command = 'C',   ///< head -> Teensy. A request, which the Teensy may refuse.
        Ack = 'A',       ///< Teensy -> head. Result of a command.
        Hello = 'H',     ///< Either way. Identifies the sender and its firmware.
        Heartbeat = 'B', ///< head -> Teensy, periodic. Needs NO reply: it exists
                         ///< only so the Teensy can say whether a head is
                         ///< attached. Replying would double the traffic to
                         ///< report something neither side acts on.
        Unknown = '?',
    };

    /** How often the Teensy emits telemetry. Fast enough for a live readout,
     *  slow enough to leave the bus mostly idle. */
    constexpr uint32_t kTelemetryIntervalMs = 200; // 5 Hz

    /** No frames for this long means the peer is considered absent. Display
     *  only - see design rule 1. */
    constexpr uint32_t kPeerTimeoutMs = 2000;

    /** How often the head announces itself. Comfortably inside kPeerTimeoutMs
     *  so one lost frame does not make the head appear to vanish. */
    constexpr uint32_t kHeartbeatIntervalMs = 500;

    /** Link baud. Both ends MUST agree, which is why it lives here and not in
     *  either firmware's own config. */
    constexpr uint32_t kBaud = 115200;

    /**
     * @brief XOR checksum of the bytes between $ and *.
     */
    inline uint8_t checksum(const char* body, const size_t length) {
        uint8_t sum = 0;

        for (size_t i = 0; i < length; i++) {
            sum ^= static_cast<uint8_t>(body[i]);
        }

        return sum;
    }

    /**
     * @brief Build a complete frame, terminator included.
     *
     * @param out      destination buffer
     * @param outSize  its size
     * @param type     frame type
     * @param body     comma-separated key=value pairs, no leading type
     * @return number of bytes written, or 0 if it would not fit
     */
    inline size_t buildFrame(char* out, const size_t outSize, const FrameType type, const char* body) {
        // "<type>:<body>" is what the checksum covers.
        char payload[kMaxFrame];
        const int payloadLen = snprintf(payload, sizeof(payload), "%c:%s", static_cast<char>(type), body);

        if (payloadLen <= 0 || static_cast<size_t>(payloadLen) >= sizeof(payload)) {
            return 0;
        }

        const uint8_t sum = checksum(payload, static_cast<size_t>(payloadLen));

        const int total = snprintf(out, outSize, "%c%s%c%02X%c", kFrameStart, payload, kChecksumSep, sum, kFrameEnd);

        if (total <= 0 || static_cast<size_t>(total) >= outSize) {
            return 0;
        }

        return static_cast<size_t>(total);
    }

    /**
     * @brief Incremental frame parser. Feed it bytes; it hands back whole,
     *        checksum-verified frames.
     *
     * Allocation-free and interrupt-safe to call from a main loop. Resynchronises
     * on '$' so a burst of noise or a truncated frame costs at most one message
     * rather than desynchronising the stream permanently.
     */
    class Parser {
        public:
            /**
             * @brief Feed one received byte.
             *
             * @return true when a complete, VALID frame is available. The frame
             *         is then readable via type() and body() until the next feed.
             */
            bool feed(const char c) {
                if (c == kFrameStart) {
                    // A start byte always begins a new frame, even mid-frame -
                    // that is what makes the parser self-synchronising.
                    length_ = 0;
                    collecting_ = true;

                    return false;
                }

                if (!collecting_) {
                    return false;
                }

                if (c == kFrameEnd) {
                    collecting_ = false;

                    return validate();
                }

                if (length_ >= (kMaxFrame - 1)) {
                    // Overlong: drop it rather than act on a truncated frame.
                    collecting_ = false;
                    overlong_++;

                    return false;
                }

                raw_[length_++] = c;

                return false;
            }

            /** @brief Type of the last valid frame. */
            [[nodiscard]] FrameType type() const {
                return type_;
            }

            /** @brief Body of the last valid frame (key=value pairs). */
            [[nodiscard]] const char* body() const {
                return body_;
            }

            /** @brief Frames rejected for a bad checksum. A climbing count means
             *         electrical noise, not a software fault. */
            [[nodiscard]] uint32_t badChecksums() const {
                return badChecksum_;
            }

            /** @brief Frames dropped for being too long or malformed. */
            [[nodiscard]] uint32_t malformed() const {
                return overlong_ + malformed_;
            }

            /**
             * @brief Extract one value by key from the current body.
             *
             * @return true if the key was present; value is copied to out.
             */
            [[nodiscard]] bool get(const char* key, char* out, const size_t outSize) const {
                const size_t keyLen = strlen(key);
                const char* p = body_;

                while (*p != '\0') {
                    // Match "key=" at a field boundary.
                    if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
                        const char* valueStart = p + keyLen + 1;
                        const char* comma = strchr(valueStart, ',');
                        const size_t valueLen = (comma != nullptr) ? static_cast<size_t>(comma - valueStart) : strlen(valueStart);

                        if (valueLen >= outSize) {
                            return false;
                        }

                        memcpy(out, valueStart, valueLen);
                        out[valueLen] = '\0';

                        return true;
                    }

                    // Advance to the start of the next field.
                    const char* comma = strchr(p, ',');

                    if (comma == nullptr) {
                        break;
                    }

                    p = comma + 1;
                }

                return false;
            }

        private:
            bool validate() {
                // Shortest possible frame is "T:*00" - type, colon, star, 2 hex.
                if (length_ < 5) {
                    malformed_++;

                    return false;
                }

                raw_[length_] = '\0';

                char* star = strrchr(raw_, kChecksumSep);

                if (star == nullptr || (raw_ + length_ - star) != 3) {
                    malformed_++;

                    return false;
                }

                const size_t payloadLen = static_cast<size_t>(star - raw_);
                const uint8_t expected = checksum(raw_, payloadLen);

                const unsigned int received = static_cast<unsigned int>(strtoul(star + 1, nullptr, 16));

                if (received != expected) {
                    badChecksum_++;

                    return false;
                }

                // Split "<type>:<body>"
                if (raw_[1] != ':') {
                    malformed_++;

                    return false;
                }

                type_ = static_cast<FrameType>(raw_[0]);
                *star = '\0';

                const size_t bodyLen = payloadLen - 2;

                if (bodyLen >= sizeof(body_)) {
                    malformed_++;

                    return false;
                }

                memcpy(body_, raw_ + 2, bodyLen);
                body_[bodyLen] = '\0';

                return true;
            }

            char raw_[kMaxFrame]{};
            char body_[kMaxFrame]{};
            size_t length_ = 0;
            bool collecting_ = false;
            FrameType type_ = FrameType::Unknown;

            uint32_t badChecksum_ = 0;
            uint32_t overlong_ = 0;
            uint32_t malformed_ = 0;
    };

} // namespace linkproto
