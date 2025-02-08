/*
    gcc -g -Wall -o twsprRPI twsprRPI.c wav_output3.c ft847.c wsprnet.c azdist.c geodist.c grid2deg.c getTempData.c -lrt -lm -lasound -pthread

    When running direct stderr to null with
        ./twsprRPI 2>/dev/null
    (or to a file instead of null)

    * A log file exist named log_twspr.txt.  It records startup, shutdown, and bursts (including freq).  I had intended to keep a second log that kept track of
    total up time and total down time as well as the bursts on each band.  But that info can all be collected from the log file so I'm leaving this off for now.

    * Another file, WSPRConfig, is read before starting any beacon.  It contains the Rx and Tx frequencies.  It is closed after every run so it can be modified.

    * A third file, blackout.txt, exists to prevent sending beacon during a satellite pass.  It is checked at the beginning of each waitForTopOfEvenMinute().  If
    necessary it will hold the program until the blackout period ends.  Format of blackout.txt file is described just above doBlackout() below.

    * A fourth file, raw_reports_log.txt, records all stations that reported hearing my beacon.  The date is missing from the first year or so of this file.

    * A fifth file, duplicate.txt, allows me to monitor the beacons from another computer allowing me to make QSOs using the idle time between beacons.  I also
    use it to print out debug lines.

    Ideas:
     - steer radio to different frequencies, say 40 MHz for one hour every night, then 2m for one hour every night, 6m for one hour.  Will have to
       change WSJT-X frequency.  One way to do this automatically seems to be to save several --rig-name options.  There doesn't seem to be a UDP
       UDP message to change this.  Another option is to use the different configurations.  It can be invoked via UDP but I experienced problems (a long
       time ago) when switching between MSK144 and FT8.

*/
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "ft847.h"
#include "twsprRPI.h"
#include "wav_output3.h"
#include "getTempData.h"


#include <netinet/in.h>
#include <net/if.h>

#include <sys/types.h>
#include <ifaddrs.h>

#include <netdb.h>

#define MINUTES_TO_WAIT     (BEACON_INTERVAL+1)
#define SECONDS_TO_WAIT     (MINUTES_TO_WAIT*60)    //  Tx for 2 min, wait 25 min, then waitForTopOfEvenMinute() will wait for the next min, resulting in Tx 28 min apart.
#define WSPR_DEFAULT_15M    (21094630)
#define WSPR_DEFAULT_10M    (28124620)
#define WSPR_DEFAULT_6M     (50293080)
#define WSPR_DEFAULT_2M     (144489110)
#define CONFIG_FILENAME     "WSPRConfig"
#define TEMPERATURE_BEACON_MAX          ((double)85.0)                  // stop sending beacons above this temperature
#define TEMPERATURE_POWER_OFF           (TEMPERATURE_BEACON_MAX+5.0)    // power down radio above this temperature
#define TEMPERATURE_HYSTERESIS_BOTTOM   (TEMPERATURE_BEACON_MAX+1.0)    // if powered off then power on below this temperature

#define NO_WAIT_FIRST_BURST     1
#define BLACKOUT_FILENAME       "blackout.txt"
#define DUP_FILENAME            "duplicate.txt"             // used to monitor beacons from another computer allowing me to make QSOs using the idle time between beacons.
#define UDP_TX_MESSAGE          "txMode;"
#define DEFAULT_MY_IP           "192.168.1.105"

extern int doCurl( struct BeaconData *beaconData, char* termPTSNum );

int terminate = 0;

static int signalCaptured = 0;
static int sock;                        // for all UDP messages
static struct sockaddr_in adr_inet;     // for UDP message to preamp.py
int SockAddrStructureSize;              // for UDP message to preamp.py
static struct sockaddr_in adr_inet2;    // for UDP message to SDRPlay// AF_INET
int SockAddrStructureSize2;             // for UDP message to SDRPlay
static struct sockaddr_in adr_inet3;    // for UDP message to send Email
int SockAddrStructureSize3;             // for UDP message to send Email
static struct sockaddr_in adr_clnt4;    // for receiving data, used in recvfrom() in two locations
static int sockRx;                      // socket for receiving data from UDPRepeater4.py

static char lineToRemove[256] = "";     // for blackoutCheck() and blackoutUpdateFile()
static FILE *dupFile;                   // for DUP_FILENAME, see comment above
static char myIP[ INET_ADDRSTRLEN ];

int sendUDPEmailMsg( char *message );
int readConfigFileWSPRFreq( int convResult );


static int readConfigFile( int *rxFreqHz, struct BeaconData *beaconData );
static int readConfigFileWSPRFreqHelp( int convResult, int WSPRFreq );
static int readConfigFileHelp( char *string );
static void SignalHandler( int signal );
static int txWspr( int rxFreq, struct BeaconData *beaconData); //, int txFreq, char* timestamp );
static char *getWavFilename( int txFreq );
static int waitForTopOfEvenMinute( int txFreq );
static int updateFiles( char *eventName );
static int findttyUSB( void );
static int installSignalHandlers( int useMyHandlers );
static int initializeNetwork( void );
static void closeNetwork( void );
static int sendUDPMsg( int doingTx );
static int getMyIPAddress( char *myIPAddress );
static int blackoutCheck( time_t *blackoutEndTime );
static int blackoutUpdateFile( void );
static int doBlackout( void );


static void SignalHandler( int signal ) {
    signalCaptured = signal;
    terminate = 1;
}


int main( int argc, char **argv ) {
    fd_set rfds;
    struct timeval tv;
    int retval;
    int NumBytesIn;
    struct BeaconData beaconData[ MAX_NUMBER_OF_BEACONS ];
    char termPTSNum[4] = "";
    int heatWait = 0;
    int resetSelectWait = 1;

    int rx0FreqHz = WSPR_DEFAULT_10M;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i],"?"))  {
                printf("\n\nUsage \"./twsprRPI <PTS> 2>/dev/null\"");
                printf("\n     - <PTS> terminal number used to output 15m results.  Appended to /dev/pts/.");
                printf("\n       Use \"tty\" command to determine this terminal's number.");
                printf("\n       Do NOT use leading zeros.");
                printf("\n       If parameter is not a number then 15m output will go the this terminal.");
                printf("\n\n");
                return 1;
            }

            //  Anything else then check to see if all numbers
            if ( strspn(argv[i], "0123456789") == strlen(argv[i]) ) {
                strcpy(termPTSNum,argv[i]);
                printf("\nUsing /dev/pts/%s for lowest freq output\n\n",termPTSNum);
            }
        }
    }

    if (getMyIPAddress( myIP ) != 0) {
        strcpy( myIP, DEFAULT_MY_IP );      // if error set default
    }
    printf("My IP Address %s\n",myIP);

    dupFile = (FILE *)fopen(DUP_FILENAME,"wt");
    if (dupFile == (FILE *)NULL) {
        printf("Unable to open %s for writing.\n",DUP_FILENAME);
        return 1;
    }

    if (initializeNetwork() == -1) { return -1; }
    if (initializePortAudio() == -1) { return -1; }
    if (ft847_open() == -1) { return -1; }
    if (updateFiles("Startup ")) { return 1; }

    for (int iii = 0; iii < MAX_NUMBER_OF_BEACONS; iii++) {    // initialize beacon data.  Really not necessary.  It's initialized in readConfigFile()
        beaconData[iii].txFreqHz = 0;
        beaconData[iii].timestamp[0] = 0;
        beaconData[iii].tone[0] = 0;
        beaconData[iii].txFreqHzActual = 0;
        beaconData[iii].temperature = 0.0;
    }

    printf("\n\n");
    printf("<ENTER> to quit, '*'+<ENTER> to read WSPRConfig and change Rx freq, '-'+<ENTER> to terminate wait period\n");
    printf("\n");

    // minCounter counts minutes from 0 to minWait.
    int minWait = MINUTES_TO_WAIT;
#ifdef NO_WAIT_FIRST_BURST
    int minCounter = MINUTES_TO_WAIT-1;         // so it does a Tx on the next available interval
    printf("Wait 1 min (<ENT>, *<ENT>, -<ENT>): ");  fflush( (FILE *)NULL );
#else
    int minCounter = 0;
