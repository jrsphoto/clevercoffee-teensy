/**
 * @file boilerSim.cpp
 *
 * @brief A thermal model of the boiler, for working at a desk. See boilerSim.h.
 */

#ifdef DESK_SIM

    #include "boilerSim.h"

    #include <Arduino.h>

    #include "heaterHandler.h"

namespace boilersim {

    namespace {

        /** Kelvin above ambient. The whole state of the model. */
        double deltaK = 0.0;

        uint32_t lastStepMs = 0;
        double heatingMs = 0.0;

        /** Last seen ISR energised-tick count, to difference against. */
        uint32_t lastOnTicks = 0;

        /**
         * Longest step the integrator will take, milliseconds.
         *
         * The main loop runs far faster than this, but a long blocking
         * operation - a flash write, a bring-up command that prints a lot -
         * would otherwise hand the integrator a huge dt and make the modelled
         * boiler jump. Clamping turns that into a slight under-estimate of
         * heating, which is the harmless direction.
         */
        constexpr uint32_t kMaxStepMs = 250;

    } // namespace

    void init() {
        deltaK = 0.0;
        heatingMs = 0.0;
        lastOnTicks = heater::onTickCount();
        lastStepMs = millis();

        Serial.println();
        Serial.println("=====================================================");
        Serial.println(" DESK SIMULATION BUILD - there is no real boiler here");
        Serial.println();
        Serial.print(" element ");
        Serial.print(kElementWatts, 0);
        Serial.print(" W into ");
        Serial.print(kHeatCapacityJPerK, 0);
        Serial.print(" J/K, losing ");
        Serial.print(kLossWattsPerK, 1);
        Serial.println(" W/K to the room");
        Serial.print(" so about ");
        Serial.print((95.0 - 20.0) * kHeatCapacityJPerK / kElementWatts / 60.0, 1);
        Serial.println(" minutes from cold to 95 C, as the real one takes");
        Serial.println();
        Serial.println(" The modelled rise is ADDED to the real probe reading,");
        Serial.println(" so sensor faults, staleness and over-temperature all");
        Serial.println(" still work on top of it. 'boilersim' to see the state.");
        Serial.println("=====================================================");
        Serial.println();
    }

    void loop() {
        const uint32_t now = millis();
        uint32_t stepMs = now - lastStepMs;

        if (stepMs == 0) {
            return;
        }

        lastStepMs = now;

        if (stepMs > kMaxStepMs) {
            stepMs = kMaxStepMs;
        }

        const double dt = stepMs / 1000.0;

        // ENERGY FROM THE ISR'S OWN TICK COUNT, not from the PID's requested
        // duty and not from sampling the pin.
        //
        // The heater is time-proportioned by a 10 ms interrupt against a
        // 1000 ms window, and heater::onTickCount() is that interrupt's own
        // record of how many ticks it actually drove the output for. Taking
        // the difference since the last pass gives the energy delivered
        // exactly, with no aliasing between the main loop's rate and the
        // tick's - which sampling the pin would suffer from.
        //
        // It also keeps the property that matters: this is what the ISR DID,
        // not what anything asked for. A gate that silently blocks heating,
        // or a permit that expires, shows up here as a boiler that will not
        // warm up, exactly as it would on the real machine.
        //
        // The first attempt read outputs::isOn(Channel::Heater) and always
        // saw "off": the heater does NOT go through the outputs module, it is
        // driven straight at the pin by the ISR. The gates said PERMITTED and
        // 9321 of 10181 ticks were energised while the model sat at ambient.
        const uint32_t onTicks = heater::onTickCount();
        const uint32_t onDelta = onTicks - lastOnTicks;
        lastOnTicks = onTicks;

        const double onSeconds = (onDelta * heater::kTickMs) / 1000.0;
        heatingMs += onDelta * heater::kTickMs;

        // Energy in is power times the time actually energised; loss runs for
        // the whole step whether heating or not.
        const double joulesIn = kElementWatts * onSeconds;
        const double joulesOut = kLossWattsPerK * deltaK * dt;

        deltaK += (joulesIn - joulesOut) / kHeatCapacityJPerK;

        // Cannot go below ambient. A boiler has no way to cool itself past the
        // room it is standing in, and letting the integrator drift negative
        // would show up as a probe reading below ambient, which would look
        // like a sensor fault rather than a modelling artefact.
        if (deltaK < 0.0) {
            deltaK = 0.0;
        }
    }

    double deltaC() {
        return deltaK;
    }

    void jumpTo(const double reportedC, const double ambientC) {
        deltaK = reportedC - ambientC;

        if (deltaK < 0.0) {
            deltaK = 0.0;
        }

        Serial.print("boilersim: jumped to ");
        Serial.print(reportedC, 1);
        Serial.print(" C (ambient ");
        Serial.print(ambientC, 1);
        Serial.print(" + ");
        Serial.print(deltaK, 1);
        Serial.println(" modelled)");
    }

    void reset() {
        deltaK = 0.0;
        heatingMs = 0.0;
        lastOnTicks = heater::onTickCount();
        Serial.println("boilersim: back to ambient");
    }

    double heatingSeconds() {
        return heatingMs / 1000.0;
    }

    void printStatus() {
        Serial.print("boilersim: +");
        Serial.print(deltaK, 2);
        Serial.print(" C above ambient, heater has been on ");
        Serial.print(heatingSeconds(), 1);
        Serial.print(" s, output ");
        Serial.print(heater::onTickCount());
        Serial.print(" of ");
        Serial.print(heater::tickCount());
        Serial.println(" ticks energised");

        Serial.print("           equilibrium if left on: +");
        Serial.print(kElementWatts / kLossWattsPerK, 0);
        Serial.println(" C above ambient (the PID should cut in long before)");
        Serial.println("usage: boilersim <C>   jump so the boiler reads this");
        Serial.println("       boilersim off   back to ambient");
    }

} // namespace boilersim

#endif // DESK_SIM
