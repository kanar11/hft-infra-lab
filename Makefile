.PHONY: build test benchmark simulate clean

CXX      = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -pthread

build:
	$(CXX) $(CXXFLAGS) -o orderbook/orderbook orderbook/orderbook.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/orderbook_v2 orderbook/orderbook_v2.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/benchmark_orderbook orderbook/benchmark_orderbook.cpp
	$(CXX) $(CXXFLAGS) -o orderbook/latency_histogram orderbook/latency_histogram.cpp
	$(CXX) $(CXXFLAGS) -o lockfree/spsc_queue lockfree/spsc_queue.cpp
	$(CXX) $(CXXFLAGS) -o memory-latency/cache_latency memory-latency/cache_latency.cpp

test: build
	python3 tests/test_oms.py
	python3 tests/test_itch.py
	python3 tests/test_ouch.py
	python3 tests/test_fix.py
	python3 tests/test_router.py

benchmark:
	python3 tests/benchmark.py
	cd orderbook && ./benchmark_orderbook
	cd orderbook && ./latency_histogram
	cd lockfree && ./spsc_queue
	cd memory-latency && ./cache_latency

simulate:
	python3 simulator/market_sim.py 10000

clean:
	rm -f orderbook/orderbook orderbook/orderbook_v2 orderbook/benchmark_orderbook
	rm -f orderbook/latency_histogram lockfree/spsc_queue memory-latency/cache_latency
