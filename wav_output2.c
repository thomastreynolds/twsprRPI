/**
      wav_output2.c - This came from ~/HamRadio/remote/portaudio/temp/wav_output2.c.  It was adapted for twspr program.

      Below are the notes from portaudio version:

          The purpose of this is to read a wav file and play it until the end.  The intention is to use it to send out WSPR beacon.
          I started with patest_start_stop.c

          http://truelogic.org/wordpress/2015/09/04/parsing-a-wav-file-in-c/ - this is a nice block of code for reading a wav file in C.  It is
          saved as parseWav.c.  There are a few C language errors in the reading of the header (strings not null terminated).  The code in
          readWavFile() (below) fixes them.

          The WSPR is 110.6 seconds long.  It starts a bit more thanone second after the top-of-minute (https://swharden.com/software/FSKview/wspr/),
          so the first group of samples sent out are zero.  110.6 sec at 12000 samples per second is 1,327,200 samples (out of 1,440,000).

          gcc -g -Wall -o wav_output2 wav_output2.c -lportaudio -lrt -lm -lasound -pthread
*/

#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "portaudio.h"

#define SAMPLE_RATE         (12000) //(44100)
#define FRAMES_PER_BUFFER   (400)
#define WSPR_MSG_SAMPLES    1327200 // 110.6 seconds at 12000 samples per second
#define DEFAULT_GAIN        1.0

//#define DEBUG_ON

extern int terminate;   // would normally be in twspr.h but this is the only item so no need for a header file.

//  Tom stuff
#define RING_BUFFER_SIZE 1024
float *wavDataFloat = (float *)NULL;
float gain = DEFAULT_GAIN;
unsigned long numOfSamples = 0;
unsigned long numOfSamplesToSend = 0;
unsigned long currentSample = 0;

float debugBuffer[RING_BUFFER_SIZE];
int debugBufferPointer = 0;
int interruptCounter = 0;

int initializePortAudio( void );
void terminatePortAudio( void );
int sendWSPRData( char *filename, float gainX );
static int readWavFile( char *filename );
static int patestCallback( const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer,
                const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData );

/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int patestCallback( const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData )
{
    float *out = (float*)outputBuffer;
    unsigned long i;
    float gainUsed;
    int finished = paContinue;

    (void) timeInfo; /* Prevent unused variable warnings. */
    (void) statusFlags;
    (void) inputBuffer;
    (void) userData;

    gainUsed = gain;

    for( i=0; i<framesPerBuffer; i++ )
    {
#ifdef DEBUG_ON
        *out++ = 0; //gainUsed*wavDataFloat[ currentSample ];  /* left */
        *out++ = 0; //gainUsed*wavDataFloat[ currentSample ];  /* right */
        if (currentSample < numOfSamplesToSend) {
            debugBuffer[ debugBufferPointer++ ] = gainUsed*wavDataFloat[ currentSample ]; //framesPerBuffer;
            debugBuffer[ debugBufferPointer++ ] = gainUsed; //wavDataFloat[ currentSample ];; //framesPerBuffer;
            debugBuffer[ debugBufferPointer++ ] = gain; //wavDataFloat[ currentSample ];; //framesPerBuffer;
            debugBufferPointer %= RING_BUFFER_SIZE-1;
        }
#else
        *out++ = gainUsed*wavDataFloat[ currentSample ];  /* left */
        *out++ = gainUsed*wavDataFloat[ currentSample ];  /* right */
#endif
        currentSample++;
        if (currentSample >= numOfSamples) {        // buffer is complete
            finished = paComplete;
            currentSample = 0;
        }
        if (currentSample >= numOfSamplesToSend) {  //  stop sending any data after this point (110.6 sec plus initial ~1 sec of zeros).
            gain = gainUsed = 0;
        }
    }

    interruptCounter++;

    return finished;
}


