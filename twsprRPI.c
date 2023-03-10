/*
    gcc -g -Wall -o twsprRPI twsprRPI.c wav_output2.c ft847.c wsprnet.c azdist.c geodist.c grid2deg.c -lportaudio -lrt -lm -lasound -pthread

    When running direct stderr to null with
        ./twsprRPI 2>/dev/null
    (or to a file instead of null)

    * A log file exist named log_twspr.txt.  It records startup, shutdown, and bursts (including freq).  I had intended to keep a second log that kept track of
    total up time and total down time as well as the bursts on each band.  But that info can all be collected from the log file so I'm leaving this off for now.

    * Another file, WSPRConfig, is read before starting any beacon.  It contains the Rx and Tx frequencies.  It is closed after every run so it can be modified.

    * A third file, blackout.txt, exists to prevent sending beacon during a satellite pass.  It is checked at the beginning of each waitForTopOfEvenMinute().  If
    necessary it will hold the program until the blackout period ends.  Format of blackout.txt file is described just above doBlackout() below.

    * A fourth file, raw_reports_log.txt, records all stations that reported hearing my beacon.  The date is mission from the first year or so of this file.

    pavucontrol is different for 6m and for 10m in order to get 10w output.  With gain = 1.0 (in wav_output2.c, global variable) on 10m the pavucontrol for this
    app runs about 39% while for 6m about 33%.  So in order to keep the pavucontrol the same (39%) for both I need to reduce the gain for 6m to 0.6.  So I added
    a gain parameter to sendWSPRData().  So:
       Set pavucontrol slider for this app to 39%.
       10m pass a gain parameter of 1.0.
       6m pass a gain parameter of 0.6.
    I could reduce the gain parameter even more and set the pavucontrol slider to 100% but this will limit the resolution that I have.

    Ideas:
     - record who heard me into a .csv file, sort of my WSPR log of one way qsos.
     - steer radio to different frequencies, say 40 MHz for one hour every night, then 2m for one hour every night, 6m for one hour.  Will have to
       change WSJT-X frequency.  The only way to do this automatically seems to be to save several --rig-name options.  There doesn't seem to be a
       UDP message to change this or to close WSJT-X.  I'll have to rely on Linux to do that for me.

*/
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "ft847.h"


#define MINUTES_TO_WAIT     (29)
#define SECONDS_TO_WAIT     (MINUTES_TO_WAIT*60)    //  Tx for 2 min, wait 25 min, then waitForTopOfEvenMinute() will wait for the next min, resulting in Tx 28 min apart.
#define WSPR_DEFAULT_15M    (21094630)
#define WSPR_DEFAULT_10M    (28124620)
#define WSPR_DEFAULT_6M     (50293080)
#define WSPR_DEFAULT_2M     (144489110)             // was 144489060 but N3IZN said I was too low.  I think both of our radios are off.
#define WSPR_15M            (21094600)              //  the actual frequencies.  The fudge factor is in readConfigFileWSPRFreq().
#define WSPR_10M            (28124600)
#define WSPR_6M             (50293000)
#define WSPR_2M             (144489000)
#define CONFIG_FILENAME     "WSPRConfig"

#define NO_WAIT_FIRST_BURST 1
#define BLACKOUT_FILENAME   "blackout.txt"

extern int sendWSPRData( char *filename, float gainX );     // would normally be in wav_output2.h but this is the only definition so just put it here.
extern int initializePortAudio( void );
extern void terminatePortAudio( void );
extern int doCurl( char* date1, char* date2, char* date3, char* date4, char* termPTSNum );         // same here except this is from wsprnet.c

extern double n3iznFreq;        // this is also the only variable definition imported from wsprnet.c

int terminate = 0;

static int signalCaptured = 0;
static int sock;                        // for all UDP messages
static struct sockaddr_in adr_inet;     // for UDP message to preamp.py
int SockAddrStructureSize;              // for UDP message to preamp.py
static struct sockaddr_in adr_inet2;    // for UDP message to SDRPlay// AF_INET
int SockAddrStructureSize2;             // for UDP message to SDRPlay
static struct sockaddr_in adr_inet3;    // for UDP message to send Email
int SockAddrStructureSize3;             // for UDP message to send Email
static char lineToRemove[256] = "";     // for blackoutCheck() and blackoutUpdateFile()

