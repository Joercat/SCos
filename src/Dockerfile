FROM ubuntu:latest

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libvulkan-dev \
    vulkan-tools \
    libglfw3-dev \
    libglm-dev \
    git

# Set working directory
WORKDIR /src

# Copy source code
COPY . .

# Build the project
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

# Set entry point
ENTRYPOINT ["./build/scos"]
