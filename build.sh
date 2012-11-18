#!/bin/bash


rm -f recv.x86 recv.arm senderd.x86

# Compile reciever for x86
gcc -o recv.x86 recv.c ao.c -lcelt0 -lao -lm

# Compile pulseaudio sender for x86
gcc -o senderd.x86 senderd.c -lcelt0 -lpthread -Wall


# Compile reciever for ARM v4T

export PATH="/mnt/homes/ilya/elbereth/linux/usr/local/arm/4.3.2/bin:$PATH"

CFLAGS=" -march=armv4t -mtune=arm920t -Os" CXXFLAGS=" -march=armv4t -mtune=arm920t -Os" arm-linux-gcc -o recv.arm recv.c ao.c -L. -lao -lcelt0 -lm

