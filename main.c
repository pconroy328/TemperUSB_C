/* 
 * File:   main.c
 * Author: patrick
 *
 * Created on March 19, 2010, 8:15 PM
        pconroy@gx100:~/www$ lsusb
        Bus 003 Device 002: ID 1130:660c Tenx Technology, Inc.
 *
 * Rudimentary program to see if I can pull Temperature Data from the TemperHID
 * device a pure USB device.
 *
 * NB: make sure you've installed libusb and libusb-dev packages
 * 
 * 09-Dec-2013  - adding in MQTT support!
 * 02-Dec-2015  - extensive cleanup - pulling in more common logging, MQTT and INI file libraries
 * 08-Dec-2015  - messing around with Git and AWS Code Commit
 * 14-Dec-2015  - still messing around with Git and AWS Code Commit
 */
#define _POSIX_SOURCE

#include <stdlib.h>
#include <stdio.h>              /* String function definitions */
#include <string.h>             /* String function definitions */
#include <unistd.h>             /* UNIX standard function definitions */
#include <fcntl.h>              /* File control definitions */
#include <errno.h>              /* Error number definitions */
#include <termios.h>            /* POSIX terminal control definitions */
#include <time.h>
#include <signal.h>
#include <getopt.h>

#include <usb.h>

#include "temperusb.h"
#include <libmqtt_pmc.h>
#include <liblogger_pmc.h>
#include <libiniparser_pmc.h>



#ifndef FALSE
# define FALSE   0
#define TRUE    (!FALSE)
#endif



static  char    *version = "v2.0 [Common Libraries]";
static  char    *defaultINIFileName = "TemperUSB.ini";
static  char    iniFileName[ 1024 ];


//
//      compensation - number of degrees F to add or subtract from the
//                     device readings to make it more accurate.
//      My device seems about 1.5-2.5*F higher than the LaCrosse Ws2308
static  double  compensationDegreesF = -1.5;


//
//      tempReadInterval - number of seconds in between device sending temp
//                      readings
static  int     tempReadInterval = 120;


//
//      sensor name - an identification that tells where this sensor is located
static  char                *sensorName;


static  int                 debug = FALSE;
static  int                 skipIniFile = FALSE;

char                        logFileName[ 256 ];
static  int                 logFileOpen = FALSE;
static  FILE                *logFile = NULL;

static  char                *mqttServerName;
static  int                 mqttPort = 1883;
static  char                *mqttTopic;
static  int                 mqttQoS = 0;
static  int                 mqttRetainMsgs = FALSE;
static  int                 mqttTimeOut = 300;

static  int                 MQTT_Connected = FALSE;

static  struct  mosquitto   *myMQTTInstance;



//
//  These are the USB device codes and how we find our TemperUSB device in the system
#define VENDOR_ID   0x1130
#define PRODUCT_ID  0x660c
#define USB_TIMEOUT 1000                /* milliseconds */


struct Temper {
        struct usb_device   *device;
        usb_dev_handle      *handle;
        int                 debug;
        int                 timeout;
};



// -------------------------------------------------------------------------------------
static
void    mqttPublish (double deviceTemp)
{
    char            timeStr[ 50 ];
    time_t          t;
    struct tm       *tmp;
    char            buffer[ 1024 ];

    if (!MQTT_Connected) {
        Logger_LogDebug( "TEMPER: Error: Attempt to publish Weather Reading using MQTT. Broker not connected\n" );
        fprintf( stderr, "TEMPER: Error: Attempt to publish Weather Reading using MQTT; Broker not connected\n" );
        return;
    }

    //
    //  get current date/time -- NOT THREAD SAFE CALLS! :)
    t = time( NULL );
    tmp = localtime( &t );

    //
    //  format it so it's easy to consume by mySQL YYYY-MM-DD HH:MM:SS
    strftime( timeStr, sizeof timeStr, "%Y-%m-%d %H:%M:%S %z", tmp );

    
    memset( buffer, '\0', sizeof buffer );
    int length = snprintf( buffer, sizeof buffer,
                "%s | %s | %s | Temp %3.1f |",
                mqttTopic,
                timeStr,
                sensorName,
                deviceTemp
            );

    
    MQTT_Publish( myMQTTInstance, mqttTopic, buffer, mqttQoS );   
}

// ------------------------------------------------------------------
void     terminationHandler (int signalValue)
{
    Logger_LogDebug( "Termination Signal received..." );
    MQTT_Teardown( myMQTTInstance, NULL );
    exit( 1 );
}

