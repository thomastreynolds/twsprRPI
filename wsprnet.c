/*
    To run standalone:
        - uncomment MAIN_HERE directive at the bottom of file.
            gcc -g -Wall wsprnet.c azdist.c geodist.c grid2deg.c -lm
        - I usually want to remove the curl command below and just read the latest x.txt file, created from twsprRPI.
        - I'll have to change the three parameters in call to doCurl() at the bottom of the file, date1/2/3 to whatever times are in the x.txt file.
        - The call to sendUDPEmailMsg() must be commented out.  There is a commented out print statement below it that can be restored to print its message.
*/
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include "twsprRPI.h"

#define START_OF_LINE1  "<tr id=\"evenrow\">"
#define START_OF_LINE2  "<tr id=\"oddrow\">"
#define MAX_ENTRIES     500

#define RAW_LOG_NAME    "raw_reports_log.txt"


#define PURPLE    "\033[95m"
#define CYAN      "\033[96m"
#define DARKCYAN  "\033[36m"
#define BLUE      "\033[94m"
#define GREEN     "\033[92m"
#define YELLOW    "\033[93m"
#define RED       "\033[91m"
#define BOLD      "\033[1m"
#define UNDERLINE "\033[4m"
#define END       "\033[0m"

extern void azdist_(char* MyGrid, char* HisGrid,            // would normally be in a .h file but this is the only place that calls it.
                    int* nAz, int* nDmiles, int* nDkm);


struct Entry {
    char timestamp[64];
    char freq[64];
    char snr[64];
    char drift[64];
    char reporter[64];
    char reporterLocation[64];
    char distance[64];
    char azimuth[64];
    char distance2[64];
};
typedef struct Entry Entry;

char *goldenCalls[] = { "KK6PR",     "KP4MD",  "W7PAU",  "KA7OEI-1", "AC0G",
                        "KPH",       "KV0S",   "WA2TP",  "W2ACR",    "KA7OEI/Q",
                        "AI6VN/KH6", "K6RFT",  "KV4TT",  "W3ENR",    "K1RA-PI",
                        "W7WKR-K2",  "KV6X",   "N3IZN/SDR", "AA6RF" };
#define NUM_OF_GOLDEN_CALLS 19   // do this because "size_t n = sizeof(a) / sizeof(int);" won't work since each element is a different size.

int doCurl( struct BeaconData *beaconData, char* termPTSNum );

static int processEntries( Entry **entries, int *numEntries, char* termPTSNum, char *thedate, int minBeacon );
static int parseHTMLLine( char *string, struct BeaconData *beaconData, int numBeacons, Entry **entry, int *numEntries, char *thedate, int *numberOfDuplicates );
static char* parseHTMLTag( char *string, char *field );
static void doOneGrid( char *his, int *nAz, int *nDmiles );
static int getIndexBasedOnFreq( int ifreq );
static void resetGoldenList( void );
static int goldenListNotEmpty( void );
static void insertInGoldenList( char *freq );
static void processGoldenList( int txFreqHz, char* tone, int txFreqHzActual, double temperature, FILE *fptr, char* thedate, int *headerNotPrinted  );