static int readWavFile( char *filename ) {
    int returnValue = 0;

    // WAVE file header format
    struct HEADER {
        unsigned char riff[4];                      // RIFF string
        unsigned int overall_size   ;               // overall size of file in bytes
        unsigned char wave[4];                      // WAVE string
        unsigned char fmt_chunk_marker[4];          // fmt string with trailing null char
        unsigned int length_of_fmt;                 // length of the format data
        unsigned int format_type;                   // format type. 1-PCM, 3- IEEE float, 6 - 8bit A law, 7 - 8bit mu law
        unsigned int channels;                      // no.of channels
        unsigned int sample_rate;                   // sampling rate (blocks per second)
        unsigned int byterate;                      // SampleRate * NumChannels * BitsPerSample/8
        unsigned short block_align;                 // NumChannels * BitsPerSample/8
        unsigned short bits_per_sample;             // bits per sample, 8- 8bits, 16- 16 bits etc
        unsigned char data_chunk_header [4];        // DATA string or FLLR string
        unsigned int data_size;                     // NumSamples * NumChannels * BitsPerSample/8 - size of the next chunk that will be read
    };

    unsigned char buffer4[4];
    unsigned char buffer2[2];
    char string[5] = { 0, 0, 0, 0, 0 };

    FILE *ptr;
    struct HEADER header;
    int read = 0;

    // open file
    printf("Reading file %s ...\n",filename);
    ptr = fopen(filename, "rb");
    if (ptr == NULL) {
        printf("Error opening file %s\n",filename);
        return 1;
    }

    // read header parts
    read = fread(string, sizeof(header.riff), 1, ptr);     // header.riff is not null terminated so read it (4 bytes) into a null terminated string
    //printf("(1-4): %s \n", string);

    read = fread(buffer4, sizeof(buffer4), 1, ptr);
    memcpy(&header.overall_size,buffer4,sizeof(header.overall_size));
    //printf("(5-8) header.overall_size: bytes:%u, Kb:%u \n", header.overall_size, header.overall_size/1024);

    read = fread(string, sizeof(header.wave), 1, ptr);      // header.wav is not null terminated so read it (4 bytes) into a null terminated string
    //printf("(9-12) header.wav: %s\n", string);

    read = fread(string, sizeof(header.fmt_chunk_marker), 1, ptr);  // header.fmt_chunk_marker is not null terminated so read it (4 bytes) into a null terminated string
    //printf("(13-16) header.fmt_chunk_marker: %s\n", string);

    read = fread(buffer4, sizeof(buffer4), 1, ptr);
    memcpy(&header.length_of_fmt,buffer4,sizeof(header.length_of_fmt));
    //printf("(17-20) header.length_of_fmt: %u \n", header.length_of_fmt);

    read = fread(buffer2, sizeof(buffer2), 1, ptr);
    header.format_type = buffer2[0] | (buffer2[1] << 8);
    //{
    //    char format_name[10] = "";
    //    if (header.format_type == 1)
    //        strcpy(format_name,"PCM");
    //    else if (header.format_type == 6)
    //        strcpy(format_name, "A-law");
    //    else if (header.format_type == 7)
    //        strcpy(format_name, "Mu-law");
    //    printf("(21-22) header.format_type: %u %s \n", header.format_type, format_name);
    //}

    read = fread(buffer2, sizeof(buffer2), 1, ptr);
    header.channels = buffer2[0] | (buffer2[1] << 8);
    //printf("(23-24) header.channels: %u \n", header.channels);

    read = fread(buffer4, sizeof(buffer4), 1, ptr);
    memcpy(&header.sample_rate,buffer4,sizeof(header.sample_rate));
    //printf("(25-28) header.sample_rate: %u\n", header.sample_rate);

    read = fread(&header.byterate, sizeof(header.byterate), 1, ptr);
    //printf("(29-32) header.byterate: %u (Bit Rate:%u)\n", header.byterate, header.byterate*8);

    read = fread(&header.block_align, sizeof(header.block_align), 1, ptr);
    //printf("(33-34) header.block_align: %u \n", header.block_align);

    read = fread(&header.bits_per_sample, sizeof(header.bits_per_sample), 1, ptr);
    //printf("(35-36) header.bits_per_sample: %u \n", header.bits_per_sample);

    read = fread(string, sizeof(header.data_chunk_header), 1, ptr);         // header.data_chunk_header is not null terminated so read it (4 bytes) into a null terminated string
    //printf("(37-40) header.data_chunk_header: %s \n", string);

    read = fread(&header.data_size, sizeof(header.data_size), 1, ptr);
    //printf("(41-44) header.data_size: %u bytes\n", header.data_size);

    //printf("\n");

    // calculate no.of samples
    numOfSamples = (8 * header.data_size) / (header.channels * (unsigned int)header.bits_per_sample);
    //printf("numOfSamples: %lu \n", numOfSamples);

    long size_of_each_sample = (header.channels * (unsigned int)header.bits_per_sample) / 8;
    //printf("size_of_each_sample: %ld bytes\n", size_of_each_sample);

    // calculate duration of file
    //float duration_in_seconds = (float) header.overall_size / header.byterate;
    //printf("duration_in_seconds = %1.1f\n", duration_in_seconds);

    //  now read the data

    // make sure that the bytes-per-sample is completely divisible by number of channels
    long bytes_in_each_channel = (size_of_each_sample / header.channels);
    if ((bytes_in_each_channel * header.channels) != size_of_each_sample) {
        printf("Error: %ld x %ud <> %ld\n", bytes_in_each_channel, header.channels, size_of_each_sample);
    }
    else {
        char data_buffer[size_of_each_sample];      // buffer for one sample.  If there are two channels then it is big enough to hold both samples.
        wavDataFloat = (float *)calloc(numOfSamples,sizeof(float));
        if (wavDataFloat == (float *)NULL) {
            returnValue = 1;
        }
        else {
            short samp;
            int initialZeros = 1;       // flag to indicate when past the first block of zeros.

            for (int iii = 1; iii < numOfSamples; iii++) {
                read = fread(data_buffer, sizeof(data_buffer), 1, ptr);
                if (read != 1) {
                    returnValue = 1;
                    break;
                }
                memcpy(&samp,data_buffer,sizeof(samp));
                wavDataFloat[iii] = (float)( (double)samp / 32768.0 );
                if (initialZeros) {
                    if (samp != 0) {
                        initialZeros = 0;
                        numOfSamplesToSend = (long)(iii + WSPR_MSG_SAMPLES);
                    }
                }

                //if (iii > (numOfSamples-10)) {
                //    printf("%d %02hhx %02hhx %04hx (%d) %1.1lf %1.5f %d\n",iii,data_buffer[0],data_buffer[1],samp,samp,(double)samp,wavDataFloat[iii],initialZeros);
                //}
            }
        }
    }

    fclose(ptr);
    return returnValue;
}


