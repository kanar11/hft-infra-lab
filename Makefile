.PHONY: all build test benchmark simulate clean

CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -Werror -pthread

BUILDDIR := build

SRCS := \
	orderbook/orderbook.cpp \
	orderbook/orderbook_v2.cpp \
	orderbook/benchmark_orderbook.cpp \
	orderbook/latency_histogram.cpp \
	lockfree/spsc_queue.cpp \
	memory-latency/cache_latency.cpp \
	itch-parser/benchmark_itch.cpp \
	benchmarks/latency_benchmark.cpp \
	benchmarks/orderbook_benchmark.cpp \
	oms/oms_demo.cpp \
	risk/risk_demo.cpp \
	router/router_demo.cpp \
	logger/logger_demo.cpp \
	strategy/strategy_demo.cpp \
	fix-protocol/fix_demo.cpp \
	ouch-protocol/ouch_demo.cpp \
	dpdk-bypass/dpdk_demo.cpp \
	monitoring/monitor_demo.cpp \
	multicast/multicast_demo.cpp \
	simulator/sim_demo.cpp \
	tests/test_all.cpp

BINS := $(patsubst %.cpp,%,$(SRCS))
OBJS := $(addprefix $(BUILDDIR)/,$(patsubst %.cpp,%.o,$(SRCS)))
DEPS := $(OBJS:.o=.d)

all: build

build: $(BINS)

# Link each binary from its single object file
$(BINS): %: $(BUILDDIR)/%.o
	$(CXX) $(CXXFLAGS) -o $@ $<

# Compile .cpp -> .o with automatic header dependency tracking
$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# Run all unit tests (each demo includes built-in tests)
test: build
	./oms/oms_demo 0
	./risk/risk_demo 0
	./router/router_demo 0
	./logger/logger_demo 0
	./strategy/strategy_demo 0
	./fix-protocol/fix_demo 0
	./ouch-protocol/ouch_demo 0
	./dpdk-bypass/dpdk_demo 0
	./monitoring/monitor_demo 0
	./multicast/multicast_demo 0
	./simulator/sim_demo 0
	./tests/test_all

# Run throughput benchmarks for all modules
benchmark: build
	@echo "=== Orderbook ==="
	./orderbook/benchmark_orderbook
	./orderbook/latency_histogram
	@echo ""
	@echo "=== Lock-Free Queue ==="
	./lockfree/spsc_queue
	@echo ""
	@echo "=== Cache Latency ==="
	./memory-latency/cache_latency
	@echo ""
	@echo "=== ITCH Parser ==="
	./itch-parser/benchmark_itch
	@echo ""
	@echo "=== Latency Benchmark ==="
	./benchmarks/latency_benchmark 100000
	@echo ""
	@echo "=== Orderbook Benchmark ==="
	./benchmarks/orderbook_benchmark 100000
	@echo ""
	@echo "=== OMS ==="
	./oms/oms_demo 500000
	@echo ""
	@echo "=== Risk Manager ==="
	./risk/risk_demo 500000
	@echo ""
	@echo "=== Smart Router ==="
	./router/router_demo 500000
	@echo ""
	@echo "=== Trade Logger ==="
	./logger/logger_demo 500000
	@echo ""
	@echo "=== Mean Reversion Strategy ==="
	./strategy/strategy_demo 500000
	@echo ""
	@echo "=== OUCH Protocol ==="
	./ouch-protocol/ouch_demo 500000
	@echo ""
	@echo "=== FIX Protocol ==="
	./fix-protocol/fix_demo 500000
	@echo ""
	@echo "=== DPDK Bypass ==="
	./dpdk-bypass/dpdk_demo 100000
	@echo ""
	@echo "=== Monitoring ==="
	./monitoring/monitor_demo 200000
	@echo ""
	@echo "=== Multicast ==="
	./multicast/multicast_demo 200000
	@echo ""
	@echo "=== Market Simulator (E2E) ==="
	./simulator/sim_demo 50000

# Run end-to-end market simulation
simulate: build
	./simulator/sim_demo 10000
	./simulator/sim_demo 10000 --strategy
	./simulator/sim_demo 10000 --router
	./simulator/sim_demo 10000 --strategy --router

clean:
	rm -f $(BINS)
	rm -rf $(BUILDDIR)
	find . -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