int doCurl( struct BeaconData *beaconData, char* termPTSNum ) {
    FILE *fptr;
    char *cc, string[4096];
    int returnValue = 0;
    Entry *entries[MAX_ENTRIES];
    int numEntries = 0;
    char thedate[64];
    int numBeacons;
    int numberOfDuplicates;
    int iii;
    int minBeacon;      // the lowest beacon frequency

    //  Remove the seconds from the timestamp string.  It should already be removed, just in case.
    minBeacon = INT_MAX;
    for (iii = 0; iii < MAX_NUMBER_OF_BEACONS; iii++) {
        if (beaconData[iii].txFreqHz == 0) {
            break;
        }
        if ( strlen(beaconData[iii].timestamp) > 5) {       // they should all have length == 0 or 5
            beaconData[iii].timestamp[5] = 0;               // the dates passed are in HR:MN:SC so remove the :SC
        }
        if (beaconData[iii].txFreqHz < minBeacon) {         // get lowest beacon frequency in Hz and place it in minBeacon
            minBeacon = beaconData[iii].txFreqHz;
        }
    }
    numBeacons = iii;
    numberOfDuplicates = 0;

    system("curl -s -d \"mode=html&band=all&limit=600&findcall=nq6b&findreporter=&sort=date\" http://www.wsprnet.org/olddb -o x.txt");

    fptr = fopen("x.txt","rt");
    if (fptr == (FILE *)NULL) {
        return -1;
    }
    //printf("\n");
    while (!feof(fptr)) {
        cc = fgets( string, 4096, fptr );         // read one line of the file
        if (cc == (char *)NULL) {
            break;
        }
        //if ((strncmp( string, START_OF_LINE1, strlen(START_OF_LINE1) ) == 0) || (strncmp( string, START_OF_LINE2, strlen(START_OF_LINE2) ) == 0))  {
        if (strstr( string, START_OF_LINE1) || strstr( string, START_OF_LINE2) )  {
            //printf("%s",string);
            if (parseHTMLLine( string, beaconData, numBeacons, entries, &numEntries, thedate, &numberOfDuplicates ) ) {
                //returnValue = -1;
                break;
            }
        }
    }

    //  The output of the above curl statement and file read is entries[], a list of all the station that heard this beacon, with duplicates removed.
    //      Now display them.
    processEntries( entries, &numEntries, termPTSNum, thedate, minBeacon );

    for (iii = 0; iii < numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            free( entries[iii] );
        }
    }
    fclose(fptr);

    printf("Num entries %d                                \n",numEntries);
    printf("Num duplicates %d                             \n",numberOfDuplicates);   

    if (goldenListNotEmpty()) {
        int headerNotPrinted = 1;
        fptr = fopen("log_golden.txt","at");
        if (fptr == (FILE *)NULL) {
            return -1;
        }
        for (iii = 0; iii < numBeacons; iii++) {
            processGoldenList( beaconData[iii].txFreqHz, beaconData[iii].tone, beaconData[iii].txFreqHzActual, beaconData[iii].temperature, fptr, thedate, &headerNotPrinted );
        }
        fclose(fptr);
    }

    return returnValue;
}