int sendUDPEmailMsg( char *message );


static int readConfigFile( int *rx0FreqHz, int *tx1FreqHz, int *tx2FreqHz, int *tx3FreqHz, int *tx4FreqHz );
static int convResultOrAdjust( int convResult );
static int readConfigFileWSPRFreq( int convResult );
static int readConfigFileWSPRFreqHelp( int convResult, int WSPRFreq );
static int readConfigFileHelp( char *string );
static void SignalHandler( int signal );
static int txWspr( int rxFreq, int txFreq, char* timestamp );
static char *getWavFilename( int txFreq );
static int waitForTopOfEvenMinute( int txFreq );
static int updateFiles( char *eventName );
static int installSignalHandlers( int useMyHandlers );
static int initializeNetwork( void );
static void closeNetwork( void );
static int sendUDPMsg( int doingTx );
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
    char timestamp1[16],timestamp2[16],timestamp3[16],timestamp4[16];
    char termPTSNum[4] = "";

    int rx0FreqHz = WSPR_DEFAULT_15M;
    int tx1FreqHz = WSPR_DEFAULT_15M;
    int tx2FreqHz = WSPR_DEFAULT_10M;
    int tx3FreqHz = WSPR_DEFAULT_6M;
    int tx4FreqHz = WSPR_DEFAULT_2M;

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
                printf("\nUsing /dev/pts/%s for 15m output\n\n",termPTSNum);
            }
        }
    }

    if (initializeNetwork() == -1) { return -1; }
    if (initializePortAudio() == -1) { return -1; }
    if (ft847_open() == -1) { return -1; }
    if (updateFiles("Startup ")) { return 1; }

    printf("\n\n");
    printf("<ENTER> to quit, '*'+<ENTER> to read WSPRConfig and change Rx freq, '-'+<ENTER> to terminate wait period\n");
    printf("\n");

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

    while (terminate == 0) {
        if (minCounter == 0) {
            printf("Wait %d min (<ENT>, *<ENT>, -<ENT>): ",minWait);  fflush( (FILE *)NULL );
        }

        tv.tv_sec = 60;
        tv.tv_usec = 0;

        FD_ZERO(&rfds);
        FD_SET(0, &rfds);       //  stdin

        retval = select(/*(sock + 1)*/ 1, &rfds, NULL, NULL, &tv);
        if (retval < 0) {
            if (terminate == 0) { printf("Error in select()\n"); } else { printf("Captured signal %d\n",signalCaptured); }
            break;
        }
        else if (retval > 0) {
            if (FD_ISSET(0, &rfds)) {           //  If keyboard data.  Note I'm not putting the keyboard in raw mode so
                //  The keyboard was hit.       //    nothing will happen unless I hit ENTER.
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
                            if (readConfigFile( &rx0FreqHz, &tx1FreqHz, &tx2FreqHz, &tx3FreqHz, &tx4FreqHz ) || (ft847_writeFreqHz( rx0FreqHz ))) {
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
        } else {
            if (readConfigFile( &rx0FreqHz, &tx1FreqHz, &tx2FreqHz, &tx3FreqHz, &tx4FreqHz )) {
                retval = -1;    // on error or if rx0FreqHz == 0
                break;
            }
            printf("\nRx %d   Tx1 %d   Tx2 %d   Tx3 %d   Tx3 %d\n",rx0FreqHz, tx1FreqHz, tx2FreqHz, tx3FreqHz, tx4FreqHz );
            if (ft847_writeFreqHz( rx0FreqHz )) {                 // set radio to newly read frequency.  It happens again at the end of txWspr() but I frequently don't want to wait.
                retval = -1;    // on error or if rx0FreqHz == 0
                break;
            }
            timestamp1[0] = timestamp2[0] = timestamp3[0] = timestamp4[0] = 0;      // zero out timestamp strings in case tx1/tx2/tx3/tx4FreqHz is zero
            minWait = MINUTES_TO_WAIT;
            if (tx1FreqHz != 0) {
                if (txWspr(rx0FreqHz, tx1FreqHz, timestamp1)) {
                    retval = -1;
                    break;
                }
                minWait -= 4;
            }
            if (tx2FreqHz != 0) {
                if (txWspr(rx0FreqHz, tx2FreqHz, timestamp2)) {
                    retval = -1;
                    break;
                }
                minWait -= 4;
            }
            if (tx3FreqHz != 0) {
                if (txWspr(rx0FreqHz, tx3FreqHz, timestamp3)) {
                    retval = -1;
                    break;
                }
                minWait -= 4;
            }
            if (tx4FreqHz != 0) {
                if (txWspr(rx0FreqHz, tx4FreqHz, timestamp4)) {
                    retval = -1;
                    break;
                }
                minWait -= 4;
            }
            if ( (tx1FreqHz != 0) || (tx2FreqHz != 0) || (tx3FreqHz != 0) || (tx4FreqHz != 0) ) {   // if a beacon was sent
                if (waitForTopOfEvenMinute( 0 )) {                                                  // ... wait for two more minutes
                    retval = -1;
                    break;
                }
                if (doCurl( timestamp1, timestamp2, timestamp3, timestamp4, termPTSNum )) {         // ... and get results from wsprnet.org
                    retval = -1;
                    break;
                }
                minWait -= 2;
            }
            minCounter = 0;
            printf("\n");
        }
    }

    if (updateFiles("Shutdown")) { retval = -1; }
    ft847_close();
    terminatePortAudio();
    closeNetwork();
    printf("\n");
    return retval;
}


//  This function is called as sleep ends.  It waits for the top of second and then initiates a send.
static int txWspr( int rxFreq, int txFreq, char* timestamp ) {
    int iii;
    float gain;
    char string[16];
    struct tm *info;
    time_t rawtime;         // time_t is long integer

    if (waitForTopOfEvenMinute( txFreq )) {
        return 1;
    }

    //  Update files with a burst message based on the frequency
    if (txFreq > 144000000) {
        strcpy( string, "Burst2m " );
    } else if (txFreq > 50000000) {
        strcpy( string, "Burst6m " );
    } else if (txFreq > 28000000) {
        strcpy( string, "Burst10m" );
    } else {
        strcpy( string, "Burst15m" );
    }
    if (updateFiles(string)) { return 1; }

    //  Put radio in Tx mode and put SDRPlay into Tx mode
    if (ft847_FETMOXOn()) { return 1; }
    if (sendUDPMsg( 1 )) { return 1; }

    if (txFreq > 144000000) {
        gain = 0.8;
    } else if (txFreq > 50000000) {                // set gain based on freq: 0.8 for 2m, 0.6 for 6m and 1.0 for 10m/15m
        gain = 0.6;
    } else {
        gain = 1.0;
    }

    time( &rawtime );
    info = gmtime( &rawtime );      // UTC
    sprintf(timestamp,"%02d:%02d",info->tm_hour, info->tm_min);
    printf("Beacon freq %d Hz at %s:%02d UTC          \n", txFreq, timestamp, info->tm_sec);
    iii = sendWSPRData( getWavFilename(txFreq), gain );
    if (iii) {
        printf("Error on sendWSPRData()\n");
    }

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
//      even second it will set the radio to txFreq unless txFreq == 0
//  Later I added code to check blackout.txt in order to see if it needs to delay things for a satellite pass.
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

    doBlackout();

    //   loop until top of minute
    printf("\nWaiting for top of even minute: ");  fflush( (FILE *)NULL );
    while (1) {
        time( &rawtime );                   // rawtime is the number of seconds in the epoch (1/1/1970).  time() also returns the same value.
        info = localtime( &rawtime );       // info is the structure giving seconds and minutes

        if (info->tm_sec == 0) {            // if top of minute
            int isOdd = info->tm_min % 2;   // ... and this is an even minute
            if (!isOdd) {                   // ... then break
                break;
            }
        }

        //  If three seconds before the top of even second set the radio to txFreq (unless txFreq == 0)
        if (!freqChangeDone) {
            if (txFreq) {                                       // if txFreq != 0
                if (info->tm_sec == 57) {                       // if 57 second
                    int isOdd = info->tm_min % 2;               // ... and this is an odd minute
                    if (isOdd) {                                // ... write freq change
                        if (ft847_writeFreqHz( txFreq )) {      // set radio to transmit frequency
                            returnValue = 1;
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

        if (curSec != info->tm_sec) {
            curSec = info->tm_sec;
            printf("\rWaiting for top of even minute: %02d %02d     ",info->tm_min,curSec);
            fflush( (FILE *)NULL );
        }

        {
            int NumBytesIn;

            ioctl(0,TIOCINQ,&NumBytesIn);
            if (NumBytesIn > 0) {
                while ( NumBytesIn > 0 ) {  //  Swallow ENTER and everything before it.
                    getchar();
                    NumBytesIn--;
                }
                printf("\r Press ENTER to resune.                     ");     fflush( (FILE *)NULL );
                getchar();
            }           //  if (NumBytesIn > 0)
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
    //printf("Current local time and date: %ld %d %d %d   %s ", rawtime, info->tm_hour, info->tm_min, info->tm_sec, asctime(info));
    return returnValue;
}


//  Handles all the logging events.  It is meant to be self-contained.  That is, fptr is openned and closed here.
static int updateFiles( char *eventName ) {
    time_t rawtime;
    struct tm *info;
    char timestamp[64];

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
    fprintf( logFile, "%s  %ld   %s \n", eventName, rawtime, timestamp);
    fclose(logFile);
    return 0;
}


//  Read config file, get data, and close it again.  That way the file can be minipulated between bursts.
//      If rxFreq == 0 then return error and let the program quit.
//      If any of the frequencies are not correct return error.
//          rx0FreqHz not within 1.8 MHz - 450 MHz
//          tx1FreqHz, tx2FreqHz, tx3FreqHz, or tx4FreqHz not a frequency (not all numbers).  If not WSPR freq
//              then the frequency will be zero and no beacon will take place but no error returned.
static int readConfigFile( int *rx0FreqHz, int *tx1FreqHz, int *tx2FreqHz, int *tx3FreqHz, int *tx4FreqHz ) {
    FILE *fptr;
    char *cc, string[64];
    int convResult = 0;

    //  If file is missing then it is not an error.  Just continue to use the current values.
    //  If file is present then new values will be assigned to all three parameters.  However it is only an error if rx0FreqHz is zero.
    //  If tx1/tx2/tx3/tx4FreqHz are present then the values must match WSPR values.  Anything else or any error on conversion will cause
    //      tx1/tx2/tx3/tx4FreqHz to be set to zero but no error will be returned.
    //
    //      The format of the lines is:
    //          rx0FreqHz xxxxxxxx
    //          tx1FreqHz xxxxxxxx
    //          tx2FreqHz xxxxxxxx
    //          tx3FreqHz xxxxxxxx
    //          tx4FreqHz xxxxxxxx
    //      The rx0/tx1/tx2/tx3/tx4FreqHz must start on the first character of the line.
    //      The frequency must be in Hz and can be be as short as 7 digits (<10 MHz) or as long as 10 digits (144 or 432 MHz)
    //      Lines without this format can be present but will be ignored.

    fptr = fopen(CONFIG_FILENAME,"rt");
    if (fptr == (FILE *)NULL) {
        return 0;           // no error
    }
    *rx0FreqHz = *tx1FreqHz = *tx2FreqHz = *tx3FreqHz = *tx4FreqHz = 0;   // no going back.  Must get something assigned.
    while (!feof(fptr)) {
        cc = fgets( string, 64, fptr );         // read one line of the file
        if (cc == (char *)NULL) {
            break;
        }
        if (strlen(string) < 17) {      // token (rx0/tx1/tx2/tx3/tx4FreqHz) always 9 characters + at least one space + at least 7 characters for the freq.
            continue;
        }
        string[9] = 0;    // null terminate right after the token
        if (!strcmp(string,"rx0FreqHz")) {
            int convResult = readConfigFileHelp( &string[10] );
            if ( convResult < 0) {  break;  }       // error - quit
            if ((convResult >= 1800000) && (convResult <= 450000000)) {
                *rx0FreqHz = convResult;
            }
        } else if (!strcmp(string,"tx1FreqHz")) {
            int convResult = readConfigFileHelp( &string[10] );
            if ( convResult < 0) {  continue;  }    //  error - read the next line, if any.
            if ( readConfigFileWSPRFreq( convResult ) == 0 ) {
                *tx1FreqHz = convResultOrAdjust( convResult );
            }
        } else if (!strcmp(string,"tx2FreqHz")) {
            int convResult = readConfigFileHelp( &string[10] );
            if ( convResult < 0) {  continue;  }    //  error - read the next line, if any.
            if ( readConfigFileWSPRFreq( convResult ) == 0 ) {
                *tx2FreqHz = convResultOrAdjust( convResult );
            }
        } else if (!strcmp(string,"tx3FreqHz")) {
            int convResult = readConfigFileHelp( &string[10] );
            if ( convResult < 0) {  continue;  }    //  error - read the next line, if any.
            if ( readConfigFileWSPRFreq( convResult ) == 0 ) {
                *tx3FreqHz = convResultOrAdjust( convResult );
            }
        } else if (!strcmp(string,"tx4FreqHz")) {
            int convResult = readConfigFileHelp( &string[10] );
            if ( convResult < 0) {  continue;  }    //  error - read the next line, if any.
            if ( readConfigFileWSPRFreq( convResult ) == 0 ) {
                *tx4FreqHz = convResultOrAdjust( convResult );
            }
        }
    }

    clearerr(fptr);
    fclose(fptr);
    if (*rx0FreqHz == 0) {
        return -1;
    } else if (convResult < 0) {
        return -1;
    } else {
        return 0;
    }
}


/*
    This is an attempt to adjust the 2m beacon freq to get it close to 1500 Hz, 144.490500 MHz.  wsprnet.c captures the value of n3iznFreq,
        a double, which is the freq N3IZN/SDR reports for my 2m beacon.  The rest is done here.  The frequency sent to the FT847 is adjusted
        by half the error distance.  Note that ft847_writeFreqHz() rounds down to the 10 Hz resolution.

    If there is no N3IZN report then the value of n3iznFreq is reset to 0.0.  In that case the last 2m frequency will be used again or, if
        last2mFreq == 0 then convResult will be returned.

    When the frequency is outside the +/-100 Hz WSPR passband then sometimes a sidelobe will be reported.  That can still be effective at
        moving the frequency back.

    TODO - perhaps after four straight non-reports we reset last2mFreq to zero and go back to returning convResult.
*/
static int convResultOrAdjust( int convResult ) {
    static int last2mFreq = 0;

    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_2M ) == 0 ) {         // if 2m frequency
        printf("\n\nN3IZN freq = %lf\n",n3iznFreq);
        if (n3iznFreq != 0.0) {                                             //  and no offset has yet been applied.
            double tempf;
            int intfreq,offset;

            tempf = n3iznFreq * 1000000;                // convert to Hz
            intfreq = (int)tempf;                       // convert to integer
            printf("n3izn freq as integer %d\n",intfreq);
            printf("convResult %d\n",convResult);
            printf("last2mFreq %d\n",last2mFreq);
            offset = 144490500 - intfreq;               // negative if freq > 1500, positive is freq < 1500 (144,490,500 Hz)
            printf("difference %d\n",offset);
            //offset = offset / 2;                        // take offset and cut it in half.
            printf("Proposed offset %d\n",offset);

            if (last2mFreq == 0) {                      // if this is the first N3IZN result ...
                last2mFreq = convResult+offset;         // ... then add the offset to convResult
            }
            else {                                      // otherwise ...
                last2mFreq = last2mFreq+offset;         // ... then add the offset to previous freq used.
            }
            printf("New freq = %d\n",last2mFreq);
            return last2mFreq;

        }

        else {
            printf("last2mFreq = %d\n",last2mFreq);
            if (last2mFreq == 0) {
                printf("No N3IZN result, no previous freq.  Using %d\n",convResult);
                return convResult;
            }
            else {
                printf("No N3IZN result, reusing %d\n",last2mFreq);
                return last2mFreq;
            }
        }

    }

    return convResult;      // if not 2m then just return convResult.
}


//  Verify that the frequency is a WSPR frequency.  Returns 0 if so and -1 if not.  Note that I'm not checking for one freq on each band.  So
//      there is noting to stop me from sending a WSPR message on the same frequency several times in a row.
static int readConfigFileWSPRFreq( int convResult ) {
    if ( readConfigFileWSPRFreqHelp( convResult, WSPR_15M ) == 0 ) { return 0; }
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
    char *srvr_addr = "192.168.1.196";
    char *srvr_addr2 = "192.168.1.111";
    char *srvr_addr3 = "192.168.1.48";

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

    return 0;
}


static void closeNetwork( void ) {
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


