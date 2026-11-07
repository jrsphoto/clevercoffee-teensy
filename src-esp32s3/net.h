/**
 * @file net.h
 *
 * @brief WiFi and the web server on the display head
 *
 * The head owns everything to do with the network. The Teensy owns the machine
 * and does not know or care whether any of this works - see the link rules in
 * linkProtocol.h. Nothing in this file may ever gate an actuator, because
 * nothing in this file can reach one.
 *
 * WIFI FOLLOWS UPSTREAM, including the bit that matters most
 *
 * Ported from upstream's wiFiSetup() in main.cpp: WiFiManager joins a saved
 * network, and if it cannot, it raises its own access point with a captive
 * portal where the OWNER types their credentials into a page served by this
 * board.
 *
 * That last part is not an implementation detail. It means WiFi credentials
 * never pass through a config file in this repository, a commit, or anybody
 * else's hands - they go from the person standing at the machine straight into
 * the ESP32's flash. Any "simpler" scheme that puts an SSID and password in
 * source or in a settings file is a downgrade, however convenient it looks.
 *
 * Upstream's AP password default is kept (defaults.h, WM_PASS "CleverCoffee"),
 * because an open access point that can configure a machine with a 1500 W
 * heater on it is not a good idea even briefly.
 *
 * THE PORTAL IS USELESS IF NOBODY KNOWS IT IS THERE, which is why the display
 * shows the AP name and address while it is up. Upstream does the same thing
 * with displayLogo(). On a machine with no screen you would be reduced to
 * guessing; here there is a screen, so it gets used.
 *
 * BLOCKING, AND WHY IT IS ACCEPTABLE HERE
 *
 * WiFiManager's autoConnect blocks for up to its connect timeout. On a board
 * that controlled a boiler that would be unacceptable. On this one it is fine:
 * the Teensy is already running the machine independently, and the worst case
 * is that this screen says "connecting" for a few seconds. The display is told
 * what is happening first, so a frozen-looking panel is never a mystery.
 */

#pragma once

#include <stdint.h>

namespace net {

    /**
     * @enum State
     * @brief What the network is doing. DISPLAY AND DIAGNOSTICS ONLY.
     */
    enum class State : uint8_t {
        Idle,       ///< not started
        Connecting, ///< trying a saved network
        Portal,     ///< our own AP is up, waiting for someone to configure it
        Online,     ///< joined a network
        Offline,    ///< gave up; the machine still works perfectly
    };

    /**
     * @brief Mount the filesystem holding the web front end.
     *
     * Separate from begin() because a missing filesystem is worth reporting on
     * its own: it means the second upload was forgotten (BENCH.md), not that
     * anything is broken. Returns false in that case, and the server still
     * starts so the API is reachable even with no pages to serve.
     */
    bool mountFilesystem();

    /**
     * @brief Bring up WiFi, then the web server.
     *
     * Blocks while connecting. See the header for why that is acceptable on
     * this board and would not be on the other one.
     */
    void begin();

    /** @brief Service the network. Call every loop pass. Never blocks. */
    void loop();

    /** @brief Current state. For the screen and the console. */
    State state();

    /** @brief Human-readable state. */
    const char* stateName();

    /**
     * @brief Our address, as text.
     *
     * The station address when online, the access point's when the portal is
     * up, and "-" otherwise. What someone would type into a browser.
     */
    const char* addressText();

    /** @brief The network name we are on, or the AP's name while in portal. */
    const char* networkName();

    /** @brief Is the web server listening? */
    bool serverRunning();

    /** @brief Was the front end filesystem mounted? */
    bool filesystemMounted();

    /** @brief Requests served since boot. */
    uint32_t requestCount();

    /**
     * @brief Forget the stored network and restart into the portal.
     *
     * Upstream exposes this as a /wifireset endpoint. Deliberately a
     * restart rather than an in-place reconfigure: WiFiManager's state machine
     * is far simpler to reason about from a cold start than half torn down.
     */
    void forgetNetworkAndRestart();

    /** @brief Print network state for the console. */
    void printStatus();

} // namespace net
