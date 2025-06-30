CXX = g++
CXXFLAGS = -std=c++17 -Wall `pkg-config --cflags opencv4`
LDFLAGS = `pkg-config --libs opencv4`

OBJS = main.o region.o neurona.o

all: orion

orion: $(OBJS)
	$(CXX) $(CXXFLAGS) -o orion $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f *.o orion

