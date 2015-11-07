#@author ustiugov
SHELL = /bin/sh
CC  = gcc

#CFLAGS       = -fPIC -Wall -m64 -mcpu=v9 -O0 -g -I../../libsonuma
CFLAGS       = -fPIC -Wall -m64 -mcpu=v9 -O3 -g -I../../libsonuma

#-combine
#LDFLAGS	= -shared -fPIC -lc -lpthread -lrt
LDFLAGS	= -shared -fPIC -lc
#LDFLAGS__SABRE_UBENCH = -shared -fPIC -lc -lpthread -lrt -lsonuma -L. 
LDFLAGS__SABRE_UBENCH = -lsonuma -L. -lpthread 
#LDFLAGS	= -static-libstdc++ -nodefaultlibs -Wl,--whole-archive ./linux/libevent/lib/libevent.a -Wl,--no-whole-archive  -shared -fPIC
#-lstdc++ -lpthread -levent -shared

TARGET  = libsonuma.so

#TARGET_SABRE_UBENCH = SABRes_sync_ubench
TARGET_SABRE_UBENCH = SABRes_son_multithread

SOURCES = \
	../../libsonuma/sonuma.c

HEADERS = \
	../../libsonuma/sonuma.h
 
SOURCES_SABRE_UBENCH = \
	./SABRes_son_multithread.c
#	./SABRes_sync_ubench.c
 
OBJECTS = $(SOURCES:.c=.o)

OBJECTS_SABRE_UBENCH = $(SOURCES_SABRE_UBENCH:.c=.o)

sonuma: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJECTS)

sab_ub: $(TARGET_SABRE_UBENCH)

$(TARGET_SABRE_UBENCH): $(OBJECTS_SABRE_UBENCH)
	$(CC) $(FLAGS) $(CFLAGS) -o $(TARGET_SABRE_UBENCH) $(OBJECTS_SABRE_UBENCH) $(LDFLAGS__SABRE_UBENCH)

.PHONY: clean

clean:
	-${RM} ${TARGET} ${OBJECTS} $(SRCS:.c=.o) $(TARGET_SABRE_UBENCH) $(OBJECTS_SABRE_UBENCH) 
