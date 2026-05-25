CXX      ?= g++
CXXFLAGS := -O3 -std=c++17 -march=native -pthread -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
LDFLAGS  := -pthread

all: ingest build_indexes

ingest: ingest.cpp csv_common.h
	$(CXX) $(CXXFLAGS) ingest.cpp -o ingest $(LDFLAGS)

build_indexes: build_indexes.cpp
	$(CXX) $(CXXFLAGS) build_indexes.cpp -o build_indexes $(LDFLAGS)

clean:
	rm -f ingest build_indexes

.PHONY: all clean
