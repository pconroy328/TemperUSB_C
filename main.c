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
 * 16-Dec-2015  - still messing around with Git and AWS Code Commit
 * 26-Jun-2020  - pulling out INI file stuff, adding mDNS
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
#include <libmqttrv.h>
#include <log4c.h>
//#include <libiniparser_pmc.h>



#ifndef FALSE
# define FALSE   0
#define TRUE    (!FALSE)
#endif



static  char    *version = "v4.3 [new libmqtt]";

//
//      compensation - number of degrees F to add or subtract from the
//                     device readings to make it more accurate.
//      My device seems about 1.5-2.5*F higher than the LaCrosse Ws2308
static  double  compensationDegreesF = -10.0;


//
//      tempReadInterval - number of seconds in between device sending temp
//                      readings
static  int     tempReadInterval = 120;



static  int     debug = FALSE;
static  int     debugLevel = 3;
static  int     skipIniFile = FALSE;

static  int     mqttPort = 1883;
static  char    *mqttTopic = "TEMPER";
static  int     MQTT_Connected = FALSE;

static  int     mqttHostSpecified = FALSE;
static  int     debugValue = 5;
static  char    mqttHost[ 1024 ];
static  int     deviceNum = 1;
static  char    location[ 1024 ];

static  struct  mosquitto   *aMosquittoInstance;




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
        return;
    }

    //
    //  get current date/time -- NOT THREAD SAFE CALLS! :)
    t = time( NULL );
    tmp = localtime( &t );

    //
    //  format it so it's easy to consume by mySQL YYYY-MM-DD HH:MM:SS
    strftime( timeStr, sizeof timeStr, "%FT%T%z", tmp );

    static  char    *jsonTemplate = "{ "
    "\"topic\":\"%s\","
    "\"version\":\"1.0\","
    "\"deviceNum\":%d,"
    "\"dateTime\":\"%s\","
    "\"location\":\"%s\","
    "\"temperature\":%.1f}";
    
    
    memset( buffer, '\0', sizeof buffer );
    int length = snprintf( buffer, sizeof buffer, jsonTemplate,
                mqttTopic,
                deviceNum,
                timeStr,
                location,
                deviceTemp
            );

    
    //puts( buffer );
    MQTT_Publish( aMosquittoInstance, mqttTopic, buffer, 0 );   
}

// ------------------------------------------------------------------
void     terminationHandler (int signalValue)
{
    Logger_LogDebug( "Termination Signal received...\n" );
    MQTT_Teardown( aMosquittoInstance, NULL );
    exit( 1 );
}


// --------------------------------------------------------------------------
void    help (void)
{
    puts( "Options are:" );
    puts( "    -c <degrees F>       adjust temperature reading by  <+/- degrees F>");
    puts( "    -n <ID>              assign an ID number to this device" );
    puts( "    -l <Location>        assign a location to this device" );
    puts( "    -v <depth>           enables verbose debugging 1..5" );
    puts( "    -r <seconds>         sets temperature reading interval to <second>, max of 255" );
    puts( "    -h <server>          send MQTT data to this MQTT server" );
    puts( "    -m <mqtt port num>   use this port number for MQTT (eg 1883)" );
    puts( "    -t <topic>           use <topic> as the MQTT topic string" );
    
    
    puts( "" );
    
    puts( "" );
    puts( "Signals" );
    //puts( "  Sending USR1 causes the application to close, reset, reopen reread everything" );
    //puts( "  Sending USR2 causes the application to read the INI file" );
    
}   // help

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
    
    // printf( "............. %0.2f\n", *tempC );
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

    while (( (ch = getopt( argc, argv, "l:v:n:c:r:h:m:t:" )) != -1) && (ch != 255)) {
        switch (ch) {
            case 'c':   compensationDegreesF = (double) atof( optarg );
                        break;
            case 'r':   tempReadInterval = atoi( optarg );
                        break;
            case 'n':   deviceNum =  atoi( optarg );
                        break;
            case 'l':   strncpy( location, optarg, sizeof location );
                        break;
            case 'v':   debug = TRUE;
                        debugLevel = atoi( optarg );
                        break;
            case 'h':   strncpy( mqttHost, optarg, sizeof mqttHost );
                        mqttHostSpecified = TRUE;   
                        break;
            case 't':   mqttTopic = optarg;
                        break;
            case 'm':   mqttPort = atoi( optarg );
                        break;

            default:    help();
                        exit( 1 );
                        break;
        }
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

    //
    // Initialize values to some common, sensible defaults.
    mqttTopic = "TEMPER";
    strncpy( location , "RV", sizeof location );
    deviceNum = 1;
    
    printf( "TemperUSB Reader Version %s\n", version );
    parseCommandLine( argc, argv );
    
    Logger_Initialize( "/tmp/temperusb.log", debugValue );
    Logger_LogWarning( "%s\n", version );
    Logger_LogWarning( "libmqttrv version: %s\n", MQTT_GetLibraryVersion() );
    
    //
    // If they passed in an MQTTHost (with the -h option) then do NOT use avahi to find
    //  our broker.
    //
    if (!mqttHostSpecified) {
        //
        //  Not using -h - so find our broker using avahi
        Logger_LogDebug( "mDNS - Looking for an MQTT Broker in the RV first [60 seconds max]\n" );
        if (!MQTT_ConnectRV( &aMosquittoInstance, 60 )) {
            Logger_LogFatal( "Could not find an MQTT Broker via mDNS. Specify broker name on command line with -q option.\n" );
            Logger_Terminate();
            return( EXIT_FAILURE );
        }
        
        //
        // If we made it this far - we've got one
        strncpy( mqttHost, MQTT_GetCachedBrokerHostName(), sizeof mqttHost );
        mqttPort = MQTT_GetCachedBrokerPortNumber();
        Logger_LogInfo( "mDNS - Found an MQTT Broker on Host [%s], Port [%d]\n", mqttHost, mqttPort );
     
        //
        // MQTT_ConnectRV and MQTT_Connect() call MQTT_Initialize
        
    } else {
        Logger_LogDebug( "Direct Connect to specific broker- Looking for an MQTT Broker on Host [%s], Port [%d]\n", mqttHost, mqttPort );
        
        if (!MQTT_Initialize( mqttHost, mqttPort, &aMosquittoInstance )) {
            Logger_LogFatal( "Could not find an MQTT Broker on Host [%s], Port [%d]\n", mqttHost, mqttPort );
            Logger_Terminate();
            return( EXIT_FAILURE );
        }
    }
    
    MQTT_Connected = TRUE;
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

        //  Since I'm in the United States - lets convert to Fahrenheit too!
        //      Tf = (9/5)*Tc+32; Tc = temperature in degrees Celsius, Tf = temperature in degrees Fahrenhei
        tempF = (((9.0 / 5.0) * tempC) + 32.0);
        tempF += compensationDegreesF;

        //MQTT_SendReceive( aMosquittoInstance );
        mqttPublish( tempF );

        sleep( tempReadInterval );
    }       // while !done

    MQTT_Teardown( aMosquittoInstance, mqttTopic );
    Logger_Terminate();

    return EXIT_SUCCESS;
}