static int processEntries( Entry **entries, int *numEntries, char* termPTSNum, char *thedate, int minBeacon ) {
    int num28MHz = 0;
    FILE *fptr, *remoteTerminal;
    int firstGolden = 0;
    double remoteTerminalFreq;

    resetGoldenList();              // clear out golden list here, before checking if entries are blank (wsprnet.org is down).  Otherwise golden list from previous burst will be repeated
    if (*numEntries == 0) {
        printf("\n");
        return 0;
    }

    remoteTerminal = (FILE *)NULL;
    remoteTerminalFreq = 0.0;
    if (termPTSNum[0] != 0) {
        char filename[64];

        sprintf(filename,"/dev/pts/%s",termPTSNum);
        remoteTerminal = fopen(filename,"at");      //  Error is ok because remoteTerminal will be checked against NULL

        remoteTerminalFreq = (double)minBeacon;         // convert minBeacon to double because it is compared against a double (24924000 becomes 24924000.0)
        remoteTerminalFreq /= 1000000;                  // convert from kHz to mHz (24924000.0 becomes 24.924000)
        remoteTerminalFreq = floor(remoteTerminalFreq); // remove everything after decimal (24.924000 becomes 24.000000)
        remoteTerminalFreq += 1.0;                      // add one because the comparison below is for all freqs below this value (24.000000 becomes 25.000000)
    }

    fptr = fopen(RAW_LOG_NAME,"at");
    if (fptr == (FILE *)NULL) {
        printf("\n");
        return 0;
    }

    //  Run through the list once and count how many 28 MHz stations are there.  I need to know this in advance for use in the second loop below.
    //      so that I can know to print in red when less than 10 entries.
    for (int iii = 0; iii < *numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            double entryFreq;
            sscanf(  entries[iii]->freq, "%lf", &entryFreq );

            // I want to collect all the 28 MHz reports and average the frequency value.  I also need the number of 28 MHz stations for use within the loop.
            if (entryFreq > 28.0) {
                if (entryFreq < 29.0) {     // if 10m entry
                    num28MHz++;
                }
            }
        }
    }

    //  Loop through and print things out.
    for (int iii = 0; iii < *numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            int tempInt;
            double entryFreq;
            FILE *terminal;         // either stdout or /dev/pts/?

            {
                char *ccc;              // I started getting bizzare frequencies from WSPRNet.org.  Find these and eliminate them.
                char string[64];

                strcpy(string, entries[iii]->freq);
                ccc = strchr(string,'.');                   // search for decimal point in string and remove it
                if (ccc == (char *)NULL) { continue; }
                strcpy( ccc, &ccc[1] );
                sscanf(string,"%d",&tempInt);               // convert string to int, representing frequency in Hz
                if (readConfigFileWSPRFreq(tempInt-1500)) { continue; }    // readConfigFileWSPRFreq() returns -1 if not a supported WSPR freq.  Have to add 1500 Hz for tone offset.
            }

            //  Get the frequency as a double for use below, checking for 21 MHz and 28 MHz.
            sscanf(  entries[iii]->freq, "%lf", &entryFreq );

            //  Set terminal to stdout unless ( 21 MHz AND a /dev/pts/XX number was input on the command line )
            terminal = stdout;
            if (entryFreq < remoteTerminalFreq) {   //  if entry is on the lowest beacon frequency band ...
                if (remoteTerminal) {               //  ... and if user imput a PTS number on the command line
                    terminal = remoteTerminal;      //  ... then write to that terminal.
                }
            }

            //  This block of code just determines what color to print the line with.
            sscanf( entries[iii]->distance, "%d", &tempInt );
            if (tempInt > 3000) {                                   // if distance > 3000 then print green
                fprintf(terminal,GREEN);
            } else {
                sscanf( entries[iii]->snr, "%d", &tempInt );
                if (tempInt >= 0) {                                 // if snr >= 0 then print blue
                    fprintf(terminal,BLUE);
                } else {
                    if (num28MHz < 10) {                // 28 MHz - highlight in red things that are not LOS but don't bother until the band begins to shut down.
                        if (entryFreq > 28.0) {
                            if (entryFreq < 29.0) {     // if 10m entry
                                char grid[64];
                                strcpy( grid, entries[iii]->reporterLocation );     // if 4-digit grid square not DM12, DM13, or DM14 then print red
                                grid[4] = 0;        // 4 digit grid square
                                if ( (strcmp(grid,"DM12")) && (strcmp(grid,"DM13")) && (strcmp(grid,"DM14")) && (strcmp(grid,"DM13")) ) {
                                    fprintf(terminal,RED);
                                }
                            }
                        }
                    }
                }
            }

            //  print the line
            fprintf(terminal,"   %s %10s  %3s %2s  %10s   %6s  %5s mi  %3s deg\n",
                   entries[iii]->timestamp, entries[iii]->freq, entries[iii]->snr, entries[iii]->drift,
                   entries[iii]->reporter, entries[iii]->reporterLocation, entries[iii]->distance,
                   entries[iii]->azimuth); //, entries[iii]->distance2);

            //  reset the color
            fprintf(terminal,END);

            //  print the line to a file
            fprintf(fptr,"   %s %10s  %3s %2s  %10s   %6s  %5s mi  %3s deg  (%s mi)\n",
                   entries[iii]->timestamp, entries[iii]->freq, entries[iii]->snr, entries[iii]->drift,
                   entries[iii]->reporter, entries[iii]->reporterLocation, entries[iii]->distance,
                   entries[iii]->azimuth, entries[iii]->distance2);

            //  Potentially send Email if on 6 or 2m
            {
                double dfreq;

                //  Make sure the frequency is 50 MHz or greater.
                tempInt = sscanf( entries[iii]->freq, "%lf", &dfreq );      // should return 1, one successful conversion
                if ( (tempInt == 1) && (dfreq >= 50.0) ) {
                    //  ... and make sure that the grid square is not DM12 or DM13
                    if (
                            ( strstr(entries[iii]->reporterLocation,"DM12") == (char *)NULL ) &&
                            ( strstr(entries[iii]->reporterLocation,"DM13") == (char *)NULL ) &&
                            ( strstr(entries[iii]->reporterLocation,"DM14") == (char *)NULL )
                       ) {
                        char message[1024],string[1024];        // super long strings because I'm too lazy to compute the actual lengths and do a calloc().

                        sprintf(message,"WSPR %s %s\n", entries[iii]->freq, entries[iii]->reporterLocation);
                        sprintf(string,"   %s %10s  %3s  %10s   %6s  %5s mi  %3s deg\n", entries[iii]->timestamp, entries[iii]->freq,
                                            entries[iii]->snr, entries[iii]->reporter, entries[iii]->reporterLocation, entries[iii]->distance, entries[iii]->azimuth);
                        strcat(message,string);
                        sendUDPEmailMsg( message );
                        //printf("%s\n",message);
                    }
                }
            }
        }
    }

    if (remoteTerminal) {
        fprintf(remoteTerminal," ------- \n");
        fclose(remoteTerminal);
    }

    //  Print out the "golden callsigns".  The ones that, from observation, seem to be GPS controlled because they are almost always reporting the same frequency.
    for (int iii = 0; iii < *numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            double entryFreq;
            sscanf(  entries[iii]->freq, "%lf", &entryFreq );
            if (entryFreq > 24.0) {        // only print out golden freqs on 12m and above
                int thisIsGoldenCall = 0;

                for (int jjj = 0; jjj < NUM_OF_GOLDEN_CALLS; jjj++) {
                    int sss = strcmp(entries[iii]->reporter, goldenCalls[jjj]);
                    if (sss == 0) {
                        thisIsGoldenCall = 1;
                        break;
                    }
                }

                if (thisIsGoldenCall) {
                    insertInGoldenList( entries[iii]->freq );

                    if (firstGolden == 0) {         // print separator line if this is the first.
                        firstGolden = 1;
                        fprintf(fptr,"\n");
                    }

                    //  print the golden call line to log file
                    fprintf(fptr,"  g %s %10s  %3s %2s  %10s   %6s  %5s mi  %3s deg (%s mi)\n",
                           entries[iii]->timestamp, entries[iii]->freq, entries[iii]->snr, entries[iii]->drift,
                           entries[iii]->reporter, entries[iii]->reporterLocation, entries[iii]->distance,
                           entries[iii]->azimuth, entries[iii]->distance2);

                }
            }
        }
    }

    fprintf(fptr," ------- %s above \n",thedate);
    fclose(fptr);

    return 0;
}


