.PHONY: build test benchmark simulate lint clean

CXX      = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -pthread

build:
	$(CXX) $(CXXFLAGS) -o orderbook/orderbook orderbook/orderbook.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/orderbook_v2 orderbook/orderbook_v2.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/benchmark_orderbook orderbook/benchmark_orderbook.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/latency_histogram orderbook/latency_histogram.cpp
	$(CXX) $(CXXFLAGS) -o lockfree/spsc_queue lockfree/spsc_queue.cpp
	$(CXX) $(CXXFLAGS) -o memory-latency/cache_latency memory-latency/cache_latency.cpp
	$(CXX) $(CXXFLAGS) -o itch_parser/benchmark_itch itch_parser/benchmark_itch.cpp
	$(CXX) $(CXXFLAGS) -o benchmarks/latency_benchmark benchmarks/latency_benchmark.cpp
	$(CXX) $(CXXFLAGS) -o benchmarks/orderbook_benchmark benchmarks/orderbook_benchmark.cpp
	$(CXX) $(CXXFLAGS) -o oms/oms_demo oms/oms_demo.cpp

test: build
	python3 tests/test_oms.py
	python3 tests/test_itch.py
	python3 tests/test_ouch.py
	python3 tests/test_fix.py
	python3 tests/test_router.py
	python3 tests/test_risk.py
	python3 tests/test_logger.py

benchmark:
	python3 tests/benchmark.py
	cd orderbook && ./benchmark_orderbook
	cd orderbook && ./latency_histogram
	cd lockfree && ./spsc_queue
	cd memory-latency && ./cache_latency
	cd itch_parser && ./benchmark_itch
	./benchmarks/latency_benchmark 100000
	./benchmarks/orderbook_benchmark 100000
	./oms/oms_demo 500000

simulate:
	python3 simulator/market_sim.py 10000

lint:
	@echo "=== Syntax check: all Python files ==="
	@python3 -m py_compile config_loader.py
	@python3 -m py_compile oms/oms.py
	@python3 -m py_compile itch_parser/itch_parser.py
	@python3 -m py_compile strategy/mean_reversion.py
	@python3 -m py_compile router/smart_router.py
	@python3 -m py_compile risk/risk_manager.py
	@python3 -m py_compile simulator/market_sim.py
	@python3 -m py_compile monitoring/infra_monitor.py
	@python3 -m py_compile dpdk-bypass/kernel_bypass_sim.py
	@python3 -m py_compile ouch-protocol/ouch_sender.py
	@python3 -m py_compile fix-protocol/fix_parser.py
	@python3 -m py_compile multicast/mc_sender.py
	@python3 -m py_compile multicast/mc_receiver.py
	@python3 -m py_compile logger/trade_logger.py
	@python3 -m py_compile multicast/mc_receiver_latency.py
	@python3 -m py_compile tests/benchmark.py
	@python3 -m py_compile tests/benchmark_chart.py
	@echo "All Python files OK"

clean:
	rm -f orderbook/orderbook orderbook/orderbook_v2 orderbook/benchmark_orderbook
	rm -f orderbook/latency_histogram lockfree/spsc_queue memory-latency/cache_latency
	rm -f itch_parser/benchmark_itch
	rm -f benchmarks/latency_benchmark benchmarks/orderbook_benchmark
	rm -f oms/oms_demo