#endif

    if (installSignalHandlers(1)) {
        return 1;
    }

    usleep(10000);
    if (ft847_setUSBMode()) {           // sometimes I have the radio in FM mode before starting this program.  If I don't set it to USB then
        return 1;                       //      it will send out 100w when MOX is asserted and stay there for two minutes.
    }


    //
    //  Main Loop
    //

    //  Each loop is one minute.  At timeout minCounter is bumped.  If less than minWait then loop.  Once the beacons begin, timing is done within txWspr() with a call
    //      to waitForTopOfEvenMinute().  When it emerges from the beacon block (all the calls to txWspr()) minCounter is set to 0.
    //  However, within that beacon block minWait is initialized to MINUTES_TO_WAIT and then dropped by 4 minutes with each beacon sent.  After the last beacon
    //      it is dropped by two more minutes for WSPRNet.org collections.  So when the beacon block ends, minCounter == 0 and minWait has the number of
    //      minutes to wait until the next beacon block.
    //  It goes off every 28 minutes because -  MINUTES_TO_WAIT == 29.  minWait is set to MINUTES_TO_WAIT just before each beacon block begins.  After each individual
    //      beacon is sent minWait -= 4.  After doCurl() minWait -= 2.  If there are four beacons then when the beacon block is complete, although minWait has dropped 18
    //      minutes, only 16 minutes have elapsed since the first beacon began.  Now minWait == 11 so by the time minCounter == minWait 27 minutes have elapsed since the
    //      first beacon.  The loop then drops into the beacon block with one minute left.  The first call to txWspr() consumes that minute.
    while (terminate == 0) {
        if (minCounter == 0) {
            if (resetSelectWait) {      // I use this an an indication that a UDP message arrived.  In that case this has already been printed.  Don't do it again.
                printf("Wait %d min (<ENT>, *<ENT>, -<ENT>): ",minWait);  fflush( (FILE *)NULL );
                fprintf(dupFile,"Wait %d min (<ENT>, *<ENT>, -<ENT>): ",minWait);  fflush( (FILE *)dupFile );
            }
        }

        if (resetSelectWait) {
            tv.tv_sec = 60;
            tv.tv_usec = 0;
        }
        resetSelectWait = 1;    // cleared when a UDP message arrives, don't want to get out of sync with top-of-minute

        FD_ZERO(&rfds);
        FD_SET(0, &rfds);       //  stdin
        FD_SET(sockRx, &rfds);

        retval = select((sockRx + 1), &rfds, NULL, NULL, &tv);
        if (retval < 0) {
            if (terminate == 0) { printf("Error in select()\n"); } else { printf("Captured signal %d\n",signalCaptured); }
            break;
        }
        else if (retval > 0) {
            if (FD_ISSET(sockRx, &rfds)) {       //  if data on Ethernet port
                //  Determine how many characters are waiting on the network port .....
                ioctl(sockRx,TIOCINQ,&NumBytesIn);
                if (NumBytesIn > 0) {
                    unsigned int len_inet;
                    int iii;
                    unsigned char dgram[512];              // receive buffer

                    len_inet = sizeof(adr_clnt4);
                    iii = recvfrom(sockRx,
                            dgram,          // receive buffer
                            sizeof(dgram),  // max length (a constant her would be better).
                            0,              // no flags
                            (struct sockaddr *)&adr_clnt4,   // filled in by function
                            &len_inet
                            );
                    if (iii < 0) {
                        printf("UDP receive socket - recvfrom() failed\n");
                        continue;
                    }
                    dgram[iii] = 0;

                    //for (int jjj = 0; jjj < iii; jjj++) { printf("%02hhx ",dgram[jjj]); } printf("\n");   // print out bytes
                    //printf("%s\n",(char *)dgram);     // print out message
                    //printf("tv.tv_sec %ld\n",(long)tv.tv_sec);    // print out number of seconds left in select()

                    //  If transmitting then make sure that we don't transmit in the next two minutes.
                    if (strcmp(UDP_TX_MESSAGE,(char *)dgram) == 0) {

                        //  if it is close to starting the beacon sequence (minWait is signed so if minWait < 3 (unlikely) the IF statement will still do the right thing)
                        if (minCounter > (minWait-3)) {
                            if (minCounter < 2) {      // (won't happen the way I use it, minCounter counts up not down)
                                minCounter = 0;
                            } else {
                                minCounter -=2;         // add two minutes to wait
                            }
                            printf("+2 ");  fflush( (FILE *)NULL );
                            fprintf(dupFile,"+2 ");  fflush( (FILE *)dupFile );
                        }
                    }
                    resetSelectWait = 0;    // continue with whatever seconds are left in select() 60 second sleep so it stays in sync with top-of-minute
                    continue;               // necessary to avoid minCounter being bumped below.
                }
            }
            else if (FD_ISSET(0, &rfds)) {          //  If keyboard data.  Note I'm not putting the keyboard in raw mode so
                //  The keyboard was hit.           //    nothing will happen unless I hit ENTER.
                ioctl(0,TIOCINQ,&NumBytesIn);
                if (NumBytesIn > 0) {
                    int iii = getchar();
                    if (NumBytesIn == 1) {      // if <ENTER>.  Since not in raw mode then if only one character it has to be <ENTER>
                        retval = 0;
                        break;
                    }
                    else if (NumBytesIn == 2) {
                        getchar();                  // get the <ENTER>.
                        if (45 == iii) {            // if '-' then cut the wait time short
                            minCounter = minWait;
                        } else if (42 == iii) {     // if '*' then read the config file and change Rx freq
                            if (readConfigFile( &rx0FreqHz, beaconData ) || (ft847_writeFreqHz( rx0FreqHz ))) {
                                retval = -1;
                                break;
                            }
                        }
                    } else {
                        printf("Unknown command\n");
                        retval = -1;
                        break;
                    }
                }           //  if (NumBytesIn > 0)
            }             //  else if (FD_ISSET(0, &rfds))    //  stdin
        }               //  else <select returned without error or timeout>

        //  If just timeout
        minCounter++;
        if (minCounter < minWait) {
            printf("%0d ",minCounter);  fflush( (FILE *)NULL );
            fprintf(dupFile,"%0d ",minCounter);  fflush( (FILE *)dupFile );
        } else {
            int beaconWasSent = 0;
            int beaconCounter = 0;
            int heatWaitInLog = 0;
            int heatWaitPowerOff = 0;

            //  Before starting make sure /dev/ttyUSBFT847 still points to ttyUSB0 or ttyUSB1.  If it points to something else then the USB to RS232 port
            //      is going south.  See 4/25/2024 entry in LinuxNotes2.docx or RaspberryPiNotes.docx
            if (findttyUSB()) {         // return 0 if ok, 1 if ttyUSBFT847 points to something else, -1 on error.
                sendUDPEmailMsg( "RPi .104 ttyUSBFT847 problem\nttyUSBFT847 no longer points to ttyUSB0 or ttyUSB1\n  It is either gone or points to another ttyUSBX\n" );
                retval = -1;
                break;
            }
            //
            //  Read the config file and prepare for the beacons
            //
            if (readConfigFile( &rx0FreqHz, beaconData )) {
                retval = -1;    // on error or if rx0FreqHz == 0
                break;
            }
            if (ft847_writeFreqHz( rx0FreqHz )) {                 // set radio to newly read frequency.  It happens again at the end of txWspr() but I don't want to wait.
                retval = -1;    // on error or if rx0FreqHz == 0
                break;
            }
            printf("Rx %d\nTx ",rx0FreqHz );
            fprintf(dupFile,"Rx %d\nTx ",rx0FreqHz );
            for (int iii = 0; iii < MAX_NUMBER_OF_BEACONS; iii++) {         // zero out timestamp strings
                //beaconData[iii].timestamp[0] = 0;                     // this is done in readConfigFile() as each beacon is read.
                if (beaconData[iii].txFreqHz != 0) {
                    printf("%d  ",beaconData[iii].txFreqHz);
                    fprintf(dupFile,"%d  ",beaconData[iii].txFreqHz);
                }
            }
            printf("\n");
            fprintf(dupFile,"\n");
            minWait = MINUTES_TO_WAIT;

            //
            //  Before starting the beacon code block check here if temperature is > 90 deg.  If so wait.
            //
            heatWait = 0;
            printf("\n");  fprintf(dupFile,"\n");
            while (terminate == 0) {
                double currentTemperature = getTempData();
                if (currentTemperature < TEMPERATURE_BEACON_MAX) { //  This will capture errors also.  getTempData() returns -1.0 on error and +1.0 if process not running.
                    break;
                } else {
                    // decide whether to turn FT847 power off.  Turn off > 90 but don't turn back on until < 86.
                    if (heatWaitPowerOff) {                                             // if FT847 is powered OFF ...
                        if (currentTemperature < TEMPERATURE_HYSTERESIS_BOTTOM) {       //   and the box has cooled 4 degrees ...
                            heatWaitPowerOff = 0;                                       //   clear flag and power the FT847 back up.
                            if ( powerOnOffFT847(1) ) { terminate = 1; break; }         // Power On - assume gpio23 is already set up
                            //system("echo \"1\" > /sys/class/gpio/gpio23/value");        // Power On - assume gpio23 is already set up
                            if (updateFiles("HeatWait")) { retval = -1; }               // Insert another HeatWait message into log
                            sendUDPEmailMsg( "Box below 86 deg, radio on\nBox temperature below 86 degrees, radio powered on\n" );
                        }
                    } else {                                                            // if FT847 is powered ON
                        if (currentTemperature > TEMPERATURE_POWER_OFF) {               //    and the box is above 90 degrees
                            heatWaitPowerOff = 1;                                       //    set flag and power the FT847 off
                            if ( powerOnOffFT847(0) ) { terminate = 1; break; }         //  Power OFF
                            //system("echo \"0\" > /sys/class/gpio/gpio23/value");        //  Power OFF
                            if (updateFiles("HeatOff ")) { retval = -1; }
                        } else {                                                        // if (85 < temperature < 90) AND (FT847 powered on).
                            if (heatWaitInLog == 0) {                                   //    just check if a log file entry needs to be made.
                                if (updateFiles("HeatWait")) { retval = -1; }
                                heatWaitInLog = 1;
                                //sendUDPEmailMsg( "Box above 85 deg, beacons halted\nBox temperature above 85 deg, beacons halted.\n" );
                            }
                        }
                    }
                    // print message on the screen
                    if (heatWaitPowerOff) {
                        printf("\rTemperature too high: %3.3lf F.  Waiting two minutes (%d min) with power OFF",currentTemperature,heatWait);
                        fprintf(dupFile,"\rTemperature too high: %3.3lf F.  Waiting two minutes (%d min) with power OFF",currentTemperature,heatWait);
                    } else {
                        printf("\rTemperature too high: %3.3lf F.  Waiting two minutes (%d min)               ",currentTemperature,heatWait);
                        fprintf(dupFile,"\rTemperature too high: %3.3lf F.  Waiting two minutes (%d min)               ",currentTemperature,heatWait);
                    }
                    fflush(stdout);   fflush(dupFile);
                    // wait two minutes.
                    for (int kkk = 0; kkk < 120; kkk++) {
                        sleep(1);
                        if (terminate) {
                            break;
                        }
                    }
                    heatWait += 2;
                }
            }
            if (heatWait > 0) {
                printf("\n");
                fprintf(dupFile,"\n");
            }

            //
            //  Send beacons (the beacon block)
            //
            while (terminate == 0) {
                //  Quit if done.  All unused beaconData[] entries are zero and are all at end of array so quit on first zero.
                if (beaconData[ beaconCounter ].txFreqHz == 0) {
                    break;
                }

                //  Send beacon.  Fill in timestamp
                if (txWspr(rx0FreqHz, &beaconData[beaconCounter])) {
                    retval = -1;
                    break;
                }

                minWait -= 4;
                beaconWasSent = 1;
                beaconCounter++;
            }

            //
            // if a beacon was sent then wait two minutes and collect data from WSPRNet.org
            //
            if ( beaconWasSent ) {
                if (waitForTopOfEvenMinute( 0 )) {                                                  // ... wait for two more minutes
                    retval = -1;
                    break;
                }
                if (doCurl( beaconData, termPTSNum )) {         // ... and get results from wsprnet.org
                    retval = -1;
                    break;
                }
                minWait -= 2;
            }
            minCounter = 0;
            printf("\n");
            fprintf(dupFile,"\n\n");
        }
    }

    if (updateFiles("Shutdown")) { retval = -1; }
    ft847_close();
    terminatePortAudio();
    closeNetwork();
    fclose(dupFile);
    printf("\n");
    return retval;
}


