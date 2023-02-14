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
extern int sendUDPEmailMsg( char *message );


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

char *goldenCalls[] = { "K1RA-PI", "WA2ZKD", "KK6PR", "KA7OEI-1", "KPH", "KP4MD", "W7PAU", "W3ENR", "AI6VN/KH6" };
#define NUM_OF_GOLDEN_CALLS 9   // do this because "size_t n = sizeof(a) / sizeof(int);" won't work sinceh each element is a different size.

//int tone28MHz = 1470;                  // filled in by twsprRPI.c inside getWavFilename(), value assigned here is just for linker when this file runs stand alone
double n3iznFreq = 0.0;

int doCurl( char* date1, char* date2, char* date3, char* date4, char* termPTSNum );

static int processEntries( Entry **entries, int *numEntries, char* termPTSNum, char *thedate );
static int parseHTMLLine( char *string, char *date1, char *date2, char *date3, char *date4, Entry **entry, int *numEntries, char *thedate );
static char* parseHTMLTag( char *string, char *field );
static void doOneGrid( char *his, int *nAz, int *nDmiles );

int doCurl( char* date1, char* date2, char* date3, char* date4, char* termPTSNum ) {
    FILE *fptr;
    char *cc, string[4096];
    int returnValue = 0;
    Entry *entries[MAX_ENTRIES];
    int numEntries = 0;
    char thedate[64];

    date1[5] = date2[5] = date3[5] = date4[5] = 0;    // the dates passed are in HR:MN:SC so remove the :SC

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
        if ((strncmp( string, START_OF_LINE1, strlen(START_OF_LINE1) ) == 0) || (strncmp( string, START_OF_LINE2, strlen(START_OF_LINE2) ) == 0))  {
            //printf("%s",string);
            if (parseHTMLLine( string, date1, date2, date3, date4, entries, &numEntries, thedate ) ) {
                //returnValue = -1;
                break;
            }
        }
    }

    processEntries( entries, &numEntries, termPTSNum, thedate );

    for (int iii = 0; iii < numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            free( entries[iii] );
        }
    }
    printf("Num entries %d\n",numEntries);

    fclose(fptr);
    return returnValue;
}


