/*
        gcc -g -Wall getTempData.c

        This gets temperature data for use by twsprRPI.

        IMPORTANT - the pidof() routine searches for "./ds18b20".  The string must be with the leading "./" or this will fail.

        Comment out MAIN_HERE to link with twsprRPI. - LATE - I converted getTempData to return a double and had it send an Email if ds18b20 is not running.
            However, I didn't modify the main routine below.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include "wav_output3.h"
#include "twsprRPI.h"

#define TEMPERATURE_FILE    "/home/pi/HamRadio/temperature/indoor.txt"

double getTempData( void );
int powerOnOffFT847( int powerOn );

//  Returns -1.0 on error and if ds18b20 is not running it returns +1.0 and sends Email.  Otherwise it returns the temperature in F.
double getTempData( void ) {
    pid_t thepid;
    FILE *fptr;
    char string[1024],*cc,*cc2,temperature[1024];
    int iii;
    double dtemperature = 0.0;
    static int emailSent = 0;       // to make sure Email is only sent once.

    //  Make sure ds18b20 is running.  If not return 0.
    thepid = pidof("./ds18b20");
    if (thepid == -1) {
        usleep(500000);
        thepid = pidof("./ds18b20");
        if (thepid == -1) {
            char message[] = "getTempData.c - Unable to find pid of process \"./ds18b20\"\n";
            printf(message);
            if (emailSent == 0) {
                sendUDPEmailMsg( message );
                emailSent = 1;
            }
            return 1.0;
        }
    }
    emailSent = 0;

    //  get latest data from indoor.txt.
    fptr = fopen(TEMPERATURE_FILE,"rt");
    if (fptr == (FILE *)NULL) {
        printf("getTempData.c - Unable to open temperature file %s\n",TEMPERATURE_FILE);
        return -1.0;
    }
    iii = fseek(fptr,-40,SEEK_END);     if (iii == -1) { printf("getTempData.c - Error in fseek()\n");  fclose(fptr); return -1; }
    fgets( string, 1024, fptr );
    fclose(fptr);

    //  now parse the last line
    cc = strstr(string,"C (");      if (cc == (char *)NULL) { printf("getTempData.c - Error looking for \"C (\"\n"); return -1; }
    cc2 = strchr(cc,')');           if (cc2 == (char *)NULL) { printf("getTempData.c - Error looking for \")\"\n"); return -1; }
    cc = &cc[3];
    cc2[0] = 0;
    strcpy(temperature,cc);

    if ( sscanf(temperature, "%lf F", &dtemperature ) != 1 ) {
        printf("getTempData.c - Unable to convert %s to double\n",temperature);
        return -1;
    }

    return dtemperature;
}


//  This routine will power on or off the FT847 using gpio23.  It will wait 0.5 seconds then read gpio23/value and verify
//      it has the correct value.  It will retry up to 10 times before quitting.  This proved necessary after one time the
//      radio powered off but did not power on.  So it was necessary to verify and retry.
//  This could have been done with the GPIO routines in ft847.c.
int powerOnOffFT847( int powerOn) {
    int returnValue = -1;
    int value;
    FILE *fptr;

    fptr = fopen("/sys/class/gpio/gpio23/value","rt");
    if (fptr == (FILE *)NULL) {
        printf("Unable to open file /sys/class/gpio/gpio23/value for reading\n");
        return returnValue;
    }

    //printf("\n");
    for (int iii = 0; iii < 10; iii++) {
        //printf("     Now attempting to turn on/off - powerOn == %d\n",powerOn);
        if (powerOn) {
            system("echo \"1\" > /sys/class/gpio/gpio23/value");        // Power On - assume gpio23 is already set up
        } else {
            system("echo \"0\" > /sys/class/gpio/gpio23/value");        // Power OFF
        }
        usleep(500000);
        value = fgetc(fptr);
        //printf("     After waiting half second gpio23/value has == %d, %hhd\n",value,(unsigned char)value);
        if (powerOn) {
            if ((char)value == '1') {
                returnValue = 0;
                break;
            }
        } else {
            if ((char)value == '0') {
                returnValue = 0;
                break;
            }
        }
    }

    fclose(fptr);
    return returnValue;
}



//#define MAIN_HERE 1
#ifdef MAIN_HERE

/*  Returns pid of the process passed as the name parameter.  It returns the pid or -1 if no process exists
    This is normally in wav_output3.c but needs to be here if stand-alone.
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

int main( void ) {
    char temperature[256];

    strcpy(temperature,"(....)");
    int iii = getTempData( temperature );
    printf("Temperature %s, return value %d\n", temperature, iii );
}


#endif
