/**
 * @file boilerSim.h
 *
 * @brief A thermal model of the boiler, for working at a desk
 *
 * WHY THIS EXISTS
 *
 * The boiler is in the garage with no seals and no plumbing. Every bit of
 * development happens at a desk with the Teensy, the display and a pair of
 * PT100 probes reading room temperature. That makes the PID the largest
 * untested thing in this firmware with no route to testing it: a control loop
 * cannot be exercised when its input never answers its output.
 *
 * So the heater output drives a model instead of an element, and the model's
 * temperature rise is ADDED TO THE REAL PROBE READING.
 *
 * WHY ADDED RATHER THAN SUBSTITUTED, which matters more than it looks
 *
 * Replacing the reading with a modelled number would throw away everything
 * underneath it: the MAX31865's own fault detection, the staleness check that
 * catches a dead sensor, the plausibility window, the real noise. Those are
 * exactly the paths that must keep working, and a flat synthetic value tests
 * none of them.
 *
 * Adding a delta keeps all of it. With the model idle the firmware reads the
 * truth - pull a probe lead and it still faults. With the model running it
 * reads a hot boiler, with real sensor behaviour underneath.
 *
 * The delta is applied BEFORE the plausibility and over-temperature checks, so
 * a simulated boiler that runs away latches a real OverTemperature fault. That
 * is the point: the interlock should be indistinguishable from the real thing.
 *
 * COMPILED ONLY INTO THE teensy41_sim ENVIRONMENT
 *
 * Not a runtime switch, not a config flag. The firmware that eventually goes
 * into the machine does not contain this code at all - there is no mode to
 * leave on by accident, which is the only serious objection to simulating a
 * safety input. See platformio.ini.
 *
 * The desk has no SSRs connected either, so the heater output drives nothing
 * whatever while this runs.
 */

#pragma once

#ifdef DESK_SIM

    #include <stdint.h>

namespace boilersim {

    /**
     * Element power, watts. John's machine is specified with a 1000 W heater
     * (the OEM 2000 W element run on one of its two legs gives ~990 W).
     */
    constexpr double kElementWatts = 1000.0;

    /**
     * Heat capacity of the thing being heated, joules per kelvin.
     *
     * About 1.5 kg of water at 4186 J/kg/K, plus the brass and copper of the
     * boiler shell - call it 3 kg at 380 J/kg/K. 6300 + 1100 = 7400.
     *
     * This sets the heat-up rate: 1000 W into 7400 J/K is 0.135 K/s, so room
     * temperature to 95 C takes about nine minutes. That matches a real
     * machine, and it is deliberately NOT accelerated - see the note on
     * simSpeed() below.
     */
    constexpr double kHeatCapacityJPerK = 7400.0;

    /**
     * Loss to the room, watts per kelvin above ambient.
     *
     * THE BOILER IS TO BE INSULATED with 10 mm melamine foam, so this is not
     * the bare figure.
     *
     * Melamine foam conducts about 0.035 W/m/K, so 10 mm gives 3.5 W/m2/K;
     * in series with the outside air film (~10) that is about 2.6, and on
     * roughly 0.13 m2 of shell about 0.35 W/K. The shell is then NOT the
     * dominant path - what stays uncovered carries the heat: the element and
     * probe bosses, the fittings, and above all the group head, eight pounds
     * of copper bolted to the boiler and hanging in free air. Those bridges
     * plausibly add 0.6 to 1.0 W/K.
     *
     * So 1.2 W/K insulated, against about 2.0 bare - roughly a third less
     * idle power, which is the usual result of insulating an espresso boiler
     * and far less than the foam alone suggests.
     *
     * ESTIMATED, AND MEASURABLE LATER. Holding power at setpoint divided by
     * the temperature difference IS this number, in one measurement on the
     * finished machine. Correct it from that rather than from the arithmetic
     * above.
     */
    constexpr double kLossWattsPerK = 1.2;

    /** @brief Prepare the model. Call once at boot. */
    void init();

    /**
     * @brief Advance the model. Call every main-loop pass.
     *
     * Samples the REAL heater output pin rather than the PID's requested duty,
     * so the model is driven by what the time-proportioning ISR actually did.
     * A bug that leaves the output off would show up here as a boiler that
     * never heats, which is the behaviour you would see on the real machine.
     */
    void loop();

    /**
     * @brief How far above ambient the modelled boiler is, in kelvin.
     *
     * Added to the real probe reading by tempHandler. Zero at boot, so the
     * firmware reads the truth until the heater runs.
     */
    double deltaC();

    /**
     * @brief Jump the model so the reported boiler temperature becomes this.
     *
     * Nine minutes of heat-up is realistic and tedious when the thing being
     * tested is behaviour near the setpoint. This skips the boring part
     * WITHOUT distorting the dynamics, which a time-acceleration factor would:
     * scaling time changes what Tn and Tv mean, so gains tuned against a
     * sped-up plant do not transfer to the real one.
     *
     * @param reportedC what boilerC() should read right now
     * @param ambientC  the real probe reading to measure the jump from
     */
    void jumpTo(double reportedC, double ambientC);

    /** @brief Back to ambient, as though the machine had been off for hours. */
    void reset();

    /** @brief Seconds of modelled heating so far, for the console. */
    double heatingSeconds();

    /** @brief Print the model's state. */
    void printStatus();

} // namespace boilersim

#endif // DESK_SIM
