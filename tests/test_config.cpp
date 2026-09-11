#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include "tape/config.hpp"

using namespace tape;

TEST_CASE("load_simulation_config reads key=value and ignores comments", "[config]") {
    const char* path = "tape_test_sim.cfg";
    {
        std::ofstream out(path);
        out << "# comment\n";
        out << "duration_seconds = 3.5\n";
        out << "num_market_makers = 4\n";
        out << "cst.seed = 99\n";
        out << "mm.quote_qty = 7\n";
    }
    SimulationConfig cfg;
    REQUIRE(load_simulation_config(path, cfg));
    CHECK(cfg.duration_seconds == 3.5);
    CHECK(cfg.num_market_makers == 4);
    CHECK(cfg.cst_params.seed == 99);
    CHECK(cfg.mm_params.quote_qty == 7);
    CHECK(cfg.cst_params.sim_seconds == 3.5);
    REQUIRE_FALSE(load_simulation_config("does-not-exist.cfg", cfg));
}