static int processEntries( Entry **entries, int *numEntries, char* termPTSNum, char *thedate ) {
    int num28MHz = 0;
    //double freqs28MHz = 0.0;
    FILE *fptr, *remoteTerminal;
    int firstGolden = 0;

    n3iznFreq = 0.0;

    if (*numEntries == 0) {
        printf("\n");
        return 0;
    }

    remoteTerminal = (FILE *)NULL;
    if (termPTSNum[0] != 0) {
        char filename[64];

        sprintf(filename,"/dev/pts/%s",termPTSNum);
        remoteTerminal = fopen(filename,"at");      //  Error is ok because remoteTerminal will be checked against NULL
        /*if (remoteTerminal) {
            printf("Using remote terminal %s.\n",filename);
        } else {
            printf("No remote terminal\n");
        }*/
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

            //  Get the frequency as a double for use below, checking for 21 MHz and 28 MHz.
            sscanf(  entries[iii]->freq, "%lf", &entryFreq );

            //  Set terminal to stdout unless ( 21 MHz AND a /dev/pts/XX number was input on the command line )
            terminal = stdout;
            if (entryFreq > 21.0) {
                if (entryFreq < 22.0) {             //  if 15m entry ...
                    if (remoteTerminal) {           //  ... and if user imput a PTS number on the command line
                        terminal = remoteTerminal;  //  ... then write to that terminal.
                    }
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
            fprintf(terminal,"   %s %10s  %3s %2s  %10s   %6s  %5s mi  %3s deg  (%s mi)\n",
                   entries[iii]->timestamp, entries[iii]->freq, entries[iii]->snr, entries[iii]->drift,
                   entries[iii]->reporter, entries[iii]->reporterLocation, entries[iii]->distance,
                   entries[iii]->azimuth, entries[iii]->distance2);

            //  reset the color
            fprintf(terminal,END);

            //  print the line to a file
            fprintf(fptr,"   %s %10s  %3s %2s  %10s   %6s  %5s mi  %3s deg  (%s mi)\n",
                   entries[iii]->timestamp, entries[iii]->freq, entries[iii]->snr, entries[iii]->drift,
                   entries[iii]->reporter, entries[iii]->reporterLocation, entries[iii]->distance,
                   entries[iii]->azimuth, entries[iii]->distance2);

            //  2 meter adjustment attempt.
            if (entryFreq > 144.0) {                                        // if 2m ...
                if (strcmp("N3IZN/SDR",entries[iii]->reporter) == 0) {      // and if N3IZN report ...
                    n3iznFreq = entryFreq;                                  // then get reported freq as double.
                }
            }

            //  Potentially send Email if on 6 or 2m
            {
                double dfreq;

                //  Make sure the frequency is 50 MHz or greater.
                tempInt = sscanf( entries[iii]->freq, "%lf", &dfreq );      // should return 1, one successful conversion
                if ( (tempInt == 1) && (dfreq >= 50.0) ) {
                    //  ... and make sure that the grid square is not DM12 or DM13
                    if (
                            ( strstr(entries[iii]->reporterLocation,"DM12") == (char *)NULL ) &&
                            ( strstr(entries[iii]->reporterLocation,"DM13") == (char *)NULL )
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

    fprintf(fptr," ------- %s above \n",thedate);
    fclose(fptr);

    if (remoteTerminal) {
        fprintf(remoteTerminal," ------- \n");
        fclose(remoteTerminal);
    }

    //  Print out the "golden callsigns".  The ones that, from observation, seem to be GPS controlled because they are almost always reporting the same frequency.
    for (int iii = 0; iii < *numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            int tempInt;
            FILE *terminal= stdout;
            int printThisLine = 0;

            for (int jjj = 0; jjj < NUM_OF_GOLDEN_CALLS; jjj++) {
                int sss = strcmp(entries[iii]->reporter, goldenCalls[jjj]);
                if (sss == 0) {
                    printThisLine = 1;
                    break;
                }
            }

            if (printThisLine) {
                if (firstGolden == 0) {
                    firstGolden = 1;
                    fprintf(terminal,"\n");
                }

                //  This block of code just determines what color to print the line with.
                sscanf( entries[iii]->distance, "%d", &tempInt );
                if (tempInt > 3000) {                                   // if distance > 3000 then print green
                    fprintf(terminal,GREEN);
                } else {
                    sscanf( entries[iii]->snr, "%d", &tempInt );
                    if (tempInt >= 0) {                                 // if snr >= 0 then print blue
                        fprintf(terminal,BLUE);
                    }
                }

                //  print the line
                fprintf(terminal,"   %s %10s  %3s %2s  %10s   %6s  %5s mi  %3s deg  (%s mi)\n",
                       entries[iii]->timestamp, entries[iii]->freq, entries[iii]->snr, entries[iii]->drift,
                       entries[iii]->reporter, entries[iii]->reporterLocation, entries[iii]->distance,
                       entries[iii]->azimuth, entries[iii]->distance2);

                //  reset the color
                fprintf(terminal,END);
            }
        }
    }

    return 0;
}


static int parseHTMLLine( char *string, char *date1, char *date2, char *date3, char *date4, Entry **entry, int *numEntries, char *thedate ) {
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


    //  Find beginning of first <td> tag
    cc = strstr(string,"<td align=");
    if (cc == (char *)NULL) { return -1; }

    //  Get timestamp
    cc = parseHTMLTag( cc, field );    if (cc == (char *)-1) { return -1; }
    cc1 = strchr(field,' ');           if (cc1 == (char *)NULL) { return -1; }     // find space, which separates the date from the time.  I only want the time.
    strcpy(timestamp,&cc1[1]);
    *cc1 = 0;   strcpy(thedate, field);     // retrieve date for below.

    //  Since they are in chronological order the first line that doesn't match any of the timestamps is the last one needed.
    if (strcmp(timestamp,date1) && strcmp(timestamp,date2) && strcmp(timestamp,date3) && strcmp(timestamp,date4)) {
        return -1;
    }

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

// uncomment curl() call and, if using a different curl results, change fopen (two lines below curl) back to x.txt
//#define MAIN_HERE 1
#ifdef MAIN_HERE

int main() {
    char date0[] = "05:12";
    char date1[] = "05:08";
    char date2[] = "05:04";
    char date3[] = "05:00";
    return doCurl( date3, date2, date1, date0, "" );
}


#endif
