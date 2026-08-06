#!/bin/bash
# Build script for DTNEX

# Check for ION headers in the local ione-code directory
if [ ! -d "../ione-code/bpv7/include" ] || [ ! -d "../ione-code/ici/include" ]; then
    echo "ION-DTN headers not found in ../ione-code/bpv7/include or ../ione-code/ici/include"
    echo "Please ensure the ione-code directory contains the ION source code"
    exit 1
fi

# Check for OpenSSL development libraries
if ! pkg-config --exists openssl; then
    echo "OpenSSL development libraries not found"
    echo "Please install libssl-dev package first"
    exit 1
fi

# Build the application
echo "Building DTNEX..."
rm -f dtnex.o dtnex

# Compile
gcc -Wall -g -I../ione-code/bpv7/include -I../ione-code/ici/include -c dtnex.c -o dtnex.o
gcc -Wall -g -I../ione-code/bpv7/include -I../ione-code/ici/include -c ion_contacts.c -o ion_contacts.o
if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi

# Link
gcc dtnex.o ion_contacts.o -o dtnex -L/usr/local/lib -lbp -lici -lm -lpthread -lcrypto
if [ $? -ne 0 ]; then
    echo "Linking failed"
    exit 1
fi

echo "Build complete. The dtnex binary is located in the current directory."
echo ""
echo "To run the application:"
echo "  ./dtnex [--debug]"
echo ""
echo "Make sure to edit dtnex.conf according to your needs."

chmod +x dtnex