//  This function is called as sleep ends.  It waits for the top of second and then initiates a send.
static int txWspr( int rxFreq, struct BeaconData *beaconData ) { // txFreq, char* timestamp ) {
    int iii;
    char string[16];
    struct tm *info;
    time_t rawtime;         // time_t is long integer
    int txFreq = beaconData->txFreqHz;
    double dtemperature = getTempData();

    //  The FT847 tends down in freq as the temperature goes up and vice versa.  Compensate.
    if (txFreq > 144000000) {
        if ( dtemperature < 75.0 ) {                // 2m beacon won't be used if temperature > 70 deg.  It used to be 75 but dropped it.
            if (dtemperature > 74.0) { txFreq = 144489250; }
            else if (dtemperature > 72.5) { txFreq = 144489220; }
            else if (dtemperature > 71.0) { txFreq = 144489210; }
            else if (dtemperature > 70.0) { txFreq = 144489200; }
            else if (dtemperature > 68.5) { txFreq = 144489170; }
            else if (dtemperature > 67.5) { txFreq = 144489140; }
            else if (dtemperature > 66.5) { txFreq = 144489130; }
            else if (dtemperature > 64.8) { txFreq = 144489120; }
            else if (dtemperature > 61.0) { txFreq = 144489110; }
            else if (dtemperature > 59.4) { txFreq = 144489100; }
            else if (dtemperature > 57.5) { txFreq = 144489090; }
            else if (dtemperature > 56.3) { txFreq = 144489080; }
            else if (dtemperature > 55.3) { txFreq = 144489070; }
            else if (dtemperature > 54.1) { txFreq = 144489060; }
            else if (dtemperature > 53.1) { txFreq = 144489050; }
            else if (dtemperature > 52.1) { txFreq = 144489040; }
            else if (dtemperature > 50.6) { txFreq = 144489030; }
            else if (dtemperature > 50.0) { txFreq = 144489020; }
            else if (dtemperature > 46.6) { txFreq = 144489010; }
            else if (dtemperature > 45.3) { txFreq = 144489020; }    // below 45.3 it seems to go up again.
            else { txFreq = 144489030; }                             // No data belo 44.3 deg
        }
    } else if (txFreq > 50000000) {                 // set gain based on freq: 0.8 for 2m, 0.6 for 6m and 1.0 for 10m/15m, an FT847 quirk.
        double dtemperature = getTempData();
        if ( dtemperature < 90.0 ) {
            if (dtemperature > 84.0) { txFreq = 50293160; }         // 86 deg 50293.160 puts the beacon in the middle of the 200 Hz WSPR passband
            else if (dtemperature > 81.0) { txFreq = 50293130; }    // 82 deg 50293.130
            else if (dtemperature > 78.0) { txFreq = 50293100; }
            else if (dtemperature > 77.0) { txFreq = 50293090; }
            else if (dtemperature > 75.0) { txFreq = 50293080; }
            else if (dtemperature > 73.5) { txFreq = 50293070; }
            else if (dtemperature > 70.5) { txFreq = 50293060; }
            else if (dtemperature > 67.5) { txFreq = 50293050; }
            else if (dtemperature > 65.0) { txFreq = 50293030; }
            else if (dtemperature > 56.0) { txFreq = 50293020; }
            else if (dtemperature > 51.0) { txFreq = 50293010; }
            else if (dtemperature > 45.2) { txFreq = 50293010; }
            else { txFreq = 50293020; }                             // data below 45.2 seems to go up again, no data below 44.15
        }
    } else if (txFreq > 28000000) {
        double dtemperature = getTempData();
        if ( dtemperature < 90.0 ) {
            if (dtemperature > 85.0) { txFreq = 28124700; }
            else if (dtemperature > 82.0) { txFreq = 28124690; }
            else if (dtemperature > 80.0) { txFreq = 28124680; }
            else if (dtemperature > 79.0) { txFreq = 28124670; }
            else if (dtemperature > 76.0) { txFreq = 28124660; }
            else if (dtemperature > 73.0) { txFreq = 28124650; }
            else if (dtemperature > 68.0) { txFreq = 28124640; }
            else if (dtemperature > 60.0) { txFreq = 28124630; }
            else if (dtemperature > 54.5) { txFreq = 28124620; }
            else { txFreq = 28124610; }                             // as of this writing 45.9 deg is the lowest temperature for which I have data
        }
    } else if (txFreq > 24000000) {
        double dtemperature = getTempData();
        if ( dtemperature < 90.0 ) {
            if (dtemperature > 85.0) { txFreq = 24924690; }
            else if (dtemperature > 82.0) { txFreq = 24924680; }
            else if (dtemperature > 81.0) { txFreq = 24924670; }
            else if (dtemperature > 78.9) { txFreq = 24924660; }
            else if (dtemperature > 77.2) { txFreq = 24924650; }
            else if (dtemperature > 72.5) { txFreq = 24924640; }
            else if (dtemperature > 66.0) { txFreq = 24924630; }
            else if (dtemperature > 57.0) { txFreq = 24924620; }
            else { txFreq = 24924610; }                             // as of this writing 44.7 deg is the lowest temperature for which I have data
        }
    }
    beaconData->txFreqHzActual = txFreq;
    beaconData->temperature = dtemperature;
    strcpy( beaconData->tone, getWavFilename(txFreq) );

    if (waitForTopOfEvenMinute( txFreq )) {
        return 1;
    }

    //  Put radio in Tx mode and put SDRPlay into Tx mode
    if (ft847_FETMOXOn()) { return 1; }
    if (sendUDPMsg( 1 )) { return 1; }

    time( &rawtime );
    info = gmtime( &rawtime );      // UTC
    sprintf(beaconData->timestamp,"%02d:%02d",info->tm_hour, info->tm_min);
    printf("Beacon freq %d Hz at %s:%02d UTC          \n", txFreq, beaconData->timestamp, info->tm_sec);
    fprintf(dupFile,"Beacon freq %d Hz at %s:%02d UTC          \n", txFreq, beaconData->timestamp, info->tm_sec);
    iii = sendWSPRData( beaconData->tone, dupFile );
    if (iii) {
        printf("Error on sendWSPRData()\n");
    }

    //  Update files with a burst message based on the frequency
    if (txFreq > 144000000) {
        strcpy( string, "Burst2m " );
    } else if (txFreq > 50000000) {
        strcpy( string, "Burst6m " );
    } else if (txFreq > 28000000) {
        strcpy( string, "Burst10m" );
    } else if (txFreq > 24000000) {
        strcpy( string, "Burst12m" );
    } else {
        strcpy( string, "Burst15m" );
    }
    if (updateFiles(string)) { return 1; }

    //  Take radio and SDRPlay out of Tx mode
    if (sendUDPMsg( 0 )) { return 1; }
    if (ft847_FETMOXOff()) { return 1; }

    // set radio back to receive frequency
    usleep(1500000);                        // sleep for 1.5 seconds in case this function is called again.  Need waitForTopOfEvenMinute() to progress past sec == 0
    if (ft847_writeFreqHz( rxFreq )) {
        return 1;
    }
    return iii;
}


