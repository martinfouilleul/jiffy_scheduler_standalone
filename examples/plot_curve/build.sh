#!/bin/bash

if [ ! -d ./bin ] ; then
	mkdir ./bin
fi

INCLUDES="-I../../src -I../../src/util -I../../src/platform"
LIBS="-L../../bin -lsched"
FLAGS="-g -DDEBUG -mmacos-version-min=10.15.4"

clang++ $FLAGS -o ./bin/print_curve $INCLUDES $LIBS main.cpp
