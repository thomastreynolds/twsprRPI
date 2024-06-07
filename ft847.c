/*
  ft847.c - this is a C language version of the necessary functions from ft847.py.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>

int ft847_open( void );
int ft847_close( void );
int ft847_FETMOXOn( void );
int ft847_FETMOXOff( void );
int ft847_writeFreqHz( int freq );

//#define BUFFER_SIZE   64

static int serline = -1;                        // Linux port for the serial line.  Needed in select() and ioctl() although ft847_create() returns it.
static int GPIOInit = -1;
//static const char *SerName = "/dev/ttyUSB0";
static const char *SerName = "/dev/ttyUSBFT847";    // udev rule set up because the tty port started changing from ttyUSB0 to ttyUSB1
static struct termios OriginalTTYSettings;
//static unsigned char Buffer[BUFFER_SIZE];
//static int currentScreenOn = -1;

static int ft847_CATOn( void );
static int ft847_CATOff( void );
static unsigned char ConvertOneByte( char upperChar, char lowerChar );
static int ft847_write( unsigned char* msg, char* funcName );
//static int ft847_readwrite( char* msg, char* funcName );
static int ft847_writeMsg( unsigned char* msg );
//static int ft847_readMsg( char *msg );
static int ft847_initGPIO( void );
static int ft847_shutdownGPIO( void );
static int GPIOExport(int pin);
static int GPIOUnexport(int pin);
static int GPIODirection(int pin, int dir);
//static int GPIORead(int pin);
static int GPIOWrite(int pin, int value);


//  function returns -1 on error.  No printing, print error or success in calling routine.
int ft847_open( void ) {
  struct termios newtio;              // IO structure for terminal comm settings.

  serline = open(SerName, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (serline == -1) {
    printf("ft847_open() open error\n");
    return -1;
  }

  tcgetattr(serline,&OriginalTTYSettings);

  /*  The newtio structure:
    tcflag_t c_iflag;      // input modes
    tcflag_t c_oflag;      // output modes
    tcflag_t c_cflag;      // control modes
    tcflag_t c_lflag;      // local modes
    cc_t     c_cc[NCCS];   // control chars
  */
  bzero(&newtio, sizeof(newtio));
  newtio.c_iflag = IGNBRK | IGNPAR;       //  ignore parity and break errors on iflag (input modes)
  newtio.c_cflag = CS8 | CLOCAL | CREAD;  //  8 bit character size, ignore modem control lines, enable receiver (one stop bit by default)
  //newtio.c_lflag &= ~ICANON;
  newtio.c_cc[VMIN] = 1;                  //  read() will return after one character has been received.
  newtio.c_cc[VTIME] = 10;                //  read() will timeout after 10 deciseconds (0.1 second)
  cfsetispeed(&newtio, B57600);           //  Set input baud rate in newtio
  cfsetospeed(&newtio, B57600);           //  Set output baud rate in newtio
  tcflush(serline, TCIFLUSH);             //  flush received data on the port (TCIFLUSH)
  tcsetattr(serline, TCSANOW, &newtio);   //  Finally, set the port to the values in newtio.  TCSANOW says do it now.

  return serline;
}


int ft847_close( void ) {
  tcsetattr(serline, TCSANOW, &OriginalTTYSettings);  // Put serial port back to its original settings.
  return 0;
}


int ft847_writeFreqHz( int freq ) {
  char freqString[16];
  unsigned char msg[5] = { 0x00, 0x00, 0x00, 0x00, 0x01 };
  int returnValue = 0;

  //  The FT847 wants the frequency encoded in bytes in a strange manner.  A frequency of 432.198760 Mhz (432198.760 kHz) is encoded
  //    in (always) four bytes - 0x43, 0x21, 0x98, 0x76.  Note the resolution is only down to 10 Hz.

  sprintf( freqString, "%09d", freq );      // convert freq in Hz to string equivalent.  Insert leading zeros.
  freqString[ strlen(freqString)-1 ] = 0;   // remove the digit for Hz so the resolution is at 10 Hz.  No decimal point.  Since freq is passed in Hz the trailing zeros will always be there.

  msg[0] = ConvertOneByte( freqString[0], freqString[1] );
  msg[1] = ConvertOneByte( freqString[2], freqString[3] );
  msg[2] = ConvertOneByte( freqString[4], freqString[5] );
  msg[3] = ConvertOneByte( freqString[6], freqString[7] );

  //printf(" freq: %02hhx %02hhx %02hhx %02hhx %02hhx\n",msg[0],msg[1],msg[2],msg[3],msg[4]);
  if (ft847_CATOn()) { return -1; }
  if (ft847_write( msg, "ft847_writeFreqHz" )) {
    returnValue = -1;                   // if error on ft847_write() then attempt to turn CAT off but always return error.
    ft847_CATOff();
  } else {
    returnValue = ft847_CATOff();       // else if no error on ft847_write() then call ft847_CATOff() and respect its return value.
  }
  return returnValue;
}


