// Offline 2-second simulation. No network, no Binance, no extra data files.
// Parameters come from examples/sim.cfg if present, else defaults.

#include "tape/simulation.hpp"
#include "tape/config.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    using namespace tape;
    SimulationConfig cfg;
    cfg.duration_seconds    = 2.0;
    cfg.num_market_makers   = 2;
    cfg.num_noise_traders   = 4;
    cfg.num_momentum_agents = 1;
    cfg.verbose             = true;
    cfg.cst_params.seed     = 12345;
    cfg.cst_params.sim_seconds = cfg.duration_seconds;

    const char* path = (argc > 1) ? argv[1] : "examples/sim.cfg";
    if (load_simulation_config(path, cfg))
        std::printf("loaded config %s\n", path);
    else
        std::printf("using built-in defaults (no file at %s)\n", path);

    TapeSimulation sim(cfg);
    sim.run();
    return 0;
}