static int parseHTMLLine( char *string, struct BeaconData *beaconData, int numBeacons, Entry **entry, int *numEntries, char *thedate, int *numberOfDuplicates ) {
    /*
       <td align=left>&nbsp;2022-01-18 23:22&nbsp;</td>
       <td align=left>&nbsp;NQ6B&nbsp;</td>
       <td align=right>&nbsp;50.294526&nbsp;</td>
       <td align=right>&nbsp;-16&nbsp;</td>
       <td align=right>&nbsp;-1&nbsp;</td>
       <td align=left>&nbsp;DM12mr&nbsp;</td>
       <td align=right>&nbsp;+37&nbsp;</td>
       <td align=right>&nbsp;5.012&nbsp;</td>
       <td align=left>&nbsp;N3IZN/SDR&nbsp;</td>
       <td align=left>&nbsp;DM13ji&nbsp;</td>
       <td align=right>&nbsp;73&nbsp;</td>
       <td align=right>&nbsp;45&nbsp;</td>
       <td align=right>&nbsp;WSPR-2&nbsp;</td>
    */
    char *cc,*cc1;
    char field[128];
    char timestamp[64],freq[64],snr[64],drift[64], reporter[64], reporterLocation[64], distance[64],azimuth[64],distance2[64];
    int done;

    //  Find beginning of first <td> tag
    cc = strstr(string,"<td align=");
    if (cc == (char *)NULL) { return 0; }       // don't return -1 (error).  Just let it continue through the rest of the HTML code.

    //  Get timestamp
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    cc1 = strchr(field,' ');           if (cc1 == (char *)NULL) { return -1; }     // find space, which separates the date from the time.  I only want the time.
    strcpy(timestamp,&cc1[1]);
    *cc1 = 0;   strcpy(thedate, field);     // retrieve date for below.

    //  Since they are in chronological order the first line that doesn't match any of the timestamps is the last one needed.
    done = 1;
    for (int jjj = 0; jjj < numBeacons; jjj++) {
        if (strcmp(timestamp,beaconData[jjj].timestamp) == 0) {
            done = 0;
            break;
        }
    }
    if (done) {
        return -1;
    }
    //if (strcmp(timestamp,date1) && strcmp(timestamp,date2) && strcmp(timestamp,date3) && strcmp(timestamp,date4)) {
    //    return -1;
    //}

    //  Next field is my call, ignore it.
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }

    //  Next is frequency
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    strcpy(freq,field);

    //  Next is SNR
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    strcpy(snr,field);

    //  Next is drift
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    strcpy(drift,field);

    //  Next three are my grid, dBm and Watts, ignore.
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }

    //  Next is reporting callsign
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    strcpy(reporter,field);

    //  Next is reporting callsign location
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    strcpy(reporterLocation,field);

    //  Next is distance in km and miles.  Ignore km
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    strcpy(distance,field);

    //  get azimuth
    {
        int nAz,nDist2;

        doOneGrid( reporterLocation, &nAz, &nDist2 );
        sprintf(azimuth,"%03d",nAz);
        sprintf(distance2,"%4d",nDist2);
    }


    if (*numEntries < MAX_ENTRIES) {

        int iii;

        //  Loop through the entries.  If the frequency and reporter for any entry match the current line then update snr and quit
        for (iii = 0; iii < *numEntries; iii++) {

            //  Freqs are not exactly the same.  I really only want to know what band it is on so copy freqencies and truncate at the decimal point.
            //printf("%s %s %s %s\n",entry[iii]->freq, freq, entry[iii]->reporter, reporter);
            char freqEntry[20];             // freq already recorded in entries[]
            char freqNew[20];               // current freq parsed from HTML
            char *cc;
            strcpy(freqEntry,entry[iii]->freq);
            strcpy(freqNew,freq);
            cc = strchr(freqEntry,'.');     // get MHz (28, 50, 144) from this entries[]
            *cc = 0;
            cc = strchr(freqNew,'.');       // get MHz from freq parsed from HTML
            *cc = 0;

            if ( strcmp( freqEntry, freqNew ) == 0 ) {                  // if this element of entries[] is the same band as this HTML line
                if ( strcmp(entry[iii]->reporter, reporter) == 0 ) {    // ... and the same reporting station
                    int snrEntry, snrCur;                               // ... then check to see if the SNR in the HTML line is greater than in entries[]
                    sscanf( entry[iii]->snr, "%d", &snrEntry );  //  convert entry into an integer
                    sscanf( snr, "%d", &snrCur  );               //  convert current snr value to an integer
                    if ( snrEntry < snrCur ) {
                        strcpy(entry[iii]->snr,snr);
                    }
                    //printf("d   %s %10s  %3s %2s  %10s   %6s %5s\n",timestamp, freq, snr, drift, reporter, reporterLocation, distance);
                    (*numberOfDuplicates)++;
                    break;
                }
            }
        }

        //  If no match was found in the above loop then add new entry.
        if (iii == *numEntries) {
            entry[*numEntries] = malloc( sizeof( Entry ) );
            if (entry[*numEntries] == (Entry *)NULL) {
                printf("Error in malloc()\n");
                return -1;
            }

            strcpy( entry[*numEntries]->timestamp,timestamp);
            strcpy( entry[*numEntries]->freq,freq);
            strcpy( entry[*numEntries]->snr,snr);
            strcpy( entry[*numEntries]->drift,drift);
            strcpy( entry[*numEntries]->reporter,reporter);
            strcpy( entry[*numEntries]->reporterLocation,reporterLocation);
            strcpy( entry[*numEntries]->distance,distance);
            strcpy( entry[*numEntries]->azimuth,azimuth);
            strcpy( entry[*numEntries]->distance2,distance2);

            (*numEntries)++;
        }
    }

    /*
        Store timestamp, freq, snr, drift, reporter, reporterLocation, distance in an array
        Process the array, removing duplicates.
        Print array.  If dist > 3000 miles highlight in green.  After first or second report of only the regulars set a flag.  While that
            flag is set print anyone that is not a regular in red.  What clears the flag?
        Keep logbook.
        If new entry in logbook highlight bold.  Or maybe first for the day.
    */

    return 0;
}