//  This returns the name of the wav file to send.  It starts at 1470 Hz and works its way up to 1530 Hz.
static char *getWavFilename( int txFreq ) {
    static char returnValue[10];
    static int tone = 1470;

    //  If 2m or 6m then fix tone.  Otherwise rotate through options.
    if (txFreq > 50000000) {
        strcpy(returnValue,"1500.wav");
    } else {
        tone += 10;
        if (tone > 1530) {
            tone = 1470;
        }
        sprintf(returnValue,"%d.wav",tone);
    }

    return returnValue;
}


//  This function delays until the top of an even second, at which point it will return.  Three seconds before the top of
//      even second it will set the radio to txFreq unless txFreq == 0.  Once that is done it will return in three seconds.
//      Prior to that, and if txFreq != 0, the user can hit enter to suspend loop.  Hit enter again to resume.
//      Also, prior to that and again if txFreq != 0, if a UDP message arrives indicating that the FT847 is transmitting then
//      it will wait 60 seconds before continuing looking for top-of-even-minute.
//  Later I added code to check blackout.txt in order to see if it needs to delay things for a satellite pass.
//  Later I modified it to check if ENTER key is pressed and halt countdown if so.
//  Later checked for a UDP message indicating transmit.  If received it delays things for one minute.  Specifically, with each
//      transmit UDP message it resets delayUDPTimer (local variable) to 60.
static int waitForTopOfEvenMinute( int txFreq ) {
    /*  struct tm {
            int tm_sec;         // seconds
            int tm_min;         // minutes
            int tm_hour;        // hours
            int tm_mday;        // day of the month
            int tm_mon;         // month
            int tm_year;        // year
            int tm_wday;        // day of the week
            int tm_yday;        // day in the year
            int tm_isdst;       // daylight saving time
        }   */
    struct tm *info;
    time_t rawtime;         // time_t is long integer
    int curSec = 0;          // debug for display
    int returnValue = 0;
    int freqChangeDone = 0;     // flag
    int NumBytesIn;
    int delayUDPTimer = 0;

    doBlackout();

    //   loop until top of minute
    printf("\nWaiting for top of even minute: ");  fflush( (FILE *)NULL );
    fprintf(dupFile,"\nWaiting for top of even minute: ");  fflush( (FILE *)dupFile );
    while (1) {
        time( &rawtime );                   // rawtime is the number of seconds in the epoch (1/1/1970).  time() also returns the same value.
        info = localtime( &rawtime );       // info is the structure giving seconds and minutes

        //  This is the usual exit from loop and from function
        if (delayUDPTimer == 0) {                   // if not delayed due to UDP message indicating transmit.  delayUDPTimer will be zero if txFreq == 0.
            if ((freqChangeDone) || (txFreq==0)) {  // ... and already changed frequency at 57 seconds before top of even minute OR if txFreq == 0 meaning no freq change
                if (info->tm_sec == 0) {            // ... and now top of minute
                    int isOdd = info->tm_min % 2;   // ... and this is an even minute
                    if (!isOdd) {                   // ... then break with returnValue == 0 (no error)
                        break;
                    }
                }
            }
        }

        //  If three seconds before the top of even second set the radio to txFreq (unless txFreq == 0)
        if ( (!freqChangeDone) && (delayUDPTimer == 0) ) {      // check that it's not already been done and no delay timer running.
            if (txFreq) {                                       // if txFreq != 0
                if (info->tm_sec == 57) {                       // if 57 second
                    int isOdd = info->tm_min % 2;               // ... and this is an odd minute
                    if (isOdd) {                                // ... write freq change
                        if (ft847_writeFreqHz( txFreq )) {      // set radio to transmit frequency
                            returnValue = 1;    // if error
                            break;
                        }
                        freqChangeDone = 1;
                    }
                }
            }
        }

        if (terminate) {                    // if signal caught.
            returnValue = 1;
            break;
        }

        //  Display
        if (curSec != info->tm_sec) {
            curSec = info->tm_sec;
            if (delayUDPTimer) {
                delayUDPTimer--;
                printf("\rOne minute delay for transmission: %02d       ",delayUDPTimer);  fflush( (FILE *)NULL );
                fprintf(dupFile,"\rOne minute delay for transmission: %02d       ",delayUDPTimer);  fflush( (FILE *)dupFile );
            } else {
                printf("\rWaiting for top of even minute: %02d %02d     ",info->tm_min,curSec);  fflush( (FILE *)NULL );
                fprintf(dupFile,"\rWaiting for top of even minute: %02d %02d     ",info->tm_min,curSec);  fflush( (FILE *)dupFile );
            }
        }

        //  Check for ENTER or for UDP message
        if ((txFreq) && (!freqChangeDone)) {      // don't allow ENTER key or UDP message to stop beacon if frequency has already been changed or if txFreq == 0

            //  Check for ENTER key to suspend
            ioctl(0,TIOCINQ,&NumBytesIn);
            if (NumBytesIn > 0) {
                while ( NumBytesIn > 0 ) {  //  Swallow ENTER and everything before it.
                    getchar();
                    NumBytesIn--;
                }
                printf("\r Press ENTER to resume.                     ");     fflush( (FILE *)NULL );
                getchar();
            }

            //  Check for data on network port
            ioctl(sockRx,TIOCINQ,&NumBytesIn);
            if (NumBytesIn > 0) {
                unsigned int len_inet;
                int iii;
                unsigned char dgram[512];              // receive buffer

                len_inet = sizeof(adr_clnt4);
                iii = recvfrom(sockRx,
                        dgram,          // receive buffer
                        sizeof(dgram),  // max length (a constant her would be better).
                        0, //MSG_DONTWAIT,
                        (struct sockaddr *)&adr_clnt4,   // filled in by function
                        &len_inet
                        );
                if (iii < 0) {
                    printf("UDP receive socket - recvfrom() failed\n");
                    continue;
                }
                if (iii > 0) {
                    dgram[iii] = 0;
                    if (strcmp(UDP_TX_MESSAGE,(char *)dgram) == 0) {
                        delayUDPTimer = 60;
                    }
                    //printf("%s %d\n",(char *)dgram,delayUDPTimer);
                }
            }
        }

        usleep(10000);
    }

    //  Convenient place to turn screen off at midnight and on at 6:00.
    /*
    if (info->tm_hour < 6) {
        ft891_screenBrightness( 0 );
    } else {
        ft891_screenBrightness( 1 );
    }
    */

    printf("\r");
    fprintf(dupFile,"\r");
    //printf("Current local time and date: %ld %d %d %d   %s ", rawtime, info->tm_hour, info->tm_min, info->tm_sec, asctime(info));
    return returnValue;
}


//  Handles all the logging events.  It is meant to be self-contained.  That is, fptr is openned and closed here.
static int updateFiles( char *eventName ) {
    time_t rawtime;
    struct tm *info;
    char timestamp[64];
    double currentTemperature;

    static FILE* logFile;       // always open for append
    const char* LOG_FILENAME = "log_twspr.txt";

    time( &rawtime );   // rawtime is of time_t, number of seconds since the epoch
    info = localtime( &rawtime );
    strftime( timestamp, 64, "%a %b %d %Y %H:%M:%S", info );

    logFile = (FILE *)fopen(LOG_FILENAME,"at");
    if (logFile == (FILE *)NULL) {
        printf("Unable to open %s for append.\n",LOG_FILENAME);
        return 1;
    }
    currentTemperature = getTempData();
    fprintf( logFile, "%s  %ld   %s  %3.3lf F\n", eventName, rawtime, timestamp, currentTemperature);
    fclose(logFile);
    return 0;
}


