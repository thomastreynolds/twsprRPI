#ifndef _WAV_OUTPUT3_H_
#define _WAV_OUTPUT3_H_

extern int initializePortAudio( void );
extern void terminatePortAudio( void );
extern int sendWSPRData( char *filename, FILE* dupFile );
extern int sendFT8Data( FILE* dupFile );
extern pid_t pidof(const char* name);

#endif