int initializePortAudio( void ) {
    PaError err;

    err = Pa_Initialize();
    if( err != paNoError ) {
        fprintf( stderr, "An error occured while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
        return -1;
    }
    return 0;
}


void terminatePortAudio( void ) {
    Pa_Terminate();
    printf("Port audio shutdown\n");
}


int sendWSPRData(char *filename, float gainX)
{
    PaStreamParameters outputParameters;
    PaStream *stream;
    PaError err;
    int iii;
    struct tm *info;
    time_t rawtime;         // time_t is long integer
    int curSec = 0;          // debug for display

    //  reset some global variables.
    gain = gainX;
    numOfSamples = numOfSamplesToSend = currentSample = 0;

    iii = readWavFile( filename );
    if (iii) {
        printf("Error while reading wav file\n");
        return 1;
    }

    //err = Pa_Initialize();
    //if( err != paNoError ) goto error;

    outputParameters.device = (PaDeviceIndex)5;     // pulseAudio is device 5 on RPI but 3 on Ubuntu
    //outputParameters.device = (PaDeviceIndex)3;
    if (outputParameters.device == paNoDevice) {
        fprintf(stderr,"Error: No default output device.\n");
        goto error;
    }
    outputParameters.channelCount = 2;       /* stereo output */
    outputParameters.sampleFormat = paFloat32; /* 32 bit floating point output */
    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(
              &stream,
              NULL, /* no input */
              &outputParameters,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER, //paFramesPerBufferUnspecified, //FRAMES_PER_BUFFER,
              paClipOff | paPrimeOutputBuffersUsingStreamCallback,      /* we won't output out of range samples so don't bother clipping them */
              patestCallback,
              &wavDataFloat );
    if( err != paNoError ) goto error;

    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;

    printf("Sending beacon ");  fflush( (FILE *)NULL );
    while ( ( err = Pa_IsStreamActive( stream ) ) == 1 ) {
        time( &rawtime );                   // rawtime is the number of seconds in the epoch (1/1/1970).  time() also returns the same value.
        info = localtime( &rawtime );       // info is the structure giving seconds and minutes
        if (curSec != info->tm_sec) {
            curSec = info->tm_sec;
            printf("\rSending beacon %02d %02d     ",info->tm_min,curSec);
            fflush( (FILE *)NULL );
        }
        if (terminate) {    // from twspr.c
            break;
        }
        usleep(10000);
    }
    printf("\r");

    err = Pa_StopStream( stream );
    if( err != paNoError ) goto error;

    printf("Done sending beacon (currentSample: %ld, Number ints %d)\n",currentSample,interruptCounter);
#ifdef DEBUG_ON
    for (iii = 0; iii < RING_BUFFER_SIZE; iii++) {
        printf("%d %f\n",iii,debugBuffer[iii]);
    }
#endif

    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto error;

    //Pa_Terminate();

    if (wavDataFloat != (float *)NULL) {
        free(wavDataFloat);
    }

    return err;
error:
    //Pa_Terminate();
    if (wavDataFloat != (float *)NULL) {
        free(wavDataFloat);
    }
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return err;
}

/*
int terminate = 0;
int main(void) {
    if (initializePortAudio() == -1) { return -1; }
    int iii = sendWSPRData( "1500.wav" );
    if (iii) {
        printf("Error on sendWSPRData()\n");
    } else {
        printf("No error\n");
    }
    terminatePortAudio();
    return iii;
}
*/