static unsigned char ConvertOneByte( char upperChar, char lowerChar ) {
  unsigned char ReturnValue = 0x00;
  unsigned char upper, lower;

  upper = (unsigned char)(upperChar - '0');
  lower = (unsigned char)(lowerChar - '0');
  upper = upper & 0x0f;
  ReturnValue = upper << 4;
  lower = lower & 0x0f;
  ReturnValue = ReturnValue | lower;

  return ReturnValue;
}


static int ft847_CATOn( void ) {
  unsigned char msg[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
  return ft847_write( msg, "ft847_CATOn" );
}


static int ft847_CATOff( void ) {
  unsigned char msg[5] = { 0x00, 0x00, 0x00, 0x00, 0x80 };
  return ft847_write( msg, "ft847_CATOff" );
}


//  made into a function to encapsulate the error messages.  Returns 0 if ok, -1 on write error.
//      The paramater msg[] contains the message to write.
static int ft847_write( unsigned char* msg, char* funcName ) {
  int iii;

  if (ft847_writeMsg( msg )) {
    ft847_close();                  // frequent problem is that ttyUSB0 becomes ttyUSB1.  There is a ttyUSBFT847 which always points to the correct one (udev rule).
    iii = ft847_open();             //    So if error then close/open and try again.  "dmesg | grep ttyUSB" will tell me if it has switched.
    if (iii == -1) {
      printf("%s() write command error 1\n",funcName);
      return -1;
    }
    iii = ft847_writeMsg( msg );
    if (iii) {
      printf("%s() write command error 2,  %d\n",funcName,iii);
      return -1;
    }
  }
  return 0;
}


// returns 0 if ok, -1 on if port not open, -2 on write error.  Only called from ft847_write() directly above.
static int ft847_writeMsg( unsigned char* msg ) {
  if (serline == -1) { return -1; }                     // if serial port not open
  if (write( serline, msg, 5) != 5) { return -2; }      // if error in writing.
  return 0;
}


//  GPIO specific #defines
#define BUFFER_MAX 3
#define DIRECTION_MAX 35
#define VALUE_MAX 30
#define IN  0
#define OUT 1
#define LOW  0
#define HIGH 1
#define POUT 24


//  Put ft847 in Tx mode for digital
int ft847_FETMOXOn( void ) {
  if (ft847_initGPIO()) { return -3; }          // don't bother initializing GPIO until needed.  That way another program can use it.
  if (GPIOInit == -1) { return -2; }
  int iii = GPIOWrite(POUT, HIGH);
  //if (ft847_shutdownGPIO()) { return -1; }    // keep GPIO open while transmitting.  It once failed to open at end of burst and radio stayed in Tx mode for 3 hours.
  return iii;
}


//  Take ft847 out of Tx mode for digital
int ft847_FETMOXOff( void ) {
  //if (ft847_initGPIO()) { return -1; }
  if (GPIOInit == -1) { return -1; }            // if GPIO hasn't been initialized
  int iii = GPIOWrite(POUT, LOW);
  if (ft847_shutdownGPIO()) { return -1; }      // close GPIO so another program can use it.
  return iii;
}


static int GPIOExport(int pin)
{
  char buffer[BUFFER_MAX];
  ssize_t bytes_written;
  int fd;

  fd = open("/sys/class/gpio/export", O_WRONLY);
  if (-1 == fd) {
    fprintf(stderr, "Failed to open export for writing!\n");
    return(-1);
  }

  bytes_written = snprintf(buffer, BUFFER_MAX, "%d", pin);
  write(fd, buffer, bytes_written);
  close(fd);

  // For some reason there needs to be a delay after writing to ../gpio/export so that it can setup the subdirectories and files.
  usleep(100000);
  return(0);
}


static int GPIOUnexport(int pin)
{
  char buffer[BUFFER_MAX];
  ssize_t bytes_written;
  int fd;

  fd = open("/sys/class/gpio/unexport", O_WRONLY);
  if (-1 == fd) {
    fprintf(stderr, "Failed to open unexport for writing!\n");
    return(-1);
  }

  bytes_written = snprintf(buffer, BUFFER_MAX, "%d", pin);
  write(fd, buffer, bytes_written);
  close(fd);
  return(0);
}


static int GPIODirection(int pin, int dir)
{
  static const char s_directions_str[]  = "in\0out";

  char path[DIRECTION_MAX];
  int fd;

  snprintf(path, DIRECTION_MAX, "/sys/class/gpio/gpio%d/direction", pin);
  fd = open(path, O_WRONLY);
  if (-1 == fd) {
    fprintf(stderr, "Failed to open gpio direction for writing!\n");
    return(-1);
  }

  if (-1 == write(fd, &s_directions_str[IN == dir ? 0 : 3], IN == dir ? 2 : 3)) {
    fprintf(stderr, "Failed to set direction!\n");
    return(-1);
  }

  close(fd);
  return(0);
}

/*  Unused here
static int GPIORead(int pin)
{
  char path[VALUE_MAX];
  char value_str[3];
  int fd;

  snprintf(path, VALUE_MAX, "/sys/class/gpio/gpio%d/value", pin);
  fd = open(path, O_RDONLY);
  if (-1 == fd) {
    fprintf(stderr, "Failed to open gpio value for reading!\n");
    return(-1);
  }

  if (-1 == read(fd, value_str, 3)) {
    fprintf(stderr, "Failed to read value!\n");
    return(-1);
  }

  close(fd);

  return(atoi(value_str));
}
*/

static int GPIOWrite(int pin, int value)
{
  static const char s_values_str[] = "01";

  char path[VALUE_MAX];
  int fd;

  snprintf(path, VALUE_MAX, "/sys/class/gpio/gpio%d/value", pin);
  fd = open(path, O_WRONLY);
  if (-1 == fd) {
    fprintf(stderr, "Failed to open gpio value for writing!\n");
    return(-1);
  }

  if (1 != write(fd, &s_values_str[LOW == value ? 0 : 1], 1)) {
    fprintf(stderr, "Failed to write value!\n");
    return(-1);
  }

  close(fd);
  return(0);
}


static int ft847_initGPIO( void ) {
  if (-1 == GPIOExport(POUT)) { return(-1); }
  usleep(10000);                                        // slight delay because apparently sometimes the RPI doesn't finish quick enough.
  if (-1 == GPIODirection(POUT, OUT)) { return(-2); }   // set to output
  usleep(10000);
  if (-1 == GPIOWrite(POUT, LOW)) { return(-3); }       // turn FET off, should be already
  GPIOInit = 1;
  return 0;
}


static int ft847_shutdownGPIO( void ) {
  if (-1 == GPIOUnexport(POUT)) { return(-4); }
  GPIOInit = -1;
  return 0;
}



//#define MAIN_HERE 1
#ifdef MAIN_HERE

int main() {
  /*
  if (ft847_open() == -1) { return -1; }

  if (ft847_FETMOXOn() < 0) { return -1; }
  printf("  GPIO %d, value %d\n", POUT, GPIORead(POUT));

  if (ft847_CATOn() == -1) { return -1; }
  sleep(5);
  if (ft847_CATOff() == -1) { return -1; }

  if (ft847_FETMOXOff() < 0) { return -1; }
  printf("  GPIO %d, value %d\n", POUT, GPIORead(POUT));

  ft847_close();
  */
  if (ft847_open() == -1) { return -1; }
  if (ft847_CATOn() == -1) { return -1; }
  if (ft847_writeFreqHz( 7074000 ) == -1) { return -1; }
  if (ft847_CATOff() == -1) { return -1; }
  return 0;
}
#endif

