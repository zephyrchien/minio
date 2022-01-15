CXX := g++
CXXFLAGS += -std=c++23

SRCS := $(wildcard *.cpp)
BINS := $(SRCS:%.cpp=%)

all: $(BINS)

$(BINS): %: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@


.PHONY: clean
clean:
	rm $(BINS)