// ----------------------------------------------------------------------------
static
int fileExists (const char * fileName)
{   
    return (access( fileName, R_OK )  == 0 );
}

// ------------------------------------------------------------------
char    *findIniFile (const char *fileName, int *found)
{
    char    buffer[ 1024 ];
    
    if (fileName == NULL) {
        strncpy( buffer, defaultINIFileName, sizeof buffer );
    
        Logger_LogDebug( "No INI filename specified - using default value:" );
        Logger_LogDebug( defaultINIFileName );
    } else {
        strncpy( buffer, fileName, sizeof buffer );
    }
    
    //
    // Can we find it?
    if (fileExists( buffer ) ) {
        strncpy( iniFileName, buffer, sizeof iniFileName );
        *found = TRUE;
        return iniFileName;
    
    } else {
        //
        // Not found - look in some other common places
        // 1. Add "./INI/ and look again
        strcpy( buffer, "./INI/" );
        strncat( buffer, fileName, sizeof buffer );
        if (fileExists( buffer ) ) {
            strncpy( iniFileName, buffer, sizeof iniFileName );
            *found = TRUE;
            return iniFileName;
        }
        
        //
        // 2. Back up one and look for ../INI/...
        strcpy( buffer, "../INI/" );
        strncat( buffer, fileName, sizeof buffer );
        if (fileExists( buffer ) ) {
            strncpy( iniFileName, buffer, sizeof iniFileName );
            *found = TRUE;
            return iniFileName;
        }
        
        //
        // 3. Look in /usr/local/INI
        strcpy( buffer, "/usr/local/INI/" );
        strncat( buffer, fileName, sizeof buffer );
        if (fileExists( buffer ) ) {
            strncpy( iniFileName, buffer, sizeof iniFileName );
            *found = TRUE;
            return iniFileName;
        }
       
        Logger_LogDebug( "Unable to find the INI file in any location" );
        fprintf( stderr, "Unable to find the INI file in any location [%s]\n", fileName );
        *found = FALSE;
    }
    
    return defaultINIFileName;
}

// ---------------------------------------------------------------------------
void    readIniFile (void)
{
    int     found = FALSE;
    
    findIniFile( iniFileName, &found );

    if (found) {
        dictionary  *iniDictionary = NULL;
        iniDictionary = iniparser_load( iniFileName );

        //
        //  Debugging my new iniparser - it looks like a section : key pair is required/
        //  Perhaps to use just a key (no section) prefix the get with just a ";"
        //)
        sensorName = iniparser_getstring( iniDictionary, "TemperUSB:deviceName", "TEMPERUSB" );
        compensationDegreesF = iniparser_getdouble( iniDictionary, "TemperUSB:tempAdjust", compensationDegreesF );
        tempReadInterval = iniparser_getint( iniDictionary, "TemperUSB:readInterval", tempReadInterval );
        debug = iniparser_getboolean( iniDictionary, "TemperUSB:debug", FALSE );

        mqttServerName = iniparser_getstring( iniDictionary, "MQTT:brokerHostName", "192.168.1.11" );
        mqttPort = iniparser_getint( iniDictionary, "MQTT:brokerPortNum", 1883 );
        mqttTopic = iniparser_getstring( iniDictionary, "MQTT:MQTTTopic", "TEMPERUSB" );
        mqttQoS = iniparser_getint( iniDictionary, "MQTT:MQTTQoS", 0 );
        mqttTimeOut = iniparser_getint( iniDictionary, "MQTT:MQTTTimeOut", 0 );
        mqttRetainMsgs = iniparser_getboolean( iniDictionary, "MQTT:MQTTRetainMsgs", FALSE );
    
        if (debug) {
            Logger_LogDebug( "After finding and reading the INI file\n" );
            Logger_LogDebug( "  compensationDegreesF:   %f\n", compensationDegreesF );
            Logger_LogDebug( "  iniFileName:            %s\n", iniFileName );
            Logger_LogDebug( "  tempReadInterval:       %d\n", tempReadInterval );
            Logger_LogDebug( "  sensorName:             %s\n", sensorName );
            Logger_LogDebug( "  skipIniFile:            %s\n", (skipIniFile ? "Yes" : "No" ) );
            Logger_LogDebug( "  brokerHostName:         %s\n", mqttServerName );
            Logger_LogDebug( "  mqttTopic:              %s\n", mqttTopic );
            Logger_LogDebug( "  brokerPortNum:          %d\n", mqttPort );
            Logger_LogDebug( "  logFileName:            %s\n", logFileName );
        }
        
    } else {
        Logger_LogError( "Was expecting an INI file but could not find it! Filename: [%s]\n", iniFileName );
    }
}

