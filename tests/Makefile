CPPFLAGS ?=
CPPFLAGS += -I..

CXXFLAGS ?=
CXXFLAGS += -std=c++11 -O3 -g -Wall -Wextra

TARGET = main
OBJ = main.o second.o

LINK.o := $(LINK.cc)

$(TARGET): $(OBJ)

$(OBJ): $(OBJ:.o=.cc) ../cxxenvi.hh

clean:
	$(RM) $(OBJ) $(TARGET)

.PHONY: clean
