/*
 * HFT Config Loader — parses config.yaml at runtime
 *
 * Zero external dependencies: hand-rolled parser for the YAML subset
 * used in config.yaml (scalar key:value + one level of list items).
 *
 * Usage:
 *   HFTConfig cfg = load_config("config.yaml");   // load file
 *   cfg = load_config();                          // auto-find from argv[0]
 *
 * Environment variable overrides (HFT_ prefix):
 *   HFT_RISK_MAX_ORDER_VALUE=250000 ./sim_demo
 *   Full list at bottom of this file.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

// ============================================================
// HFTConfig — mirrors every section of config.yaml
// ============================================================

struct HFTConfig {

    struct RiskConfig {
        int32_t max_position_per_symbol = 5000;
        int32_t max_portfolio_exposure  = 50000;
        double  max_daily_loss          = 100000.0;
        int32_t max_orders_per_second   = 1000;
        double  max_order_value         = 500000.0;
        double  max_drawdown_pct        = 5.0;
    } risk;

    struct OmsConfig {
        int32_t max_position    = 10000;
        double  max_order_value = 1000000.0;
    } oms;

    struct StrategyConfig {
        int     window        = 20;
        double  threshold_pct = 0.1;
        int32_t order_size    = 100;
    } strategy;

    struct VenueConfig {
        char    name[16]      = {};
        int64_t latency_ns    = 500;
        double  fee_per_share = 0.003;
    };

    struct RouterConfig {
        char    default_strategy[16] = "BEST_PRICE";
        int32_t split_threshold      = 500;
        std::vector<VenueConfig> venues;
    } router;

    struct SimulatorConfig {
        int num_messages = 10000;
        int seed         = 42;
    } simulator;

    struct LoggingConfig {
        char level[16] = "INFO";
    } logging;

    bool loaded = false;   // true if a file was successfully parsed
    char source[256] = {}; // path of the file that was loaded
};


// ============================================================
// Internal helpers
// ============================================================

namespace cfg_detail {

// Count leading spaces (for indentation level detection)
static int indent(const char* line) {
    int n = 0;
    while (line[n] == ' ') n++;
    return n;
}

// Trim leading/trailing whitespace — returns a new string
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Strip inline comment: "500  # some note" -> "500"
static std::string strip_comment(const std::string& s) {
    size_t h = s.find('#');
    return (h == std::string::npos) ? s : trim(s.substr(0, h));
}

// Parse "key: value" from a trimmed line.
// Returns false if no colon found.
static bool kv(const std::string& line, std::string& key, std::string& val) {
    size_t c = line.find(':');
    if (c == std::string::npos) return false;
    key = trim(line.substr(0, c));
    val = strip_comment(trim(line.substr(c + 1)));
    return !key.empty();
}

static int    to_int(const std::string& s) { return std::atoi(s.c_str()); }
static double to_dbl(const std::string& s) { return std::atof(s.c_str()); }

static void safe_copy(char* dst, size_t n, const std::string& src) {
    std::strncpy(dst, src.c_str(), n - 1);
    dst[n - 1] = '\0';
}

} // namespace cfg_detail


// ============================================================
// load_config: parse config.yaml, apply env overrides
// ============================================================

inline HFTConfig load_config(const char* filepath = "config.yaml") {
    using namespace cfg_detail;
    HFTConfig cfg;

    FILE* f = std::fopen(filepath, "r");
    if (!f) {
        // Try one directory up (if binary is in a subdirectory)
        char alt[512];
        std::snprintf(alt, sizeof(alt), "../%s", filepath);
        f = std::fopen(alt, "r");
        if (!f) return cfg;  // return defaults silently
    }

    safe_copy(cfg.source, sizeof(cfg.source), filepath);

    // Section tags
    enum Section { NONE, RISK, OMS, STRATEGY, ROUTER, SIMULATOR, LOGGING };
    Section sec = NONE;
    bool in_venues = false;

    char line_buf[512];
    while (std::fgets(line_buf, sizeof(line_buf), f)) {
        std::string raw(line_buf);
        int ind = indent(raw.c_str());
        std::string line = trim(raw);

        // Skip empty lines and full-line comments
        if (line.empty() || line[0] == '#') continue;

        // --- indentation 0: section header ---
        if (ind == 0) {
            in_venues = false;
            if (line == "risk:")           { sec = RISK;      continue; }
            if (line == "oms:")            { sec = OMS;       continue; }
            if (line == "strategy:")       { sec = STRATEGY;  continue; }
            if (line == "router:")         { sec = ROUTER;    continue; }
            if (line == "simulator:")      { sec = SIMULATOR; continue; }
            if (line == "logging:")        { sec = LOGGING;   continue; }
            continue;
        }

        // --- indentation 2: key:value under current section ---
        if (ind == 2) {
            std::string k, v;
            if (!kv(line, k, v)) continue;

            if (k == "venues" && sec == ROUTER) { in_venues = true; continue; }
            in_venues = false;  // any non-venues key at ind=2 exits the list

            switch (sec) {
                case RISK:
                    if (k == "max_position_per_symbol") cfg.risk.max_position_per_symbol = to_int(v);
                    else if (k == "max_portfolio_exposure")  cfg.risk.max_portfolio_exposure  = to_int(v);
                    else if (k == "max_daily_loss")          cfg.risk.max_daily_loss          = to_dbl(v);
                    else if (k == "max_orders_per_second")   cfg.risk.max_orders_per_second   = to_int(v);
                    else if (k == "max_order_value")         cfg.risk.max_order_value         = to_dbl(v);
                    else if (k == "max_drawdown_pct")        cfg.risk.max_drawdown_pct        = to_dbl(v);
                    break;
                case OMS:
                    if      (k == "max_position")   cfg.oms.max_position    = to_int(v);
                    else if (k == "max_order_value") cfg.oms.max_order_value = to_dbl(v);
                    break;
                case STRATEGY:
                    if      (k == "window")        cfg.strategy.window        = to_int(v);
                    else if (k == "threshold_pct") cfg.strategy.threshold_pct = to_dbl(v);
                    else if (k == "order_size")    cfg.strategy.order_size    = to_int(v);
                    break;
                case ROUTER:
                    if      (k == "default_strategy") safe_copy(cfg.router.default_strategy, 16, v);
                    else if (k == "split_threshold")  cfg.router.split_threshold = to_int(v);
                    break;
                case SIMULATOR:
                    if      (k == "num_messages") cfg.simulator.num_messages = to_int(v);
                    else if (k == "seed")         cfg.simulator.seed         = to_int(v);
                    break;
                case LOGGING:
                    if (k == "level") safe_copy(cfg.logging.level, 16, v);
                    break;
                default: break;
            }
            continue;
        }

        // --- indentation 4+: venue list items ---
        if (ind >= 4 && in_venues && sec == ROUTER) {
            // Strip leading "- " if present
            std::string item = line;
            bool is_new = (item.size() >= 2 && item[0] == '-' && item[1] == ' ');
            if (is_new) {
                cfg.router.venues.push_back(HFTConfig::VenueConfig{});
                item = trim(item.substr(2));
            }
            if (cfg.router.venues.empty()) cfg.router.venues.push_back(HFTConfig::VenueConfig{});
            HFTConfig::VenueConfig& venue = cfg.router.venues.back();

            std::string k, v;
            if (!kv(item, k, v)) continue;
            if      (k == "name")          safe_copy(venue.name, 16, v);
            else if (k == "latency_ns")    venue.latency_ns    = static_cast<int64_t>(to_int(v));
            else if (k == "fee_per_share") venue.fee_per_share = to_dbl(v);
            continue;
        }
    }
    std::fclose(f);

    // --------------------------------------------------------
    // Environment variable overrides — HFT_ prefix
    // Each env var overrides its corresponding config field.
    // Example: HFT_RISK_MAX_ORDER_VALUE=250000 ./sim_demo
    // --------------------------------------------------------
    auto env_int = [](const char* name, int32_t& field) {
        const char* v = std::getenv(name);
        if (v) field = std::atoi(v);
    };
    auto env_dbl = [](const char* name, double& field) {
        const char* v = std::getenv(name);
        if (v) field = std::atof(v);
    };

    env_int("HFT_RISK_MAX_POSITION_PER_SYMBOL", cfg.risk.max_position_per_symbol);
    env_int("HFT_RISK_MAX_PORTFOLIO_EXPOSURE",  cfg.risk.max_portfolio_exposure);
    env_dbl("HFT_RISK_MAX_DAILY_LOSS",          cfg.risk.max_daily_loss);
    env_int("HFT_RISK_MAX_ORDERS_PER_SECOND",   cfg.risk.max_orders_per_second);
    env_dbl("HFT_RISK_MAX_ORDER_VALUE",         cfg.risk.max_order_value);
    env_dbl("HFT_RISK_MAX_DRAWDOWN_PCT",        cfg.risk.max_drawdown_pct);
    env_int("HFT_OMS_MAX_POSITION",             cfg.oms.max_position);
    env_dbl("HFT_OMS_MAX_ORDER_VALUE",          cfg.oms.max_order_value);
    env_int("HFT_STRATEGY_WINDOW",              cfg.strategy.window);
    env_dbl("HFT_STRATEGY_THRESHOLD_PCT",       cfg.strategy.threshold_pct);
    env_int("HFT_STRATEGY_ORDER_SIZE",          cfg.strategy.order_size);
    env_int("HFT_SIMULATOR_NUM_MESSAGES",       cfg.simulator.num_messages);
    env_int("HFT_SIMULATOR_SEED",               cfg.simulator.seed);

    cfg.loaded = true;
    return cfg;
}


// Print loaded config (for startup diagnostics)
inline void print_config(const HFTConfig& cfg) {
    printf("=== HFT Config (%s) ===\n", cfg.loaded ? cfg.source : "defaults");
    printf("  risk.max_order_value:         %.0f\n", cfg.risk.max_order_value);
    printf("  risk.max_position_per_symbol: %d\n",   cfg.risk.max_position_per_symbol);
    printf("  risk.max_orders_per_second:   %d\n",   cfg.risk.max_orders_per_second);
    printf("  oms.max_position:             %d\n",   cfg.oms.max_position);
    printf("  oms.max_order_value:          %.0f\n", cfg.oms.max_order_value);
    printf("  strategy.window:              %d\n",   cfg.strategy.window);
    printf("  strategy.threshold_pct:       %.3f\n", cfg.strategy.threshold_pct);
    printf("  strategy.order_size:          %d\n",   cfg.strategy.order_size);
    printf("  router.default_strategy:      %s\n",   cfg.router.default_strategy);
    printf("  router.split_threshold:       %d\n",   cfg.router.split_threshold);
    for (size_t i = 0; i < cfg.router.venues.size(); ++i) {
        const auto& v = cfg.router.venues[i];
        printf("  router.venues[%zu]:            %s  lat=%ldns  fee=%.4f\n",
               i, v.name, (long)v.latency_ns, v.fee_per_share);
    }
    printf("  simulator.num_messages:       %d\n",   cfg.simulator.num_messages);
    printf("  simulator.seed:               %d\n",   cfg.simulator.seed);
    printf("  logging.level:                %s\n",   cfg.logging.level);
}