// --------------------------------------------------------------------------
void    help (void)
{
    puts( "Options are:" );
    puts( "    -c <degrees F>       adjust temperature reading by  <+/- degrees F>");
    puts( "    -r <seconds>         sets temperature reading interval to <second>, max of 255" );
    puts( "    -n <ID>              assign an Name to this device" );
    puts( "    -v                   enables verbose debugging" );
    puts( "    -i <fileName>        pull INI parameters from this file" );
    puts( "    -x                   ignore the INI file contents" );
    puts( "    -s <server>          send MQTT data to this MQTT server, (eg '192.168.0.11')" );
    puts( "    -m <mqtt port num>   use this port number for MQTT (eg 1883)" );
    puts( "    -t <topic>           use <topic> as the MQTT topic string" );
    puts( "    -z <seconds>         MQTT connection timeout setting" );
    puts( "    -f <fileName>        write debug and log data to this file" );
    puts( "" );
    
    puts( "INI File parameters:");
  printf( "  Default INI File Name: [%s]\n", defaultINIFileName );
    puts( "  [TemperUSB] - deviceName - assign a name to this device" );
    puts( "  [TemperUSB] - tempAdjust - adjust temp by <+/- degrees F> (-c)" );
    puts( "  [TemperUSB] - readInterval - time (seconds) between sampling (-r)" );
    puts( "  [TemperUSB] - debug - (dis)/(en)able verbose debugging (-v)" );
    puts( "  [TemperUSB] - MQTTServer - see dash s" );
    puts( "  [TemperUSB] - MQTTTopic - see dash t" );
    puts( "  [TemperUSB] - MQTTPort - see dash m" );
    puts( "  [TemperUSB] - MQTTTimeOut - see dash z" );
    puts( "  [TemperUSB] - MQTTQoS - MQTT Quality of Service: 0, 1 or 2" );
    puts( "  [TemperUSB] - MQTTRetainMsgs - MQTT Retain Messages: True or False" );
    puts( "  [TemperUSB] - logFile - write debug data to this file" );
    
    puts( "" );
    puts( "Signals" );
    //puts( "  Sending USR1 causes the application to close, reset, reopen reread everything" );
    //puts( "  Sending USR2 causes the application to read the INI file" );
    
}   // help




// ------------------------------------------------------------------
void     sigUsr1Handler (int signalValue)
{
    //
    //
    fprintf( stderr, "Signal USR1 received - Stopping...\n" );
    fprintf( stderr, "Reloading INI file data...\n" );
    readIniFile();

    fprintf( stderr, "Restarting everything...\n" );
    fprintf( stderr, "Done!\n" );
}

// -------------------------------------------------------------------------------------
Temper *TemperCreate(struct usb_device *dev, int timeout, int debug)
{
        Temper  *t = NULL;
        int     ret;

        t = calloc(1, sizeof( *t ) );
        t->device = dev;
        t->debug = debug;
        t->timeout = timeout;
        t->handle = usb_open( t->device );
        
        if (!t->handle) {
            free( t );
            return NULL;
        }
        
        if (t->debug) {
            Logger_LogDebug( "Trying to detach kernel driver\n" );
        }

        ret = usb_detach_kernel_driver_np( t->handle, 0 );
        if (ret) {
            if (errno == ENODATA) {
               if (t->debug) {
                    Logger_LogDebug( "Device already detached\n" );
                }
            } else {
                if (t->debug) {
                   Logger_LogDebug( "Detach failed: %s[%d]\n", strerror( errno ), errno );
                   Logger_LogDebug( "Continuing anyway\n" );
                }
            }
        } else {
            if (t->debug) {
                Logger_LogDebug( "detach successful\n" );
            }
        }
        
        ret = usb_detach_kernel_driver_np( t->handle, 1 );
        if (ret) {
            if (errno == ENODATA) {
                if (t->debug)
                   Logger_LogDebug( "Device already detached\n" );
            } else {
                if (t->debug) {
                    Logger_LogDebug( "Detach failed: %s[%d]\n", strerror( errno ), errno );
                    Logger_LogDebug( "Continuing anyway\n" );
                }
            }
        } else {
            if (t->debug) {
                Logger_LogDebug( "detach successful\n" );
            }
        }

        if (usb_set_configuration( t->handle, 1) < 0 ||
            usb_claim_interface( t->handle, 0) < 0 ||
            usb_claim_interface( t->handle, 1)) {
                usb_close(t->handle);
                free( t );
                return NULL;
        }
        return t;
}

