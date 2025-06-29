/*
    To run standalone:
        - uncomment MAIN_HERE directive at the bottom of file.
            gcc -g -Wall pskreporter.c azdist.c geodist.c grid2deg.c -lm
        - I usually want to remove the curl command below and just read the latest x.txt file.
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
#include <ctype.h>
#include "twsprRPI.h"

#define START_OF_LINE   "  <receptionReport receiverCallsign="
#define MAX_ENTRIES     500

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
    char call[64];
    char grid[64];
    char freq[64];
    char seconds[64];
    char mode[64];
    char snr[64];
    char distance[64];
    char azimuth[64];
};
typedef struct Entry Entry;

int doCurlFT8( time_t firstTxTime );

static int processEntries( Entry **entries, int *numEntries, time_t firstTxTime );
static int parseXMLLine( char *string, Entry **entry, int *numEntries );
static char* parseOneItem( char *string, char *quotedString );
static void doOneGrid( char *his, int *nAz, int *nDmiles );

int doCurlFT8( time_t firstTxTime ) {
    FILE *fptr;
    char *cc, string[4096];
    int returnValue = 0;
    Entry *entries[MAX_ENTRIES];
    int numEntries = 0;
    int iii;

    system("curl -s -d \"senderCallsign=NQ6B\"  https://retrieve.pskreporter.info/query -o y.txt");

    fptr = fopen("y.txt","rt");
    if (fptr == (FILE *)NULL) {
        return -1;
    }
    //printf("\n");
    while (!feof(fptr)) {
        cc = fgets( string, 4096, fptr );         // read one line of the file
        if (cc == (char *)NULL) {
            break;
        }
        if (strstr( string, START_OF_LINE))  {
            if (parseXMLLine( &string[strlen(START_OF_LINE)], entries, &numEntries ) ) {
                break;
            }
        }
    }

    //  The output of the above curl statement and file read is entries[], a list of all the station that heard this beacon, with duplicates removed.
    //      Now display them.
    processEntries( entries, &numEntries, firstTxTime );

    for (iii = 0; iii < numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            free( entries[iii] );
        }
    }
    fclose(fptr);

    //printf("Num entries %d\n",numEntries);

    return returnValue;
}


static int processEntries( Entry **entries, int *numEntries, time_t firstTxTime ) {
    FILE *terminal = stdout;        // either stdout or /dev/pts/?
    time_t tseconds;                //time_t is long integer
    int numRecentEntries = 0;
    char emailMessage[2048] = "";
    int needToSendEmail = 0;

    if (*numEntries == 0) {
        fprintf(terminal,"\n");
        return 0;
    }

    //  Loop through and print things out.
    fprintf(terminal,"\n\n");
    //fprintf(terminal,"  Tx Time -  %ld\n",firstTxTime);
    for (int iii = 0; iii < *numEntries; iii++) {
        if (entries[iii] != (Entry *)NULL) {
            int tempInt;
            struct tm *time_info;

            if (strcmp("FT8",entries[iii]->mode)) { continue; }

            //  convert entries[iii]->seconds to long integer tseconds then parse into time structure
            sscanf( entries[iii]->seconds, "%ld", &tseconds );
            time_info = localtime(&tseconds);

            if (tseconds < firstTxTime) { continue; } 

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

            //  print the line  call freq  snr  seconds grid distance/azimuth
            fprintf(terminal,"%9s %10s  %3s   %02d:%02d:%02d   %10s %5s mi %3s deg (%s)\n",
                   entries[iii]->call, entries[iii]->freq, entries[iii]->snr,
                   time_info->tm_hour, time_info->tm_min, time_info->tm_sec, 
                   entries[iii]->grid, entries[iii]->distance,
                   entries[iii]->azimuth, entries[iii]->mode );

            //  reset the color
            fprintf(terminal,END);
            numRecentEntries++;

            //  Potentially send Email if on 6 or 2m
            {
                double dfreq;
                char fourDigitGrid[64];

                strcpy(fourDigitGrid, entries[iii]->grid);
                fourDigitGrid[4] = 0;
                // printf("%s ",string);

                //  Make sure the frequency is 50 MHz or greater.
                tempInt = sscanf( entries[iii]->freq, "%lf", &dfreq );      // should return 1, one successful conversion
                if ( (tempInt == 1) && (dfreq >= 50000000) ) {
                    char string[64];

                    strcpy(string, entries[iii]->grid);
                    string[5] = 0;
                    //  ... and make sure that the grid square is not socal
                    if ( 
                            ( strstr( fourDigitGrid,"DM12") == (char *)NULL ) &&
                            ( strstr( fourDigitGrid,"DM13") == (char *)NULL ) &&
                            ( strstr( fourDigitGrid,"DM14") == (char *)NULL ) &&
                            ( strstr( fourDigitGrid,"DM22") == (char *)NULL ) &&
                            ( strstr( fourDigitGrid,"DM03") == (char *)NULL ) &&
                            ( strstr( fourDigitGrid,"DM04") == (char *)NULL ) 
                       ) {
                        char message[1024],string[1024];        // super long strings because I'm too lazy to compute the actual lengths and do a calloc().

                        sprintf(message,"6M FT8\n");
                        sprintf(string,"%02d:%02d:%02d %10s  %3s  %10s   %6s  %5s mi  %3s deg\n", time_info->tm_hour, time_info->tm_min, time_info->tm_sec, 
                                                        entries[iii]->freq, entries[iii]->snr, entries[iii]->call, 
                                                        entries[iii]->grid, entries[iii]->distance, entries[iii]->azimuth);
                        strcat(message,string);
                        strcat(emailMessage,message);
                        needToSendEmail = 1;
                        //printf("%s\n",message);
                    }
                }
            }
        }
    }
    fprintf(terminal,END);
    //fprintf(terminal," ------- \n");
    fprintf(terminal,"Num entries %d\n",numRecentEntries);

    if (needToSendEmail) {
        sendUDPEmailMsg( emailMessage );
    }

    return 0;
}


static int parseXMLLine( char *string, Entry **entry, int *numEntries ) {
  /*

  <lastSequenceNumber value="42033658637"/>
  <maxFlowStartSeconds value="1702800105"/>         451 seconds after recentFlowStartSeconds (7 min 31 sec)

  <receptionReport receiverCallsign="N3IZN/SDR" receiverLocator="DM13JI" senderCallsign="NQ6B" senderLocator="DM12QU"
        frequency="50313623" flowStartSeconds="1702799654" mode="FT8" isSender="1" receiverDXCC="United States" receiverDXCCCode="K"
        senderLotwUpload="2023-12-07" senderEqslAuthGuar="A" sNR="-14" />
  <receptionReport receiverCallsign="NQ6B" receiverLocator="DM12QU" senderCallsign="NQ6B" senderLocator="DM12QU"
        frequency="50313649" flowStartSeconds="1702799651" mode="FT8" isSender="1" receiverDXCC="United States" receiverDXCCCode="K"
        senderLotwUpload="2023-12-07" senderEqslAuthGuar="A" sNR="-5" />
  <receptionReport receiverCallsign="KF6JSD" receiverLocator="DM12QU" senderCallsign="NQ6B" senderLocator="DM12QU"
        frequency="50313627" flowStartSeconds="1702799651" mode="FT8" isSender="1" receiverDXCC="United States" receiverDXCCCode="K"
        senderLotwUpload="2023-12-07" senderEqslAuthGuar="A" sNR="28" />

  <senderSearch callsign="NQ6B" recentFlowStartSeconds="1702799654"  />

  */
    char call[64], grid[64], freq[64], seconds[64], mode[64], snr[64], distance[64], azimuth[64];
    char *cc;

    cc = parseOneItem( string, call );      if (cc == (char *)-1) { return -1; }
    //printf("  %s\n",call);

    cc = strstr( cc, "receiverLocator=" );  if (cc == (char *)NULL) { return -1; }
    cc = parseOneItem( &cc[ strlen("receiverLocator=") ], grid );
    grid[4] = tolower( grid[4] );       // have to convert the last two letters to lower case or else the computation of distance and azimuth won't work.
    grid[5] = tolower( grid[5] );
    //printf("  %s\n",grid);

    cc = strstr( cc, "frequency=" );  if (cc == (char *)NULL) { return -1; }
    cc = parseOneItem( &cc[ strlen("frequency=") ], freq );
    //printf("  %s\n",freq);

    cc = strstr( cc, "flowStartSeconds=" );  if (cc == (char *)NULL) { return -1; }
    cc = parseOneItem( &cc[ strlen("flowStartSeconds=") ], seconds );
    //printf("  %s\n",seconds);

    cc = strstr( cc, "mode=" );  if (cc == (char *)NULL) { return -1; }
    cc = parseOneItem( &cc[ strlen("mode=") ], mode );
    //printf("  %s\n",mode);

    cc = strstr( cc, "sNR=" );  if (cc == (char *)NULL) { return -1; }
    cc = parseOneItem( &cc[ strlen("sNR=") ], snr );
    //printf("  %s\n",snr);

    entry[*numEntries] = malloc( sizeof( Entry ) );
    if (entry[*numEntries] == (Entry *)NULL) {
        printf("Error in malloc()\n");
        return -1;
    }

    //  get azimuth and distance
    {
        int nAz,nDist2;

        doOneGrid( grid, &nAz, &nDist2 );
        sprintf(azimuth,"%03d",nAz);
        sprintf(distance,"%4d",nDist2);
    }
    //printf("  %s\n",distance);
    //printf("  %s\n\n",azimuth);

    strcpy( entry[*numEntries]->call,call);
    strcpy( entry[*numEntries]->grid,grid);
    strcpy( entry[*numEntries]->freq,freq);
    strcpy( entry[*numEntries]->seconds,seconds);
    strcpy( entry[*numEntries]->mode,mode);
    strcpy( entry[*numEntries]->snr,snr);
    strcpy( entry[*numEntries]->distance,distance);
    strcpy( entry[*numEntries]->azimuth,azimuth);

    (*numEntries)++;

    return 0;
}



/*
    parseOneItem() - All items are in double quotes.  This function pulls one item out and returns it.  Returns a pointer to one character past the
    quoted string or (char *)-1 on error.
        *string - pointer to the beginning of the quoted string.  The first character is assumed to be a double quote.
*/
static char* parseOneItem( char *string, char *quotedString ) {
    char *cc;

    //  Find end of this quoted string.
    cc = strchr( &string[1],'\"');                          // move past the beginning double quote and find the ending double quote
    if (cc == (char *)NULL) { return (char *)-1; }
    *cc = 0;
    if (strlen( &string[1] ) > 64) { return (char *)-1; }   //  no items are greater than 64 bytes.  If found one then error.
    strcpy(quotedString, &string[1]);

    return &cc[1];
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
time_t firstTxTime;

int main() {
    //time( &firstTxTime );   // get current time
    firstTxTime = 0;        // alternatively set to 0 to get all reports
    return doCurlFT8(firstTxTime);
}


#endif
