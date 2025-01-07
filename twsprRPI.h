#ifndef _COMPONENT_H_
#define _COMPONENT_H_

#define BEACON_INTERVAL     (28)                    // because of the funky way I did the timing (see comments above main loop) this is the interval between beacons.
                                                    // SHOULD always be divisible by 4.
#define MAX_NUMBER_OF_BEACONS   (BEACON_INTERVAL/4) // this is the max number of beacons that will fit in BEACON_INTERVAL

#define WSPR_30M            (10138700)
#define WSPR_17M            (18104600)
#define WSPR_15M            (21094600)
#define WSPR_12M            (24924600)
#define WSPR_10M            (28124600)
#define WSPR_6M             (50293000)
#define WSPR_2M             (144489000)

struct BeaconData {
    char timestamp[16]; // the UTC time-of-day that the beacon begins
    int txFreqHz;       // the frequency read from the configuration file
    int txFreqHzActual; // the frequency actually set in the radio, after compensation
    char tone[16];      // "1500.wav", converted to double later
    double temperature; // the temperature at the time the beacon begins
};

extern double n3iznFreq;
extern int terminate;

extern int sendUDPEmailMsg( char *message );
extern int readConfigFileWSPRFreq( int convResult );

#endif
