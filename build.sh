#!/bin/bash

g++ SDL2DisplayTest.cpp -o OGSTDisplay -lSDL2 -lSDL2_ttf -lpthread -I /usr/include -I /usr/include/SDL2 -L /usr/lib/arm-linux-gnueabihf -fopenmp
