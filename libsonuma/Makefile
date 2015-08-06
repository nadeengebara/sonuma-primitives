#@author snovakov
SHELL = /bin/sh
CC    = g++-4.7

CFLAGS       = -fPIC -Wall -O3

#-combine
LDFLAGS	= -shared -fPIC -lc -lpthread -lrt
#LDFLAGS	= -static-libstdc++ -nodefaultlibs -Wl,--whole-archive ./linux/libevent/lib/libevent.a -Wl,--no-whole-archive  -shared -fPIC
#-lstdc++ -lpthread -levent -shared

TARGET  = libsonuma.so

SOURCES = \
	./sonuma.c \
	./soft_rmc.c

HEADERS = \
	./sonuma.h \
	./soft_rmc.h
 
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJECTS)

clean:
	-${RM} ${TARGET} ${OBJECTS} $(SRCS:.c=.o)
