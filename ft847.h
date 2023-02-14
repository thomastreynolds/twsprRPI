#ifndef FT847_H
#define FT847_H 1

extern int ft847_open( void );
extern int ft847_close( void );
extern int ft847_FETMOXOn( void );
extern int ft847_FETMOXOff( void );
extern int ft847_writeFreqHz( int freq );
/*
extern int ft847_PTTOn( void );
extern int ft847_PTTOff( void );
extern int ft847_getPTTSetting( void );
extern int ft847_togglePTT( void );
extern int ft847_getFreqHz( void );
extern int ft847_writeFreqHz( int freq ); */

#endif
