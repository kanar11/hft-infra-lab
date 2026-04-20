.PHONY: build test benchmark simulate clean all

CXX      = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -pthread

# All binary targets
BINS = \
	orderbook/orderbook \
	orderbook/orderbook_v2 \
	orderbook/benchmark_orderbook \
	orderbook/latency_histogram \
	lockfree/spsc_queue \
	memory-latency/cache_latency \
	itch-parser/benchmark_itch \
	benchmarks/latency_benchmark \
	benchmarks/orderbook_benchmark \
	oms/oms_demo \
	risk/risk_demo \
	router/router_demo \
	logger/logger_demo \
	strategy/strategy_demo \
	fix-protocol/fix_demo \
	ouch-protocol/ouch_demo \
	dpdk-bypass/dpdk_demo \
	monitoring/monitor_demo \
	multicast/multicast_demo \
	simulator/sim_demo

all: build

build:
	$(CXX) $(CXXFLAGS) -o orderbook/orderbook orderbook/orderbook.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/orderbook_v2 orderbook/orderbook_v2.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/benchmark_orderbook orderbook/benchmark_orderbook.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/latency_histogram orderbook/latency_histogram.cpp
	$(CXX) $(CXXFLAGS) -o lockfree/spsc_queue lockfree/spsc_queue.cpp
	$(CXX) $(CXXFLAGS) -o memory-latency/cache_latency memory-latency/cache_latency.cpp
	$(CXX) $(CXXFLAGS) -o itch-parser/benchmark_itch itch-parser/benchmark_itch.cpp
	$(CXX) $(CXXFLAGS) -o benchmarks/latency_benchmark benchmarks/latency_benchmark.cpp
	$(CXX) $(CXXFLAGS) -o benchmarks/orderbook_benchmark benchmarks/orderbook_benchmark.cpp
	$(CXX) $(CXXFLAGS) -o oms/oms_demo oms/oms_demo.cpp
	$(CXX) $(CXXFLAGS) -o risk/risk_demo risk/risk_demo.cpp
	$(CXX) $(CXXFLAGS) -o router/router_demo router/router_demo.cpp
	$(CXX) $(CXXFLAGS) -o logger/logger_demo logger/logger_demo.cpp
	$(CXX) $(CXXFLAGS) -o strategy/strategy_demo strategy/strategy_demo.cpp
	$(CXX) $(CXXFLAGS) -o fix-protocol/fix_demo fix-protocol/fix_demo.cpp
	$(CXX) $(CXXFLAGS) -o ouch-protocol/ouch_demo ouch-protocol/ouch_demo.cpp
	$(CXX) $(CXXFLAGS) -o dpdk-bypass/dpdk_demo dpdk-bypass/dpdk_demo.cpp
	$(CXX) $(CXXFLAGS) -o monitoring/monitor_demo monitoring/monitor_demo.cpp
	$(CXX) $(CXXFLAGS) -o multicast/multicast_demo multicast/multicast_demo.cpp
	$(CXX) $(CXXFLAGS) -o simulator/sim_demo simulator/sim_demo.cpp

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
	@echo "=== FIX Protocol ==="
	./fix-protocol/fix_demo 500000
	@echo ""
	@echo "=== OUCH Protocol ==="
	./ouch-protocol/ouch_demo 500000
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
	find . -name '*.o' -delete
	find . -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
