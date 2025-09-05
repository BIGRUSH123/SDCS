CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
TARGET = cache_server
SOURCES = main.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

.PHONY: all clean
