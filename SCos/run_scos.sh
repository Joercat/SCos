
#!/bin/bash

echo "SCos Runner"
echo "==========="

# Clean up any existing QEMU processes
pkill -9 -f qemu 2>/dev/null
sleep 1

# Build the kernel
echo "Building SCos..."
make clean
make all

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "Build successful!"
echo "Starting SCos..."

# Run SCos - Replit will automatically detect and show VNC
qemu-system-i386 \
    -drive format=raw,file=scos.img \
    -m 32M \
    -no-reboot \
    -no-shutdown \
    -vga std \
    -name "SCos"

echo "SCos finished"
