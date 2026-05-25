CXX      ?= g++
CXXFLAGS := -O3 -std=c++17 -march=native -pthread -Wall -Wno-unused-function -Wno-unused-variable
LDFLAGS  := -pthread

all: ingest build_indexes

ingest: ingest.cpp common.h
	$(CXX) $(CXXFLAGS) ingest.cpp -o ingest $(LDFLAGS)

build_indexes: build_indexes.cpp common.h
	$(CXX) $(CXXFLAGS) build_indexes.cpp -o build_indexes $(LDFLAGS)

clean:
	rm -f ingest build_indexes
