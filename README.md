# twsprRPI
A C program to send out WSPR beacons and then read and display reports from wsprnet.org.

This program is written specific to my needs.  It is not a general solution for everyone.  I'm placing this in the public domain so anyone can use all or part of it as they see fit, subject to the MIT license.

## Overview

This program sends out a WSPR beacon on 15m, 10m, 6m, and 2m.  The four two minute beacons have two minute intervals between them.  It waits two more minutes and then goes to wsprnet.org and collects reports from these four beacons.  It parses and displays the results.  Any report greater than 0 dB is printed in BLUE, any report from more than 3000 miles away is in GREEN, and, when 10m is shutting down, all 10m reports in RED (wsprnet.c).

This process repeates every 28 minutes.  During the intervals between beacons the radio is set to monitor some other frequency.  The actual frequencies used are stored in WSPRConfig, a simple text file.

The program uses the Port Audio library (same as WSJT-X).  It reads wav files and sends them out the sound card (wav_output2.c).  The files in this repository were generated using wspr0, a deprecated program.  WSJT-X and wsprd can also generate them.  For 2m and 6m the 1500Hz file is always chosen.  For 10m and 15m, more crowded bands, it rotates through them.

The radio is an old Yaesu FT847 (using ft847.c).  Obviously you will need to substitute a controller for your own radio or use one of the libraries out there.  (The FT847 had limited CAT control.  A modern radio would allow more interesting features to be added).

The program was originally written on an Ubuntu box and them oved to a Raspberry Pi (hence the RPI in the name).  There is no makefile.  This is the command used to build:
  
  gcc -g -Wall -o twsprRPI twsprRPI.c wav_output2.c ft847.c wsprnet.c azdist.c geodist.c grid2deg.c -lportaudio -lrt -lm -lasound -pthread
  
I've made no attempt at optimization.  The last three C files are translated from WSJT-X Fortran code, used to compute azimuth and distance.

Enjoy!
