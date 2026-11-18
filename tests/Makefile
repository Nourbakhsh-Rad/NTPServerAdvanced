# Native (host) unit tests for NTPServerAdvanced (class AdvancedNTPServer).
# No ESP32/ESP8266 toolchain required — builds against the Arduino/AsyncUDP
# stub headers in stubs/ using the system g++.
#
# Usage:
#   make        # build tests/test_ntp
#   make run    # build and run, exit code reflects pass/fail
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++14 -DESP32 -DNTP_TESTING -Wall -Wextra -I../src -Istubs
SRC      := test_ntp.cpp stubs/serial_impl.cpp
BIN      := test_ntp

.PHONY: all run clean
all: $(BIN)

$(BIN): $(SRC) ../src/NTPServer_Advanced.h ../src/NTPServer_Advanced.cpp
	$(CXX) $(CXXFLAGS) $(SRC) ../src/NTPServer_Advanced.cpp -o $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