//  Read config file, get data, and close it again.  That way the file can be minipulated between bursts.
//      If rxFreq == 0 then return error and let the program quit.
//      If any of the frequencies are not correct return error.
//          rx0FreqHz not within 1.8 MHz - 450 MHz
//          tx1FreqHz, tx2FreqHz, tx3FreqHz, or tx4FreqHz not a frequency (not all numbers).  If not WSPR freq
//              then the frequency will be zero and no beacon will take place but no error returned.
static int readConfigFile( int *rxFreqHz, struct BeaconData *beaconData ) {
    FILE *fptr;
    char *cc, string[64];
    int convResult = 0;
    int numBeacons = 0;

    //  If file is missing then it is not an error.  Just continue to use the current values.
    //  If file is present then new values will be assigned to all parameters.  However it is only an error if rxFreqHz is zero.
    //  If txFreqHz are present then the values must match WSPR values.  Anything else or any error on conversion will cause
    //      beaconData->txFreqHz to be set to zero but no error will be returned.
    //
    //      The format of the lines is:
    //          rxFreqHz xxxxxxxx
    //          txFreqHz xxxxxxxx
    //          txFreqHz xxxxxxxx
    //          txFreqHz xxxxxxxx
    //          txFreqHz xxxxxxxx
    //      The tokens (rxFreqHz or txFreqHz) must start on the first character of the line.
    //      The frequency must be in Hz and can be be as short as 7 digits (<10 MHz) or as long as 10 digits (144 or 432 MHz)
    //      Lines without this format can be present but will be ignored.

    fptr = fopen(CONFIG_FILENAME,"rt");
    if (fptr == (FILE *)NULL) {
        return 0;           // no error
    }

    *rxFreqHz = 0;
    for (int iii = 0; iii < MAX_NUMBER_OF_BEACONS; iii++ ) {        // no going back.  Must get something assigned.
        beaconData[iii].txFreqHz = 0;
        beaconData[iii].timestamp[0] = 0;
        beaconData[iii].tone[0] = 0;
        beaconData[iii].txFreqHzActual = 0;
        beaconData[iii].temperature = 0.0;
    }

    while (!feof(fptr)) {
        if (numBeacons == MAX_NUMBER_OF_BEACONS) {
            break;
        }

        cc = fgets( string, 64, fptr );         // read one line of the file
        if (cc == (char *)NULL) {
            break;
        }
        if (strlen(string) < 16) {      // token (rx/tx1/tx2/tx3/tx4FreqHz) always 8 characters + at least one space + at least 7 characters for the freq.
            continue;
        }
        string[8] = 0;    // null terminate right after the token
        if (!strcmp(string,"rxFreqHz")) {
            int convResult = readConfigFileHelp( &string[10] );
            if ( convResult < 0) {  break;  }       // error - quit
            if ((convResult >= 1800000) && (convResult <= 450000000)) {
                *rxFreqHz = convResult;
            }
        } else if (!strcmp(string,"txFreqHz")) {
            int convResult = readConfigFileHelp( &string[10] );
            if ( convResult < 0) {  continue;  }    //  error - read the next line, if any.
            if ( readConfigFileWSPRFreq( convResult ) == 0 ) {
                // Special consideration for 2m beacon, skip it if temperature > 70 deg.
                double currentTemperature = getTempData();
                if ((convResult < 144000000) || (currentTemperature < 70.0)) {
                    beaconData[numBeacons++].txFreqHz = convResult;
                }
            }
        }
    }

    clearerr(fptr);
    fclose(fptr);
    printf("\n\nNumber of beacons %d\n",numBeacons);
    fprintf(dupFile,"\n\nNumber of beacons %d\n",numBeacons);
    if (*rxFreqHz == 0) {
        return -1;
    } else if (convResult < 0) {
        return -1;
    } else {
        return 0;
    }
}


//  Verify that the frequency is a WSPR frequency.  Returns 0 if so and -1 if not.  Note that I'm not checking for one freq on each band.  So
//      there is noting to stop me from sending a WSPR message on the same frequency several times in a row.
int readConfigFileWSPRFreq( int convResult ) {
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_30M ) == 0 ) { return 0; }
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_17M ) == 0 ) { return 0; }
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_15M ) == 0 ) { return 0; }
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_12M ) == 0 ) { return 0; }
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_10M ) == 0 ) { return 0; }
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_6M ) == 0 ) { return 0; }
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_2M ) == 0 ) { return 0; }
    return -1;
}


static int readConfigFileWSPRFreqHelp( int convResult, int WSPRFreq ) {
    int minFreq = WSPRFreq - 1000;           //  1 kHz below the designated frequency
    int maxFreq = WSPRFreq + 1000;           //  1 kHz above (the WSPR passband is 200 Hz wide)
    if ( (convResult < minFreq) || (convResult > maxFreq) ) {
        return -1;
    }
    return 0;
}


//  Called from readConfigFile() when the token (rx0/tx1/tx2/tx3/tx4FreqHz) is correct.  It attempts to extract the frequency.  It returns the
//      frequency or -1 on error.  If error then the calling routine will go to the next line.
static int readConfigFileHelp( char *string ) {
    int iii;
    int returnValue;

    //  walk past any leading spaces.  If all spaces or if null string then return error.
    for (iii = 0; iii < strlen(string); iii++) {
        if (string[iii] != ' ') {
            break;
        }
    }
    if (iii == strlen(string)) {
        return -1;
    }

    //  iii should point to the first non-space.  Walk through the string until the end, newline, or next space and make sure every character is a number.
    while (iii < strlen(string)) {
        if ((string[iii] >= '0') && (string[iii] <= '9')) {
            iii++;
            continue;
        }
        else if ((string[iii] == '\n') || (string[iii] == ' ')) {
            break;
        } else {
            return -1;
        }
    }

    //  convert substring into integer.
    string[iii] = 0;
    iii = sscanf( string, "%d", &returnValue );
    if (iii != 1) {
        return -1;      //  error on conversion
    }
    return returnValue;
}


//  int findttyUSB( void ) - scans /dev directory looking for ttyUSBFT847.  If found and it points to ttyUSB0 or ttyUSB1 or ttyUSB2 then return 0.
//      return 1 if it points to anything else and -1 on error.
static int findttyUSB( void ) {
    char directory[] = "/dev";
    DIR *dp;
    struct dirent *entry;
    struct stat statbuf;
    int return_value = 0;

    printf("\n");

    //  start by getting the current directory so when function is over can return to it.
    char currentDirectory[1024];
    char *ccc = getcwd(currentDirectory,sizeof(currentDirectory));
    if (ccc == (char *)NULL) {
        printf("cannot get current directory");
        return -1;
    }

    //  opendir() returns a DIR* pointer, used in readdir() below.
    if((dp = opendir(directory)) == NULL) {
        printf("cannot open directory: %s\n", directory);
        return -1;
    }

    //  change to the working directory in the string directory
    chdir(directory);

    /*  readdir() returns a dirent structure for the directory pointed to by DIR *dp
            struct dirent {
                ino_t          d_ino;       // inode number
                off_t          d_off;       // offset to the next dirent
                unsigned short d_reclen;    // length of this record
                unsigned char  d_type;      // type of file; not supported by all file system types
                char           d_name[256]; // filename
            };
    */
    while((entry = readdir(dp)) != NULL) {
        /*  lstat() is passed a stat structure (second parameter).  lstat() fills in, giving information about the filename (first parameter).
                Here this is only used to determine if the entry is a directory.
                    struct stat {
                        dev_t     st_dev;     // ID of device containing file
                        ino_t     st_ino;     // inode number
                        mode_t    st_mode;    // protection
                        nlink_t   st_nlink;   // number of hard links
                        uid_t     st_uid;     // user ID of owner
                        gid_t     st_gid;     // group ID of owner
                        dev_t     st_rdev;    // device ID (if special file)
                        off_t     st_size;    // total size, in bytes
                        blksize_t st_blksize; // blocksize for file system I/O
                        blkcnt_t  st_blocks;  // number of 512B blocks allocated
                        time_t    st_atime;   // time of last access
                        time_t    st_mtime;   // time of last modification
                        time_t    st_ctime;   // time of last status change
                    };
        */
        lstat(entry->d_name,&statbuf);

        // if link ...
        if(S_ISLNK(statbuf.st_mode)) {                      // S_ISLNK() is one of several macros to determine the nature of the file based on st_mode
            // if link is named ttyUSBFT847 ....
            if (strcmp("ttyUSBFT847",entry->d_name) == 0) {
                char string[1024];
                ssize_t iii;

                //  readlink() takes the name of the link (1st parameter) and writes it to string[] (2nd parameter).  The max number of bytes is 3rd parameter.
                iii = readlink(entry->d_name,string,sizeof(string));
                if (iii <= 0) {         // readlink() number of bytes written to string[], -1 on error.
                    printf("error1");
                    return_value = -1;
                    break;
                }
                string[iii] = 0; // null terminate
                if ((strcmp("ttyUSB0",string) == 0) || (strcmp("ttyUSB1",string) == 0) || (strcmp("ttyUSB2",string) == 0)) {
                    printf("%s -> %s\n",entry->d_name,string);
                    break;      // return_value is already set to 0
                } else {
                    printf("%s -> %s\n",entry->d_name,string);
                    return_value = 1;
                    break;
                }
            }
        }
    }

    //  move up one level in directory structure before returning.
    chdir(currentDirectory);
    closedir(dp);
    return return_value;
}


