.PHONY: build test benchmark clean

build:
	cd orderbook && g++ -O2 -o orderbook orderbook.cpp
	cd orderbook && g++ -O2 -o benchmark_orderbook benchmark_orderbook.cpp

test:
	python3 tests/test_oms.py
	python3 tests/test_itch.py

benchmark:
	python3 tests/benchmark.py
	cd orderbook && ./benchmark_orderbook

clean:
	rm -f orderbook/orderbook orderbook/benchmark_orderbook
