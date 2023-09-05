#ifndef _WAV_OUTPUT3_H_
#define _WAV_OUTPUT3_H_

extern int initializePortAudio( void );
extern void terminatePortAudio( void );
extern int sendWSPRData( char *filename, float gainX );
extern pid_t pidof(const char* name);

#endif
