#ifndef _COMPONENT_H_
#define _COMPONENT_H_

#define BEACON_INTERVAL     (28)                    // because of the funky way I did the timing (see comments above main loop) this is the interval between beacons.
                                                    // SHOULD always be divisible by 4.
#define MAX_NUMBER_OF_BEACONS   (BEACON_INTERVAL/4) // this is the max number of beacons that will fit in BEACON_INTERVAL

struct BeaconData {
    char timestamp[16];
    int txFreqHz;
};

extern double n3iznFreq;
extern int terminate;

extern int sendUDPEmailMsg( char *message );

#endif