/*
    parseHTMLTag() - pulls the field out of a <td></td> tag.  Returns a char* pointer to the beginning of the next tag.
        *string - pointer to the beginning of an tag somewhere in the string returned from curl call
        field[] - a string to store the tag value.
*/
static char* parseHTMLTag( char *string, char *field ) {
    //  All the HTML tags that I'm concerned with are <td>.  When the function is called string should point to the beginning of one.
    //  It will return a pointer to the next one and field[] will contain the contents of the HTML field.  Upon error it returns (char *)-1;
    char *returnValue;
    char *fieldBegin;
    char *cc;

    //  Find end of this tag and use it as the return value in preparation for the next call
    cc = strstr(string,"</td>");
    if (cc == (char *)NULL) { return (char *)-1; }
    returnValue = &cc[5];       //  point to end of </td>

    //  Find the first &nbsp; and use it to find the beginning of the field contents
    cc = strstr(string,"&nbsp;");
    if (cc == (char *)NULL) { return (char *)-1; }
    fieldBegin = &cc[6];        // point to end of &nbsp

    //  Find the second &nbsp; and use it to mark the end of the field contents
    cc = strstr(fieldBegin,"&nbsp;");
    if (cc == (char *)NULL) { return (char *)-1; }
    *cc = 0;                    // null-terminate

    strcpy(field,fieldBegin);
    return returnValue;
}


