# Makefile
# CS 4352 - Operating Systems
# Team 10 #
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread
LIBS     = -lcurl
TARGET   = scheduler_os

$(TARGET): scheduler_os.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) scheduler_os.cpp $(LIBS)

clean:
	rm -f $(TARGET)
