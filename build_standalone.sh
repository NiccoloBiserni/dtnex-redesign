#!/bin/bash
# Self-contained build script for DTNEX
# Requires only system ION libraries, not source code

echo "Building DTNEX (self-contained)..."

# Check for OpenSSL development libraries
if ! pkg-config --exists openssl; then
    echo "OpenSSL development libraries not found"
    echo "Please install libssl-dev package first: sudo apt install libssl-dev"
    exit 1
fi

# Check for ION system libraries
if [ ! -f "/usr/local/lib/libbp.so" ] && [ ! -f "/usr/lib/libbp.so" ]; then
    echo "ION-DTN system libraries not found"
    echo "Please install ION-DTN from packages or build from source"
    echo "Required libraries: libbp, libici"
    exit 1
fi

# Clean previous build
rm -f dtnex.o ion_contacts.o dtnex

# Compile with local headers
gcc -Wall -g -I. -c dtnex.c -o dtnex.o
if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi

gcc -Wall -g -I. -c ion_contacts.c -o ion_contacts.o
if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi

# Link with system ION libraries
gcc dtnex.o ion_contacts.o -o dtnex -L/usr/local/lib -lbp -lici -lm -lpthread -lcrypto
if [ $? -ne 0 ]; then
    echo "Linking failed"
    echo "Make sure ION-DTN libraries are installed in /usr/local/lib or /usr/lib"
    exit 1
fi

echo "✅ Build complete! The dtnex binary is ready."
echo ""
echo "To run the application:"
echo "  ./dtnex [--debug]"
echo ""
echo "Make sure to edit dtnex.conf according to your needs."

chmod +x dtnex