static void doOneGrid( char *his, int *nAz, int *nDmiles ) {
    int nDkm;
    char mine[] = "DM12qu";

    azdist_( mine, his, nAz,        //  Azimuth
                        nDmiles,    //  Distance in miles
                        &nDkm       //  Distance in km
                        );
}

/*
    The functions below are an attempt to use the golden calls to determine the proper frequency to set the FT847.  It collects the frequencies
    of all the golden calls in goldenList[].  The first index is for the band (12m, 10m, 6, and 2m).  The second is for each golden call
    heard.  The third index is the frequency (index 0) and sum (index 1).  The sum is only used in processGoldenList() and can be removed
    from goldenList[] and made a local array.

    I wanted the first index to represent each beacon, so that if there were multiple beacons on the same band they would be processed
    separately.  I didn't do that because processEntries() (where insertInGoldenList() is called from) doesn't have any beacon information.
    This is simpler although it will break down if multiple HF beacons are on the same band.  This is because the tone will change with
*/

#define FREQ_INDEX  0
#define SUM_INDEX   1

#define NUMBER_OF_GOLDEN_INDICES    4

#define INDEX_24MHZ     0
#define INDEX_28MHZ     1
#define INDEX_50MHZ     2
#define INDEX_144MHZ    3

static int goldenList[ NUMBER_OF_GOLDEN_INDICES ][NUM_OF_GOLDEN_CALLS][2];
static int numberItemsGoldenList[ NUMBER_OF_GOLDEN_INDICES ];

