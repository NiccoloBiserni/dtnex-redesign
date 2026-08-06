# Makefile for DTNEX

# Compiler and flags
CC = gcc
CFLAGS = -Wall -g
LIBS = -L/usr/local/lib -lbp -lici -lm -lpthread -lcrypto

# ION headers
ION_INCDIR = ../ione-code
INCLUDES = -I$(ION_INCDIR)/bpv7/include -I$(ION_INCDIR)/ici/include

# Target and source files
TARGET = dtnex
SOURCES = dtnex.c ion_contacts.c
OBJECTS = dtnex.o ion_contacts.o

# Default target
all: $(TARGET)

# Build target
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LIBS)

# Compile source files
dtnex.o: dtnex.c dtnex.h ion_contacts.h
	$(CC) $(CFLAGS) $(INCLUDES) -c dtnex.c

ion_contacts.o: ion_contacts.c ion_contacts.h
	$(CC) $(CFLAGS) $(INCLUDES) -c ion_contacts.c

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)

# Install system-wide
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Uninstall
uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