static int installSignalHandlers( int useMyHandlers ) {
    struct sigaction psa;           // it appears that signal() is deprecated.  This is the preferred way.  Code from https://www.gnu.org/software/libc/manual/html_node/Sigaction-Function-Example.html

    if (useMyHandlers) {
        psa.sa_handler = SignalHandler;
    } else {
        psa.sa_handler = SIG_DFL;       //  turn signal handlers off (unused here)
    }
    sigemptyset (&psa.sa_mask);
    psa.sa_flags = 0;

    if ( sigaction( SIGUSR1, &psa, NULL ) == -1 )  {   //  signal 10
      printf("\nError installing signal handler.\n");
      return 1;
    }
    if ( sigaction( SIGQUIT, &psa, NULL ) == -1 )  {   //  signal 3
      printf("\nError installing signal handler.\n");
      return 1;
    }
    if ( sigaction( SIGINT, &psa, NULL ) == -1 )  {    //  CTRL-C, signal 2
      printf("\nError installing signal handler.\n");
      return 1;
    }
    return 0;
}


static int initializeNetwork( void ) {
    char *srvr_addr = "192.168.1.196";          // sdrsharp, indicating transition from Tx to Rx
    char *srvr_addr2 = "192.168.1.111";         // sdruno, indicating transition from Tx to Rx
    char *srvr_addr3 = "192.168.1.238";         // email
    struct sockaddr_in adr_inet4;               // for receiving messages, only used here.

    sock = socket(AF_INET,SOCK_DGRAM,0);
    if (sock == -1) {
        printf("Unable to initialize socket.\n");
        return -1;
    }

    //  for UDP message to preamp.py
    memset(&adr_inet,0,sizeof adr_inet);                    //  Initialize the structure to zero
    adr_inet.sin_family = AF_INET;                          //  Type to internet.
    adr_inet.sin_port = htons(9083);                        //  set port
    adr_inet.sin_addr.s_addr = inet_addr(srvr_addr);        //  Convert address string to correct binary format and store in structure.
    if ( adr_inet.sin_addr.s_addr == INADDR_NONE ) {        //  If inet_addr() failed, likely because address wrong.
        printf("Cannot install address\n");
        return -1;
    }
    SockAddrStructureSize = sizeof(adr_inet);

    //  for UDP message to SDRPlay
    memset(&adr_inet2,0,sizeof adr_inet2);                  //  Initialize the structure to zero
    adr_inet2.sin_family = AF_INET;                         //  Type to internet.
    adr_inet2.sin_port = htons(9090);                       //  set port
    adr_inet2.sin_addr.s_addr = inet_addr(srvr_addr2);      //  Convert address string to correct binary format and store in structure.
    if ( adr_inet2.sin_addr.s_addr == INADDR_NONE ) {       //  If inet_addr() failed, likely because address wrong.
        printf("Cannot install address\n");
        return -1;
    }
    SockAddrStructureSize2 = sizeof(adr_inet2);

    //  for UDP message to send Email
    memset(&adr_inet3,0,sizeof adr_inet3);                  //  Initialize the structure to zero
    adr_inet3.sin_family = AF_INET;                         //  Type to internet.
    adr_inet3.sin_port = htons(5523);                       //  set port
    adr_inet3.sin_addr.s_addr = inet_addr(srvr_addr3);      //  Convert address string to correct binary format and store in structure.
    if ( adr_inet3.sin_addr.s_addr == INADDR_NONE ) {       //  If inet_addr() failed, likely because address wrong.
        printf("Cannot install address\n");
        return -1;
    }
    SockAddrStructureSize3 = sizeof(adr_inet3);

    //  for receiving UDP messages
    sockRx = socket(AF_INET,SOCK_DGRAM,0);
    if (-1 == sockRx) {
        printf("Cannot create UDP receive socket\n");
        return -1;
    }
    memset(&adr_inet4,0,sizeof(adr_inet4));
    adr_inet4.sin_family = AF_INET;
    adr_inet4.sin_port = htons(9090);                        // host to network conversion of short (16 bit) value
    adr_inet4.sin_addr.s_addr = inet_addr(myIP);  // convert string to network byte order address and store in s_addr
    if (adr_inet4.sin_addr.s_addr == INADDR_NONE) {
        printf("UDP receive socket - adr_inet4.sin_addr.s_addr == INADDR_NONE\n");
        return -1;
    }
    if (-1 == bind(sockRx,(struct sockaddr *)&adr_inet4,sizeof(adr_inet4))) {   // bind this address to this socket
        printf("UDP receive socket - bind() failed\n");
        return -1;
    }

    return 0;
}


static void closeNetwork( void ) {
    close(sockRx);
    close(sock);
}


static int sendUDPMsg( int doingTx ) {
    int zzz;
    char dgram[16] = "rxMode;";
    int sizeOfPacket;

    //  Message to SDRPlay
    if (doingTx) {
        dgram[0] = 't';
    }
    sizeOfPacket = strlen(dgram);
    zzz = sendto(sock, dgram, (size_t)sizeOfPacket, 0, (struct sockaddr *)&adr_inet2, SockAddrStructureSize2);
    if (zzz != sizeOfPacket) {
      printf("sendUDPMsg() - Error in sendto() %d %d\n",zzz,sizeof(dgram));
      return -1;
    }

    //  Message to preamp.py (probably not necessary to send it twice)
    if (doingTx) {
        strcpy(dgram,"preampOff;");
    }
    else {
        strcpy(dgram,"preampOn;");
    }
    sizeOfPacket = strlen(dgram);
    zzz = sendto(sock, dgram, (size_t)sizeOfPacket, 0, (struct sockaddr *)&adr_inet, SockAddrStructureSize);
    if (zzz != sizeOfPacket) {
      printf("sendUDPMsg() - Error2 in sendto() %d %d\n",zzz,sizeof(dgram));
      return -1;
    }
    usleep(200000);
    zzz = sendto(sock, dgram, (size_t)sizeOfPacket, 0, (struct sockaddr *)&adr_inet, SockAddrStructureSize);
    if (zzz != sizeOfPacket) {
      printf("sendUDPMsg() - Error2 in sendto() %d %d\n",zzz,sizeof(dgram));
      return -1;
    }

    return 0;
}



int sendUDPEmailMsg( char *message ) {
    int zzz;
    int sizeOfPacket;

    //  Message to send Email
    sizeOfPacket = strlen(message);
    zzz = sendto(sock, message, (size_t)sizeOfPacket, 0, (struct sockaddr *)&adr_inet3, SockAddrStructureSize3);
    if (zzz != sizeOfPacket) {
      printf("sendUDPMsg() - Error in sendto() %d %d\n",zzz,sizeof(message));
      return -1;
    }

    return 0;
}


static int getMyIPAddress( char *myIPAddress ){

    //  This is my favorite method.  It requires no knowledge of the interface.  Since it's SOCK_DGRAM it doesn't make a TCP connection and
    //      the IP 192.168.1.146 does not even have to exist.  It also works with 8.8.8.8.

    static int socktemp;                  // socket for receiving data from UDPRepeater4.py
    struct sockaddr_in inet_adr;          // for receiving messages, only used here.
    int iii;
    socklen_t structlength;               // unsigned int

    /* The equivalent Python code
    sss = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sss.connect(("192.168.1.146", 80))
    myIP = sss.getsockname()[0]
    sss.close() */

    socktemp = socket(AF_INET,SOCK_DGRAM,0);
    if (-1 == socktemp) {
        printf("getMyIPAddress() - cannot create temp socket\n");
        return -1;
    }

    memset(&inet_adr,0,sizeof(inet_adr));
    inet_adr.sin_family = AF_INET;
    inet_adr.sin_port = htons(9090);                        // host to network conversion of short (16 bit) value
    inet_adr.sin_addr.s_addr = inet_addr("192.168.1.146");  // convert string to network byte order address and store in s_addr
    if (inet_adr.sin_addr.s_addr == INADDR_NONE) {
        printf("getMyIPAddress() - inet_adr.sin_addr.s_addr == INADDR_NONE\n");
        return -1;
    }

    iii = connect( socktemp, (struct sockaddr *)&inet_adr, sizeof(inet_adr) );
    if (-1 == iii) {
        printf("getMyIPAddress() - Error in connect\n");
        return -1;
    }
    iii = getsockname( socktemp, (struct sockaddr *)&inet_adr, &structlength );
    if (-1 == iii) {
        printf("getMyIPAddress() - Error in getsockname\n");
        return -1;
    }
    //printf("length %d\n",structlength);
    {
        struct sockaddr_in* pV4Addr = (struct sockaddr_in*)&inet_adr;
        struct in_addr ipAddr = pV4Addr->sin_addr;
        char string[INET_ADDRSTRLEN];
        inet_ntop( AF_INET, &ipAddr, string, INET_ADDRSTRLEN );
        strcpy( myIPAddress, string );
    }

    close(socktemp);
    return(0);
}