static int getIndexBasedOnFreq( int ifreq ) {
    if (ifreq > 144000000) {
        return INDEX_144MHZ;
    } else if (ifreq > 50000000) {
        return INDEX_50MHZ;
    } else if (ifreq > 28000000) {
        return INDEX_28MHZ;
    } else if (ifreq > 24000000) {
        return INDEX_24MHZ;
    } else {
        return -1;
    }
}

// call this from processEntries() above, prior to the block of code that prints the golden calls.
static void resetGoldenList( void ) {
    for (int jjj = 0; jjj < NUMBER_OF_GOLDEN_INDICES; jjj++) {
        for (int iii = 0; iii < NUM_OF_GOLDEN_CALLS; iii++ ) {
            goldenList[jjj][ iii ][FREQ_INDEX] = 0;
            goldenList[jjj][ iii ][SUM_INDEX] = 0;
        }
        numberItemsGoldenList[jjj] = 0;
    }
}

//  returns 1 if there is anything in the golden list, 0 otherwise.
static int goldenListNotEmpty( void ) {
    for (int jjj = 0; jjj < NUMBER_OF_GOLDEN_INDICES; jjj++) {
        for (int iii = 0; iii < NUM_OF_GOLDEN_CALLS; iii++ ) {
            if (goldenList[jjj][ iii ][FREQ_INDEX] != 0) {
                //printf("Found non-zero at iii=%d and jjj=%d, value %d\n",iii,jjj,goldenList[jjj][ iii ][FREQ_INDEX]);
                return 1;
            }
        }
    }
    //printf("Found golden list empty\n");
    return 0;
}

// call this from processEntries() above, in the loop where golden freqs are printed
static void insertInGoldenList( char *freq ) {
    int ifreq,index;
    char *ccc;

    ccc = strchr(freq,'.');
    if (ccc == (char *)NULL) { return; }
    strcpy( ccc, &ccc[1] );
    sscanf(freq,"%d",&ifreq);

    index = getIndexBasedOnFreq( ifreq );
    if (index == -1) { return; }

    goldenList[index][ numberItemsGoldenList[index] ][FREQ_INDEX] = ifreq;
    goldenList[index][ numberItemsGoldenList[index] ][SUM_INDEX] = 0;
    //printf(" freq %d  sum %d  num %d\n",
    //       goldenList[index][ numberItemsGoldenList[index] ][FREQ_INDEX],
    //       goldenList[index][ numberItemsGoldenList[index] ][SUM_INDEX],
    //       numberItemsGoldenList[index] );
    numberItemsGoldenList[index]++;
}


