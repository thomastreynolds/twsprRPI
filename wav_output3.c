/**
      wav_output3.c - This file was created to invoke aplay to send wav files to the sound cards.  It became necessary after I discovered wav_output2.c
        and portaudio caused spurious emissions about -20 dB down from the main lobe when run on my RPi.  aplay does not do that.  See Ham Radio Notes.docx, 3/30/2023.

      Below are the notes from portaudio version:

          The WSPR is 110.6 seconds long.  It starts a bit more than one second after the top-of-minute (https://swharden.com/software/FSKview/wspr/),
          so the first group of samples sent out are zero.  110.6 sec at 12000 samples per second is 1,327,200 samples (out of 1,440,000).

          gcc -g -Wall -o wav_output3 wav_output3.c
*/

#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <signal.h>
#include "twsprRPI.h"
#include "getTempData.h"

int initializePortAudio( void );
void terminatePortAudio( void );
int sendWSPRData( char *filename, float gainX );
pid_t pidof(const char* name);

int initializePortAudio( void ) {
    // empty process, just satisfying the linker
    return 0;
}


void terminatePortAudio( void ) {
    // empty process, just satisfying the linker
}


int sendWSPRData(char *filename, float gainX)
{
    struct tm *info;
    time_t rawtime;         // time_t is long integer
    int curSec = 0;          // debug for display
    char command[256];
    pid_t thepid;
    double currentTemperature;

    //  Invoke aplay with the desired wav file
    strcpy(command,"aplay --device pulse ");
    strcat(command,filename);
    strcat(command," &");
    system(command);

    //  wait 0.5 sec, try getting the pid.  If process not started then wait two seconds and try again.  If that fails then quit.
    usleep(500000);
    thepid = pidof("aplay");
    if (thepid == -1) {
        usleep(2000000);
        thepid = pidof("aplay");
        if (thepid == -1) {
            return -1;
        }
    }
    //printf("PID is %d\n",thepid);

    //  aplay will quit on its own when two minute wav file is complete.  This loop waits for it.
    while (kill(thepid,0) == 0) {           // see if pid is valid.  Invoking kill() with a signal of 0 does not check a signal but verifies the pid.
        time( &rawtime );                   // rawtime is the number of seconds in the epoch (1/1/1970).  time() also returns the same value.
        info = localtime( &rawtime );       // info is the structure giving seconds and minutes
        if (curSec != info->tm_sec) {
            curSec = info->tm_sec;
            printf("\rSending beacon %02d %02d (pid %d, file %s) ",info->tm_min,curSec,thepid,filename);
            fflush( (FILE *)NULL );
        }
        if (curSec == 30) {                         // at 30 seconds get the temperature.  Do it then because ds18b20 process writes a new value to the log
            currentTemperature = getTempData();     //      file at the top of each minute.
        }
        if (terminate) {    // from twspr.c
            break;
        }
        usleep(10000);
    }
    printf("\r");

    printf("Done sending beacon (%s, %3.3lf F)                      \n",filename,currentTemperature);
    return 0;
}


/*  returns pid of the process passed as the name parameter.  It returns the pid or -1 if no process exists
*/
pid_t pidof(const char* name)
{
    DIR* dir;
    struct dirent* ent;
    char* endptr;
    char buf[512];

    if (!(dir = opendir("/proc"))) {        // opendir opens a stream into the directory
        perror("can't open /proc");
        return -1;
    }

    while((ent = readdir(dir)) != NULL) {   //  readdir() reads the next entry in the dir stream (from opendir()) and stores in ent.
        // strtol() convert string to long integer.  Here it converts /proc/<subdir> name to lpid.  endptr contains first non-numeric digit.
        // So if endptr is not a null character, the directory is not entirely numeric, so ignore it.  Otherwise it is a process.
        long lpid = strtol(ent->d_name, &endptr, 10);       // end->d_name is the name of the entry, in this case a directory.
        if (*endptr != '\0') {
            continue;
        }

        // try to open the cmdline file.  The first part of the command file should be the function paramter "name".
        snprintf(buf, sizeof(buf), "/proc/%ld/cmdline", lpid);
        FILE* fp = fopen(buf, "r");

        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                /* check the first token in the file, the program name */
                char* first = strtok(buf, " ");
                if (!strcmp(first, name)) {
                    fclose(fp);
                    closedir(dir);
                    return (pid_t)lpid;
                }
            }
            fclose(fp);
        }

    }

    closedir(dir);
    return -1;
}

/*
int terminate = 0;
int main(void) {
    if (initializePortAudio() == -1) { return -1; }
    int iii = sendWSPRData( "1500.wav", 1.0 );
    if (iii) {
        printf("Error on sendWSPRData()\n");
    } else {
        printf("No error\n");
    }
    terminatePortAudio();
    return iii;
}
*/
