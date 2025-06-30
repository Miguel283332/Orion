CXX = g++
CXXFLAGS = -std=c++17 -Wall `pkg-config --cflags opencv4`
LDFLAGS = `pkg-config --libs opencv4`

OBJS_ORION = main.o region.o neurona.o
OBJS_EDITOR = editor.o region.o neurona.o

all: orion editor

orion: $(OBJS_ORION)
	$(CXX) $(CXXFLAGS) -o orion $(OBJS_ORION) $(LDFLAGS)

editor: $(OBJS_EDITOR)
	$(CXX) $(CXXFLAGS) -o editor $(OBJS_EDITOR)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f *.o orion editor