//  called from doCurl(), once for each band.
static void processGoldenList( int txFreqHz, char* tone, int txFreqHzActual, double temperature, FILE *fptr, char* thedate, int *headerNotPrinted  ) {
    int index, wsprFreq, minNumGolden;

    index = getIndexBasedOnFreq( txFreqHz );
    if (index == -1) { return; }
    switch (index) {
        case 0:
            wsprFreq = WSPR_12M;
            minNumGolden = 5;
            break;
        case 1:
            wsprFreq = WSPR_10M;
            minNumGolden = 5;
            break;
        case 2:
            wsprFreq = WSPR_6M;
            minNumGolden = 1;
            break;
        case 3:
            wsprFreq = WSPR_2M;
            minNumGolden = 1;
            break;
    }

    if (numberItemsGoldenList[index] >= minNumGolden) {        // 5 stations for HF, 1 for 6m and 2m
        int trueFreq, trueSum;
        int expectedFreq, error, error10;

        //  Loop through each station and find the differences in frequency between it and all the other stations.  Insert the sum of all differences in [SUM_INDEX]
        for (int iii = 0; iii < numberItemsGoldenList[index]; iii++) {
            int thisFreq = goldenList[index][iii][FREQ_INDEX];
            for (int jjj = 0; jjj < numberItemsGoldenList[index]; jjj++) {
                goldenList[index][iii][SUM_INDEX] += (thisFreq - goldenList[index][jjj][FREQ_INDEX]);    // this will add itself but since it's zero there's no difference and it's easier than making an exception for iii==jjj
            }
        }

        //  Now find the frequency that has the minimum sum.  This should be the true frequency.
        trueFreq = goldenList[index][0][FREQ_INDEX];
        trueSum = abs(goldenList[index][0][SUM_INDEX]);
        for (int iii = 1; iii < numberItemsGoldenList[index]; iii++) {
            if ( abs(goldenList[index][iii][SUM_INDEX]) < trueSum) {
                trueFreq = goldenList[index][iii][FREQ_INDEX];
                trueSum = goldenList[index][iii][SUM_INDEX];
            }
        }

        //printf(" trueFreq %d  trueSum %d  txFreqHz %d  tone %s  txFreqHzActual %d  temperature %lf\n",trueFreq, trueSum, txFreqHz, tone, txFreqHzActual, temperature );

        //  Now have true frequency.  Display the difference between true frequency and what it should have been, based on wsprFreq+tone.
        //      Display the error.  Display what the actual transmitted frequency should have been for zero error.
        //      Display number of golden calls hear on this band.  Write data to a log file.
        tone[4] = 0;                            // change "1500.wav" to "1500"
        sscanf( tone, "%d", &expectedFreq );    // convert tone to integer
        expectedFreq += wsprFreq;               // expected freq is where the signal should have been, wspr base freq plus tone frequency shows

        //  The radio can only be set to intervals on 10Hz.  Set error to be intervals of 10 Hz.
        error = expectedFreq-trueFreq;
        error10 = (error / 10)*10;
        if (abs(error % 10) < 5) {
            error = error10;
        } else {
            if (error < 0) {
                error = error10-10;
            } else {
                error = error10+10;
            }
        }

        //printf(" temperature %3.3lf  expectedFreq %d  actualFreq %d error %d (%d) suggested %d (%d calls)\n",temperature,expectedFreq,trueFreq,
        //        error, expectedFreq-trueFreq, txFreqHzActual+(error), numberItemsGoldenList[index] );
        //printf("  Temperature - Expected Freq - Actual Freq - Better Freq - Error\n");
        if (*headerNotPrinted) {
            printf(" Temperature  Expected Hz  Actual Hz  Better Hz   Error Hz\n");
            //       ddd.ddd dg   iiiiiiiii    iiiiiiiii  iiiiiiiii  iii (%d)  %d (stn)
            *headerNotPrinted = 0;
        }
        printf("  %3.3lf F    %9d    %9d  %9d   %3d (%d)  (%d stn)\n",
                temperature, expectedFreq, trueFreq, txFreqHzActual+(error), error, expectedFreq-trueFreq, numberItemsGoldenList[index] );
        fprintf(fptr,"  %3.3lf deg   %9d Hz    %9d Hz  %9d Hz  %3d (%d) Hz  (%d calls) %s\n",
                temperature, expectedFreq, trueFreq, txFreqHzActual+(error), error, expectedFreq-trueFreq, numberItemsGoldenList[index], thedate );
    }
}



// uncomment curl() call and, if using a different curl results, change fopen (two lines below curl) back to x.txt
//#define MAIN_HERE 1
#ifdef MAIN_HERE

int main() {
    struct BeaconData beaconData[ MAX_NUMBER_OF_BEACONS ];

    for (int iii = 0; iii < MAX_NUMBER_OF_BEACONS; iii++) {
        beaconData[iii].timestamp[0] = 0;
        beaconData[iii].txFreqHz = 0;
    }
    strcpy(beaconData[0].timestamp,"19:10:00");
    beaconData[0].txFreqHz = 24924650;
    strcpy(beaconData[1].timestamp,"19:14:00");
    beaconData[1].txFreqHz = 28124640;
    strcpy(beaconData[2].timestamp,"19:18:00");
    beaconData[2].txFreqHz = 50293160;

    return doCurl( beaconData, "3" );
}


#endif