//
//  blackoutUpdateFile() -  removes one line from blackout.txt.  The line is contained in the global variable lineToRemove[].  Returns -1 on error or 0 otherwise.
//
static int blackoutUpdateFile( void ) {
    FILE *fptr;
    char *lines[256];
    char string[256];
    int iii, numLines = 0;

    //  Open file, read it into a buffer but skip lineToRemove[].
    fptr = fopen( BLACKOUT_FILENAME, "rt" );
    if (fptr == (FILE *)NULL) {
        printf("blackoutUpdateFile() - Cannot open %s file (fopen() #1)\n",BLACKOUT_FILENAME);
        return -1;
    }
    while (fgets(string, 256, fptr)){
        if (strstr( string, lineToRemove)) {        // strstr() returns non-null if lineToRemove[] is in string[] (they should match).
            continue;
        }
        int len = strlen(string)+1;                 // add 1 for null termination
        lines[ numLines ] = (char *)calloc( len, sizeof(char) );
        strcpy( lines[ numLines ], string );
        numLines++;
    }
    fclose(fptr);

    //  Open file and write the buffer back.
    fptr = fopen( BLACKOUT_FILENAME, "wt" );
    if (fptr == (FILE *)NULL) {
        printf("blackoutUpdateFile() - Cannot open %s file (fopen() #2)\n",BLACKOUT_FILENAME);
        return -1;
    }
    for (iii = 0; iii < numLines; iii++) {
        fprintf(fptr,"%s",lines[iii]);
    }
    fclose(fptr);

    //  deallocate buffer
    for (iii = 0; iii < numLines; iii++) {
        free( lines[iii] );
    }

    printf("Removed from %s the line %s\n",BLACKOUT_FILENAME,lineToRemove);
    return 0;
}



//
//    blackoutCheck() - Returns non-zero on error.  Otherwise it returns time_t blackoutEndTime, which is set to 0 if no blackout.
//
static int blackoutCheck( time_t *blackoutEndTime ) {
    FILE *fptr;
    struct tm tmStart, tmEnd;
    time_t epochStart, epochEnd, epochCurrent;
    int iii;
    char string[256];

    *blackoutEndTime = 0;               // default return value.  Calling routine will do nothing in this case.

    //
    //  Step 1 - read the file and interpret the results.  The output of this section is epochStart, epochEnd
    //

    fptr = fopen( BLACKOUT_FILENAME, "rt" );
    if (fptr == (FILE *)NULL) {
        printf("No blackout.txt file\n");
        return 0;       // not an error.
    }

    //  Loop through file, looking for the first non-comment.  Read into string[].
    while (1){
        char* ccc = fgets(string, 256, fptr);
        if (feof(fptr)) {
            fclose(fptr);
            return 0;
        }
        if (ccc == (char *)NULL) {
            printf("Error reading from %s\n", BLACKOUT_FILENAME);
            fclose(fptr);
            return -1;
        }
        if (string[0] != '#') {
            if (strlen(string) < 5) {       // a not so elegant way to avoid operating on blank lines.
                continue;
            } else {
                break;
            }
        }
    }
    fclose(fptr);

    //  Now read timestamps into tmStart and tmEnd.
    iii = sscanf(string, "%d-%d-%d %d:%d:%d, %d-%d-%d %d:%d:%d",
           &tmStart.tm_year, &tmStart.tm_mon, &tmStart.tm_mday, &tmStart.tm_hour, &tmStart.tm_min, &tmStart.tm_sec,
           &tmEnd.tm_year, &tmEnd.tm_mon, &tmEnd.tm_mday, &tmEnd.tm_hour, &tmEnd.tm_min, &tmEnd.tm_sec );
    if ( (iii != 12)
            || (tmStart.tm_mon < 1) || (tmStart.tm_mon > 12)  || (tmEnd.tm_mon < 1) || (tmEnd.tm_mon > 12)
            || (tmStart.tm_year < 2022) || (tmEnd.tm_year < 2022) || (tmStart.tm_year > 2024) || (tmEnd.tm_year > 2024)
       ) {
        printf("Error parsing from %s, line -  %s\n", BLACKOUT_FILENAME, string);
        fclose(fptr);
        return -1;
    }
    tmStart.tm_mon--;  tmEnd.tm_mon--;                  // months are numbered 0-11
    tmStart.tm_year -= 1900;  tmEnd.tm_year -= 1900;    // 2022 is 122 in tm_year
    tmStart.tm_isdst = tmEnd.tm_isdst = -1;             // tm_isdst cannot be left uninitialized.  A negative number tells mktime() to figure it out.

    //  store copy of line in case it later needs to be deleted.
    strcpy(lineToRemove,string);

    //  Get epoch time for each
    epochStart = mktime( &tmStart );
    epochEnd = mktime( &tmEnd );
    if (epochStart >= epochEnd) {
        char tempx[256];
        strcpy(tempx,string);
        tempx[ strlen(tempx)-1 ] = 0;    // remove LF at end
        printf("Error - start time after or same as end time %s, start %ld end %ld\n", tempx, epochStart, epochEnd);
        blackoutUpdateFile();
        return -1;
    }

    //
    //  Step 2 - get current time and compare
    //

    //  Get current time in time_t format (epoch)
    time( &epochCurrent );

    if (epochCurrent >= epochEnd) {         // if current time is past end time just delete the line
        printf("Blackout period over - current %ld end %ld\n", epochCurrent, epochEnd);
        blackoutUpdateFile();
        return 0;
    }

    //  If four minutes after current time is greater than start time then there is no time to do the next WSPR message.
    //      Set blackoutEndTime to the epochEnd value.
    //  If four minutes from now is less than start time then blackoutEndTime will remain at zero (set at top of function).
    if (epochCurrent+240 >= epochStart) {
        *blackoutEndTime = epochEnd;
    }

    return 0;
}


/*
    blackout.txt format example - "2022-11-02 17:55:00, 2022-11-02 17:55:02"

    The timestamp format is "%Y-%m-%d %H:%M:%S", for example "2022-10-02 16:46:00".
        The '-' and the ':' are required, with a space between date and time.
      Always local time, not UTC.
    The first timestamp is start time, then a comma, then stop time.
    The program check the first character of each line for a '#', indicating a
        comment.  It will then stop after reading the first timestamp set.  If the
        stop time has passed it will rewrite this file, eliminating that line.

*/

//
//  doBlackout() - handles all the blackout checking, file, line deletion, etc.  Calls the two functions above.
//
static int doBlackout( void ) {
    time_t blackoutEndTime;

    int iii = blackoutCheck( &blackoutEndTime );
    if (iii == 0) {
        if (blackoutEndTime) {
            time_t rawtime;

            printf("\n");
            while (1) {
                time( &rawtime );
                if (rawtime >= blackoutEndTime) {
                    break;
                }
                printf("Blackout for %ld sec - %ld, %ld\r",blackoutEndTime-rawtime,rawtime,blackoutEndTime);  fflush( stdout );
                sleep(1);
            }
            printf("Blackout complete - %ld, %ld        \n",rawtime,blackoutEndTime);
            blackoutUpdateFile();
        }
    }
    return iii;
}

