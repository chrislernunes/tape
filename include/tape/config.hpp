#pragma once

// Load SimulationConfig from a flat key=value file.
// Unknown keys are ignored. Lines starting with # are comments.
// Missing file → false, cfg unchanged.

#include "tape/simulation.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <cctype>

namespace tape {

inline void config_apply_kv(SimulationConfig& cfg, const std::string& key, const std::string& val) {
    auto as_int = [&] { return std::stoi(val); };
    auto as_dbl = [&] { return std::stod(val); };
    auto as_i64 = [&] { return static_cast<int64_t>(std::stoll(val)); };

    if (key == "duration_seconds")          cfg.duration_seconds = as_dbl();
    else if (key == "num_market_makers")    cfg.num_market_makers = as_int();
    else if (key == "num_noise_traders")    cfg.num_noise_traders = as_int();
    else if (key == "num_momentum_agents")  cfg.num_momentum_agents = as_int();
    else if (key == "enable_latency_model") cfg.enable_latency_model = (val == "1" || val == "true");
    else if (key == "verbose")              cfg.verbose = (val == "1" || val == "true");
    else if (key == "instrument_id")        cfg.instrument_id = static_cast<InstrumentId>(as_int());
    else if (key == "mm.half_spread")       cfg.mm_params.half_spread = as_i64();
    else if (key == "mm.skew_per_lot")      cfg.mm_params.skew_per_lot = as_i64();
    else if (key == "mm.quote_qty")         cfg.mm_params.quote_qty = static_cast<Quantity>(std::stoull(val));
    else if (key == "mm.max_inventory")     cfg.mm_params.max_inventory = as_i64();
    else if (key == "mm.stale_threshold")   cfg.mm_params.stale_threshold = as_i64();
    else if (key == "cst.lambda_limit")     cfg.cst_params.lambda_limit = as_dbl();
    else if (key == "cst.mu_market")        cfg.cst_params.mu_market = as_dbl();
    else if (key == "cst.theta_cancel")     cfg.cst_params.theta_cancel = as_dbl();
    else if (key == "cst.alpha_decay")      cfg.cst_params.alpha_decay = as_dbl();
    else if (key == "cst.num_levels")       cfg.cst_params.num_levels = as_int();
    else if (key == "cst.initial_mid")      cfg.cst_params.initial_mid = as_i64();
    else if (key == "cst.initial_spread")   cfg.cst_params.initial_spread = as_i64();
    else if (key == "cst.initial_qty_per_level")
        cfg.cst_params.initial_qty_per_level = static_cast<Quantity>(std::stoull(val));
    else if (key == "cst.sim_seconds")      cfg.cst_params.sim_seconds = as_dbl();
    else if (key == "cst.seed")             cfg.cst_params.seed = static_cast<uint64_t>(std::stoull(val));
}

inline bool load_simulation_config(const std::string& path, SimulationConfig& cfg) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        };
        trim(key); trim(val);
        if (key.empty() || val.empty()) continue;
        config_apply_kv(cfg, key, val);
    }
    cfg.cst_params.sim_seconds = cfg.duration_seconds;
    return true;
}

} // namespace tape
