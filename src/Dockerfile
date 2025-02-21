# Use a more specific base image with Vulkan support
FROM nvidia/opengl:1.2-glvnd-runtime-ubuntu20.04

# Install dependencies with specific versions
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    cmake=3.16.3-1ubuntu1 \
    libvulkan-dev \
    vulkan-tools \
    libglfw3-dev \
    libglm-dev \
    git \
    pkg-config \
    python3 \
    ninja-build

# Set working directory
WORKDIR /scos

# Copy CMakeLists.txt first to leverage Docker cache
COPY CMakeLists.txt .
COPY src/ src/

# Create build directory and build project
RUN mkdir -p build && \
    cd build && \
    cmake -GNinja .. && \
    ninja

# Set the executable as entry point
CMD ["./build/scos"]