/*
    On this particular day 26 stations heard me and 13 of them reported the exact same frequency.  I can assume they are GPS controlled.
    (The frequency should have been 21.096110 MHz).  I can capture these callsigns and highlight them on any band.  It would be interesting
    to see if they are always identical.  It should help me correct frequencies, both FT847 and SDR.


 2022-09-07 01:58    NQ6B    21.096137   -3    0   DM12qu    +37   5.012   N2HQI        FN13sa   3660    2274    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -13   0   DM12qu    +37   5.012   K1RA-BB    FM18cr   3536    2197    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -26   0   DM12qu    +37   5.012   AI6VN/KH6    BL10rx   4144    2575    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -16   0   DM12qu    +37   5.012   WA2TP        FN30lu   3936    2446    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -23   0   DM12qu    +37   5.012   K5KHK        FN13dc   3560    2212    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -20   0   DM12qu    +37   5.012   W3ENR        FM28jh   3762    2338    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -22   0   DM12qu    +37   5.012   KFS        CM87tj   727   452   WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -21   0   DM12qu    +37   5.012   KPH        CM88mc   818   508   WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -8    0   DM12qu    +37   5.012   KA7OEI-1     DN31uo   1045    649   WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -29   0   DM12qu    +37   5.012   KK6PR        CN94ik   1350    839   WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -21   0   DM12qu    +37   5.012   W4KEL        FM08qh   3465    2153    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -18   0   DM12qu    +37   5.012   WA2ZKD     FN13ed   3567    2216    WSPR-2
 2022-09-07 01:58    NQ6B    21.096137   -12   0   DM12qu    +37   5.012   K1RA-PI    FM18cr   3536    2197    WSPR-2

 Note that the other 13 stations report frequencies all over the place.

 2022-09-07 01:58    NQ6B    21.096149   -1    0   DM12qu    +37   5.012   WA3TTS    EN90xn    3346    2079    WSPR-2
 2022-09-07 01:58    NQ6B    21.096067   -17   0   DM12qu    +37   5.012   KX4O        FM18cp    3536    2197    WSPR-2
 2022-09-07 01:58    NQ6B    21.096141   +1    0   DM12qu    +37   5.012   N9AWU       EM69sm    2792    1735    WSPR-2
 2022-09-07 01:58    NQ6B    21.096058   -4    0   DM12qu    +37   5.012   N5FOG       EL29mh    2097    1303    WSPR-2
 2022-09-07 01:58    NQ6B    21.096139   -2    0   DM12qu    +37   5.012   PT2FHC    GH64cg    9081    5643    WSPR-2
 2022-09-07 01:58    NQ6B    21.096150   -17   0   DM12qu    +37   5.012   WD4ELG    FM06be    3367    2092    WSPR-2
 2022-09-07 01:58    NQ6B    21.096141   -14   0   DM12qu    +37   5.012   K1RA-4    FM18cr    3536    2197    WSPR-2
 2022-09-07 01:58    NQ6B    21.096064   -9    0   DM12qu    +37   5.012   WZ7I        FN20kk    3761    2337    WSPR-2
 2022-09-07 01:58    NQ6B    21.096128   -23   0   DM12qu    +37   5.012   KM3T        FN42et    4052    2518    WSPR-2
 2022-09-07 01:58    NQ6B    21.096133   -9    0   DM12qu    +37   5.012   WA2N        EM85rt    3131    1946    WSPR-2
 2022-09-07 01:58    NQ6B    21.096138   -11   0   DM12qu    +37   5.012   W1CLM       EN50uc    2645    1644    WSPR-2
 2022-09-07 01:58    NQ6B    21.096142   -4    0   DM12qu    +37   5.012   N2NOM       FN22bg    3703    2301    WSPR-2
 2022-09-07 01:58    NQ6B    21.096185   -14   0   DM12qu    +37   5.012   K1HTV-4   FM18ap    3522    2188    WSPR-2


 On another day these were the stations on 28 MHz.  Several of the stations in the first list above were in this list.  All but one
 agreed on a frequency of 28.126109 MHz.  Only N2HQI was off.  The freq should have been 28.126130, so I was 20 Hz low.  Anyhow,
 I can automatically correct my frequency by choosing callsigns which seem to have GPS control.

   19:38  28.126109  -17  0       KK6PR   CN94ik    839 mi  344 deg  ( 838 mi)
   19:38  28.126110  -25  0       W1CLM   EN50uc   1644 mi  064 deg  (1647 mi)
   19:38  28.126110   -6  0       KF9ET   EM69rd   1728 mi  067 deg  (1732 mi)
   19:38  28.126109   +7 -1      VA3MKM   EN58il   1778 mi  045 deg  (1780 mi)
   19:38  28.126110  -12  0     K1RA-PI   FM18cr   2197 mi  068 deg  (2202 mi)
   19:38  28.126109  -15  0         KPH   CM88mc    508 mi  317 deg  ( 508 mi)
   19:38  28.126109  +10  0        WV5L   EM74sc   1848 mi  078 deg  (1852 mi)
   19:38  28.126110  -18 -1       K4ZAD   FM05or   2156 mi  074 deg  (2160 mi)
   19:38  28.126110  -20  0       W7PUA   CN84jq    890 mi  338 deg  ( 890 mi)
   19:38  28.126109  -15  0       N3LSB   FM19kp   2232 mi  066 deg  (2236 mi)
   19:38  28.126110  +12  0        WA2N   EM85rt   1946 mi  074 deg  (1950 mi)
   19:38  28.126110  -19  0        K1VL   FN33om   2461 mi  060 deg  (2466 mi)
   19:38  28.126109  -20 -1       WA2TP   FN30lu   2446 mi  064 deg  (2451 mi)
   19:38  28.126109   +3  0     KX4AZ/T   EN74gc   1842 mi  056 deg  (1845 mi)
   19:38  28.126110  -25  0       KR6LA   CN90ao    611 mi  333 deg  ( 611 mi)
   19:38  28.126109  -18  0    KA7OEI-1   DN31uo    649 mi  020 deg  ( 649 mi)
   19:38  28.126110  -15  0       K5KHK   FN13dc   2212 mi  060 deg  (2216 mi)
   19:38  28.126109  -21  0      WA2ZKD   FN13ed   2216 mi  060 deg  (2221 mi)
   19:38  28.126109  +13  0        WC9P   EN52pf   1652 mi  059 deg  (1654 mi)
   19:38  28.126110  -17  0        N3PK   EM84ss   1957 mi  077 deg  (1961 mi)

   19:38  28.126119   +2  0        W3HH   EL89vb   2047 mi  088 deg  (2051 mi)
   19:38  28.126128   +7  0        NI5F   EM70fu   1823 mi  086 deg  (1827 mi)
   19:38  28.126119  -12  0       N2HQI   FN13sa   2274 mi  060 deg  (2279 mi)
   19:38  28.126102   +2  1        K0JD   EN62bv   1704 mi  058 deg  (1707 mi)
   19:38  28.126139  -17  0       KV4XY   EM75vu   1852 mi  074 deg  (1856 mi)
   19:38  28.126153  -18 -3      WA1RAJ   FN42dt   2514 mi  061 deg  (2519 mi)
   19:38  28.126118  -19  0       KD2OM   FN12gx   2224 mi  060 deg  (2228 mi)
   19:38  28.126122  -12  0        W3BX   FM19qc   2259 mi  067 deg  (2264 mi)
   19:38  28.126132  -21 -1      VE3EBR   FN25bi   2320 mi  056 deg  (2324 mi)
   19:38  28.126138  -23  0      VA3ROM   EN58jk   1780 mi  045 deg  (1782 mi)
   19:38  28.126171  -15  0       N8OBJ   EN91fh   2005 mi  063 deg  (2009 mi)
   19:38  28.126102  -15  0      K1RA-4   FM18cr   2197 mi  068 deg  (2202 mi)
   19:38  28.126143   -3  0      WA3NAN   FM19na   2246 mi  068 deg  (2251 mi)
   19:38  28.126187  -16  0      WB3AVN   FM19og   2250 mi  067 deg  (2255 mi)
   19:38  28.126115   +4  0       K9JVA   EM79te   1844 mi  067 deg  (1848 mi)
   19:38  28.126119  -23  0        K7NT   DN06if    935 mi  352 deg  ( 933 mi)
   19:38  28.126126  -22  0      KQ4BYL   FM06id   2125 mi  073 deg  (2130 mi)
   19:38  28.126020   -1  0        KX4O   FM18cp   2197 mi  068 deg  (2202 mi)
   19:38  28.125998  -30  0        W8AC   EN91jm   2023 mi  063 deg  (2027 mi)
   19:38  28.126112   -2 -2       N9AWU   EM69sm   1735 mi  066 deg  (1739 mi)
   19:38  28.126136  -27  0      KD9LOK   EM69uf   1742 mi  067 deg  (1746 mi)
   19:38  28.126115   -7  0       N2NOM   FN22bg   2301 mi  062 deg  (2305 mi)
   19:38  28.126118  -29  0     KC2STA4   FN22vx   2388 mi  061 deg  (2393 mi)
   19:38  28.126144  -13  0      AC3V/3   FN11rj   2264 mi  063 deg  (2269 mi)
   19:38  28.126131  -26  0       PY3FF   GF49ju   6101 mi  128 deg  (6090 mi)
   19:38  28.126143  -13  0        WF7W   CN88hb   1108 mi  343 deg  (1107 mi)
   19:38  28.126104  -26  0      WA6JRW   DM14he    102 mi  335 deg  ( 102 mi)
   19:38  28.126125   +3  0      WA3TTS   EN90xn   2079 mi  065 deg  (2083 mi)
   19:38  28.126112   +5  0        WF8Z   EM79sm   1841 mi  066 deg  (1845 mi)
   19:38  28.126108  -17  0      WD4ELG   FM06be   2092 mi  073 deg  (2097 mi)
   19:38  28.126087  -16  0      K5MO-1     FM05   2143 mi  075 deg  (1642 mi)


*/


