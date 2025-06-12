/*
        gcc -g -Wall ../pulseaudio.c

        I run this from the ~/HamRadio/FT8/pactl/ directory.

        Options - get current volume and stream number
                  set current volume using that stream number
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
#include <malloc.h>

#define VOLUME_LOW  24600       // scale is from 0 to 65535, 0% to 100%
#define VOLUME_HIGH 44500 //49000  //41350 was volume setting for 100% when using WSPR beacon wav files

int pulseAudioVolume( int setVolumeHigh );

//  This function MUST be called while aplay is actively running.  It searches for the aplay volume control and adjust it.
//      If aplay is not running this function will do nothing.
int pulseAudioVolume( int setVolumeHigh ) {
    FILE *fptr;
    char *cc, *cc1, string[4096];
    char *lines[4096];          // too lazy to do a linked list
    int iii;
    int streamNumber;
    //int currentVolume;
    int returnValue = 0;

    //
    //  The purpose of this first block of code is to get the stream number and the current volume setting
    //

    for (iii = 0; iii < 4096; iii++) { lines[iii] = (char *)NULL; }     // not necessary but I'm a dweeb

    system("pactl list sink-inputs > z.txt");

    fptr = fopen("z.txt","rt");
    if (fptr == (FILE *)NULL) {
        return -1;                  // if file doesn't exist return null.
    }

    iii = 0;
    while (!feof(fptr)) {
        cc = fgets( string, 4096, fptr );         // read one line of the file
        if (cc == (char *)NULL) {
            break;
        }
        lines[iii] = (char *)calloc( strlen(string)+1, sizeof(char*));
        strcpy(lines[iii],string);
        iii++;
        //printf("%s",string);
    }

    iii = 0;
    while (lines[iii] != (char *)NULL) {
        if (strstr( lines[iii], "\"aplay\"" )) {
            //printf("    %s",lines[iii-13]);
            /*
            //    This code reads the current volume.  After writing this code I realized I didn't need it.
            cc = strstr( lines[iii-13], "Volume: mono:" );
            if (cc == (char *)NULL) { printf("Error 1\n"); returnValue = -1; break; }
            cc1 = strchr(cc,'/');
            if (cc1 == (char *)NULL) { printf("Error 2\n"); returnValue = -1; break; }
            cc1[0] = 0;
            if (sscanf(&cc[14],"%d",&currentVolume) != 1) { printf("Error 3\n"); returnValue = -1; break; }
            printf("Volume: %s %d\n",&cc[14],currentVolume);
            */

            //printf("    %s",lines[iii-23]);
            cc = strstr( lines[iii-23], "Sink Input #" );
            if (cc == (char *)NULL) { printf("Error 4\n"); returnValue = -1; break; }
            cc1 = strchr(cc,'\n');
            if (cc1 == (char *)NULL) { printf("Error 5\n"); returnValue = -1; break; }
            cc1[0] = 0;
            if (sscanf(&cc[12],"%d",&streamNumber) != 1) { printf("Error 6\n"); returnValue = -1; break; }
            fprintf(stderr,"streamNumber: %s %d\n",&cc[12],streamNumber);
        }
        iii++;
    }

    iii = 0;
    while (lines[iii] != (char *)NULL) {
        //printf("%s",lines[iii]);
        free( lines[iii++] );
    }

    //unlink( "z.txt" );

    //  quit here if error
    if (returnValue == -1) {
        return returnValue;
    }

    //
    //  The second part uses the stream number to change the volume setting based on the parameter passed.  It also verifies the current volume.
    //

    if (setVolumeHigh) {
        returnValue = VOLUME_HIGH;
    }
    else {
        returnValue = VOLUME_LOW;
    }
    sprintf(string,"pactl set-sink-input-volume %d %d",streamNumber,returnValue);
    system(string);

    return returnValue;
}


// uncomment MAIN_HERE 1 to run stand-alone
//#define MAIN_HERE 1
#ifdef MAIN_HERE
int doCurlFT8( void );

int main() {
    int highLow = 0;
    printf("High 1, low 0 - ");
    scanf("%d",&highLow);
    int iii = pulseAudioVolume( 0 );
    printf("iii = %d\n",iii);
    return iii;
}


#endif