// -----------------------------------------------------------------------------
Temper *TemperCreateFromDeviceNumber(int deviceNum, int timeout, int debug)
{
    struct usb_bus *bus;
    int n;

    n = 0;
    for ( bus = usb_get_busses(); bus; bus=bus->next) {
        struct usb_device *dev;

        for (dev = bus->devices; dev; dev=dev->next) {
            if (debug) {
                Logger_LogDebug( "Found device: %04x:%04x\n", dev->descriptor.idVendor, dev->descriptor.idProduct );
            }
            
            if (dev->descriptor.idVendor == VENDOR_ID && dev->descriptor.idProduct == PRODUCT_ID) {
                if(debug) {
                    Logger_LogDebug( "Found deviceNum %d\n", n );
                }
                
                if (n == deviceNum) {
                    return TemperCreate (dev, timeout, debug );
                }
                
                n++;
            }
        }
    }
    
    return NULL;
}

// ------------------------------------------------------------------------------------
void TemperFree(Temper *t)
{
    if (t) {
        if (t->handle) {
            usb_close(t->handle);
        }
        free( t );
    }
}

// -----------------------------------------------------------------------------
static int TemperSendCommand (Temper *t, int a, int b, int c, int d, int e, int f, int g, int h)
{
    unsigned char   buf[ 32 ];
    int             ret;

    memset( buf, 0, 32 );
    buf[ 0 ] = a;
    buf[ 1 ] = b;
    buf[ 2 ] = c;
    buf[ 3 ] = d;
    buf[ 4 ] = e;
    buf[ 5 ] = f;
    buf[ 6 ] = g;
    buf[ 7 ] = h;

    ret = usb_control_msg(t->handle, 0x21, 9, 0x200, 0x01, (char *) buf, 32, t->timeout);
    
    if(ret != 32) {
        Logger_LogError( "usb_control_msg failed" );
        return -1;
    }
    
    return 0;
}

// -----------------------------------------------------------------------------
static int TemperGetData (Temper *t, char *buf, int len)
{
    int ret;

    return usb_control_msg( t->handle, 0xa1, 1, 0x300, 0x01, (char *) buf, len, t->timeout );
}

// -----------------------------------------------------------------------------
int TemperGetTemperatureInC(Temper *t, double *tempC)
{
    char buf[ 256 ];
    int ret, temperature, i;

    TemperSendCommand( t, 10, 11, 12, 13, 0, 0, 2, 0 );
    TemperSendCommand( t, 0x54, 0, 0, 0, 0, 0, 0, 0 );
    
    for (i = 0; i < 7; i++) {
        TemperSendCommand(t, 0, 0, 0, 0, 0, 0, 0, 0 );
    }
    
    TemperSendCommand(t, 10, 11, 12, 13, 0, 0, 1, 0 );
    
    ret = TemperGetData(t, buf, 256);
    if (ret < 2) {
        return -1;
    }

    temperature = (buf[ 1 ] & 0xFF) + (buf[ 0 ] << 8);
    temperature += 1152;                    // calibration value
    *tempC = temperature * (125.0 / 32000.0);
    return 0;
}

// -----------------------------------------------------------------------------
int TemperGetOtherStuff(Temper *t, char *buf, int length)
{
    TemperSendCommand( t, 10, 11, 12, 13, 0, 0, 2, 0 );
    TemperSendCommand( t, 0x52, 0, 0, 0, 0, 0, 0, 0 );
    TemperSendCommand( t, 10, 11, 12, 13, 0, 0, 1, 0 );
    return TemperGetData( t, buf, length );
}


