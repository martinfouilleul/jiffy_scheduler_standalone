Jiffy Temporal Scheduler
------------------------

Jiffy is a temporal cooperative scheduler for musical applications. It can schedule computations along multiple symbolic timelines synchronized with each other using tempo curves. This allows to author and execute rich and open temporal scenarios to pilot sound effects, lights, mechatronics, etc.

User code is executed in fibers and can yield and request to be rescheduled at a later symbolic time, or wait until another fiber has completed. It can also request to be put in a background thread to execute blocking actions without blocking the cooperative scheduling.

Symbolic timescales are organized as a hierarchy: the symbolic time of a timescale is translated to the symbolic time of its parent (and to the real-time at the root of the hierarchy) using tempo curves. These curves are defined piecewise by linear or cubic Bézier curves. Tempo can be specified either as a function of a timescale's symbolic time, or as a function of its parent's (more concrete) time.

Building and linking
--------------------
This project currently builds on macOS 10.15.4. Run build.sh to build the library.
Then include ./src/scheduler.h in your sources and link your program to libsched.a.

Using
-----
Documentation is currently an ongoing work. In the meantime, please refer to ./src/scheduler.h and examples/tick/main.cpp.

License
-------
This project is licensed under the terms of the MIT (expat) license. See LICENSE or http://opensource.org/licenses/MIT for more information.
