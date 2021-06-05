#!/bin/bash

DEBUG_FLAGS="-g -DDEBUG"

#--------------------------------------------------------------
# Detect OS and set environment variables accordingly
#--------------------------------------------------------------
OS=$(uname -s)

if [ $OS = "Darwin" ] ; then
	#echo "Target '$target' for macOS"
	CC=clang
	CXX=clang++
	DYLIB_SUFFIX='dylib'
	SYS_LIBS=''
	FLAGS="-mmacos-version-min=10.15.4"

elif [ $OS = "Linux" ] ; then
	echo "Error: Linux is not supported yet"
	exit -1
else
	echo "Error: Unsupported OS $OS"
	exit -1
fi

#--------------------------------------------------------------
# Set paths
#--------------------------------------------------------------
BINDIR="./bin"
SRCDIR="./src"
RESDIR="./resources"
INCLUDES="-I$SRCDIR -I$SRCDIR/util -I$SRCDIR/platform"

#--------------------------------------------------------------
# Build
#--------------------------------------------------------------

if [ ! \( -e bin \) ] ; then
	mkdir ./bin
fi

# We use one compilation unit for all C++ code
clang++ $DEBUG_FLAGS -c -o $BINDIR/sched.o $FLAGS $INCLUDES $SRCDIR/sched_main.cpp
# build the static library
libtool -static -o $BINDIR/libsched.a $BINDIR/sched.o