// -----------------------------------------------------------------------------
static
void    parseCommandLine (int argc, char *argv[])
{
    int     ch;
    opterr = 0;

    while (( (ch = getopt( argc, argv, "xvn:c:r:s:i:s:m:t:f:" )) != -1) && (ch != 255)) {
        switch (ch) {
            case 'c':   compensationDegreesF = (double) atof( optarg );
                        break;
            case 'i':   strncpy( iniFileName, optarg, sizeof iniFileName );
                        break;
            case 'r':   tempReadInterval = atoi( optarg );
                        break;
            case 'n':   sensorName =  optarg;
                        break;
            case 'v':   debug = TRUE;
                        break;
            case 'x':   skipIniFile = TRUE;
                        break;
            case 's':   mqttServerName = optarg;
                        break;
            case 't':   mqttTopic = optarg;
                        break;
            case 'm':   mqttPort = atoi( optarg );
                        break;
            case 'f':   strncpy( logFileName, optarg, sizeof( logFileName ) );
                        break;

            default:    help();
                        exit( 1 );
                        break;
        }
    }
    
    if (debug) {
        Logger_LogDebug( "After command line parsing\n" );
        Logger_LogDebug( "  compensationDegreesF:   %f\n", compensationDegreesF );
        Logger_LogDebug( "  iniFileName:            %s\n", iniFileName );
        Logger_LogDebug( "  tempReadInterval:       %d\n", tempReadInterval );
        Logger_LogDebug( "  sensorName:             %s\n", sensorName );
        Logger_LogDebug( "  skipIniFile:            %s\n", (skipIniFile ? "Yes" : "No" ) );
        Logger_LogDebug( "  mqttServerName:         %s\n", mqttServerName );
        Logger_LogDebug( "  mqttTopic:              %s\n", mqttTopic );
        Logger_LogDebug( "  mqttPort:               %d\n", mqttPort );
        Logger_LogDebug( "  logFileName:            %s\n", logFileName );
    }

}


// --------------------------------------------------------------------------
int main(int argc, char** argv)
{

    int                 done = FALSE;
    double              tempC = 0.0;
    double              tempF = 0.0;
    Temper              *t = NULL;
    char                buf[ 256 ];
    int                 ret;
    struct sigaction    sa;

    //
    // Install signal handlers so we exit gracefully
    /* Set up the structure to specify the new action. */
    sa.sa_handler = terminationHandler;
    sigemptyset ( &sa.sa_mask );
    sa.sa_flags = 0;

    if (sigaction ( SIGINT, &sa, NULL ) == -1)
        fprintf( stderr, "Unable to install SIGNINT handler!\n" );
    if (sigaction ( SIGTERM, &sa, NULL ) == -1)
        fprintf( stderr, "Unable to install SIGTERM handler!\n" );


    //
    // Initialize values to some common, sensible defaults.
    sensorName = "TEMPERUSB";
    strncpy( iniFileName, defaultINIFileName, sizeof iniFileName  );

    printf( "TemperUSB Reader Version %s\n", version );
    parseCommandLine( argc, argv );
    
    Logger_Initialize( "temperusb.log", 5 );  

    
    if (!skipIniFile) {
        readIniFile();
        MQTT_InitializeFromINIFile( iniFileName, &myMQTTInstance );
    } else {
        MQTT_Initialize( mqttServerName, mqttPort, &myMQTTInstance );
    }

    usb_set_debug( 0 );
    usb_init();
    usb_find_busses();
    usb_find_devices();

    t = TemperCreateFromDeviceNumber( 0, USB_TIMEOUT, (debug ? 1 : 0 ) );
    if (!t) {
        Logger_LogFatal( "TemperCreateFromDeviceNumber failed\n" );
        exit( -1 );
    }


    //
    //  I doubt this is necessary but it was in the example code
    memset( buf, 0, 256 );
    ret = TemperGetOtherStuff( t, buf, 256 );


    //
    //  Now loop forever reading temps from the device
    while (!done) {

        if (TemperGetTemperatureInC( t, &tempC ) < 0) {
            Logger_LogFatal( "TemperGetTemperatureInC failed\n" );
            exit( 1 );
        }

        //  Since I'm in the United States - lets convert to Farenheit too!
        //      Tf = (9/5)*Tc+32; Tc = temperature in degrees Celsius, Tf = temperature in degrees Fahrenhei
        tempF = (((9.0 / 5.0) * tempC) + 32.0);
        tempF += compensationDegreesF;

        //
        mqttPublish( tempF );

        sleep( tempReadInterval );
    }       // while !done

    MQTT_Teardown( myMQTTInstance, mqttTopic );
    Logger_Terminate();

    return EXIT_SUCCESS;
}

