# twsprRPI
A C program to send out WSPR beacons and then read and display reports from wsprnet.org.

This program was written specific to my needs.  It is not a general solution for everyone.  I'm placing this in the public domain so anyone can use all or part of it as they see fit, subject to the MIT license.

## Overview

This program sends out a WSPR beacon on 15m, 10m, 6m, and 2m.  The four two minute beacons have a two minute interval between them.  Two minutes after the last beacon the program goes to wsprnet.org and collects all reports from these four beacons.  It then parses and displays the results.  Any report greater than 0 dB is printed in BLUE, any greater than 3000 miles away is in GREEN, and, when 10m is shutting down, any 10m reports in RED.


