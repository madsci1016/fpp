/*
 *   FPD output handler for Falcon Pi Player (FPP)
 *
 *   Copyright (C) 2013 the Falcon Pi Player Developers
 *      Initial development by:
 *      - David Pitts (dpitts)
 *      - Tony Mace (MyKroFt)
 *      - Mathew Mrosko (Materdaddy)
 *      - Chris Pinkham (CaptainMurdoch)
 *      For additional credits and developers, see credits.php.
 *
 *   The Falcon Pi Player (FPP) is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "E131.h"
#include "falcon.h"
#include "FPD.h"
#include "log.h"
#include "sequence.h"
#include "settings.h"

#ifdef USEWIRINGPI
#	include "wiringPi.h"
#	include "wiringPiSPI.h"
#else
#	define wiringPiSPISetup(a,b)          1
#	define wiringPiSPIDataRW(a,b,c)       c
#	define delayMicroseconds(a)           0
#endif

#define MAX_PIXELNET_DMX_PORTS          12 
#define PIXELNET_DMX_DATA_SIZE          32768
#define PIXELNET_HEADER_SIZE            6       
#define PIXELNET_DMX_BUF_SIZE           (PIXELNET_DMX_DATA_SIZE+PIXELNET_HEADER_SIZE)

#define PIXELNET_DMX_COMMAND_CONFIG     0
#define PIXELNET_DMX_COMMAND_DATA       0xFF

typedef struct fpdPrivData {
	unsigned char inBuf[PIXELNET_DMX_DATA_SIZE];
	unsigned char outBuf[PIXELNET_DMX_BUF_SIZE];

	int  threadIsRunning;
	int  runThread;
	int  dataWaiting;

	pthread_t       processThreadID;
	pthread_mutex_t bufLock;
	pthread_mutex_t sendLock;
	pthread_cond_t  sendCond;
} FPDPrivData;

typedef struct {
	char active;
	char type;
	int startChannel;
} PixelnetDMXentry;


pthread_t pixelnetDMXthread;
char PixelnetDMXcontrolHeader[] = {0x55,0x55,0x55,0x55,0x55,0xCC};
char PixelnetDMXdataHeader[] =    {0xCC,0xCC,0xCC,0xCC,0xCC,0x55};


PixelnetDMXentry pixelnetDMX[MAX_PIXELNET_DMX_PORTS];
int pixelnetDMXcount =0;
int pixelnetDMXactive = 0;

/////////////////////////////////////////////////////////////////////////////
// Prototypes for some functions below
int FPD_StartOutputThread(void *data);
int FPD_StopOutputThread(void *data);

/////////////////////////////////////////////////////////////////////////////

/*
 *
 */
void FPD_Dump(FPDPrivData *privData)
{
	LogDebug(VB_CHANNELOUT, "  privData: %p\n", privData);

	if (!privData)
		return;

	LogDebug(VB_CHANNELOUT, "    threadIsRunning: %d\n", privData->threadIsRunning);
	LogDebug(VB_CHANNELOUT, "    runThread      : %d\n", privData->runThread);
}

/*
 *
 */
int SendOutputBuffer(FPDPrivData *privData)
{
	LogDebug(VB_CHANNELDATA, "SendOutputBuffer()\n");

	int i;
	unsigned char *c = privData->outBuf + PIXELNET_HEADER_SIZE;

	memcpy(privData->outBuf, PixelnetDMXdataHeader, PIXELNET_HEADER_SIZE);

	pthread_mutex_lock(&privData->bufLock);
	memcpy(c, privData->inBuf, PIXELNET_DMX_DATA_SIZE);
	privData->dataWaiting = 0;
	pthread_mutex_unlock(&privData->bufLock);

int checkValues = 1;
if ((!IsSequenceRunning()) ||
    ((prevSeqData[0] == -1) && (prevSeqData[FPPD_MAX_CHANNELS-1] == -1) &&
     (prevSeqData[1] == -2) && (prevSeqData[FPPD_MAX_CHANNELS-2] == -2) &&
     (prevSeqData[2] == -3) && (prevSeqData[FPPD_MAX_CHANNELS-3] == -3) &&
     (prevSeqData[3] == -4) && (prevSeqData[FPPD_MAX_CHANNELS-4] == -4) &&
     (prevSeqData[4] == -5) && (prevSeqData[FPPD_MAX_CHANNELS-5] == -5)))
checkValues = 0;

	for(i = 0; i < PIXELNET_DMX_DATA_SIZE; i++, c++)
	{
if (checkValues && *c != prevSeqData[i])
LogDebug(VB_SETTING, "FPD outBuf (%d) != seqData[%d] (%d)\n", *c, i, prevSeqData[i]);
		if (*c == 170)
		{
			*c = 171;
		}
	}

	if (LogMaskIsSet(VB_CHANNELDATA) && LogLevelIsSet(LOG_EXCESSIVE))
		HexDump("FPD Channel Header & Data", privData->outBuf, 256);

	i = wiringPiSPIDataRW (0, privData->outBuf, PIXELNET_DMX_BUF_SIZE);
	if (i != PIXELNET_DMX_BUF_SIZE)
	{
		LogErr(VB_CHANNELOUT, "Error: wiringPiSPIDataRW returned %d, expecting %d\n", i, PIXELNET_DMX_BUF_SIZE);
		return 0;
	}

	return 1;
}

/*
 *
 */
void *RunFPDOutputThread(void *data)
{
	LogDebug(VB_CHANNELOUT, "RunFPDOutputThread()\n");

	long long wakeTime = GetTime();
	struct timeval  tv;
	struct timespec ts;

	FPDPrivData *privData = (FPDPrivData *)data;

	privData->threadIsRunning = 1;
	LogDebug(VB_CHANNELOUT, "FPD output thread started\n");

	while (privData->runThread)
	{
		// Wait for more data
		pthread_mutex_lock(&privData->sendLock);
		LogExcess(VB_CHANNELOUT, "FPD output thread: sent: %lld, elapsed: %lld\n",
			GetTime(), GetTime() - wakeTime);

		pthread_mutex_lock(&privData->bufLock);
		if (privData->dataWaiting)
		{
			pthread_mutex_unlock(&privData->bufLock);

			gettimeofday(&tv, NULL);
			ts.tv_sec = tv.tv_sec;
			ts.tv_nsec = (tv.tv_usec + 200000) * 1000;

			if (ts.tv_nsec >= 1000000000)
			{
				ts.tv_sec  += 1;
				ts.tv_nsec -= 1000000000;
			}

			pthread_cond_timedwait(&privData->sendCond, &privData->sendLock, &ts);
		}
		else
		{
			pthread_mutex_unlock(&privData->bufLock);
			pthread_cond_wait(&privData->sendCond, &privData->sendLock);
		}

		wakeTime = GetTime();
		LogExcess(VB_CHANNELOUT, "FPD output thread: woke: %lld\n", GetTime());
		pthread_mutex_unlock(&privData->sendLock);

		if (!privData->runThread)
			continue;

		// See if there is any data waiting to process or if we timed out
		pthread_mutex_lock(&privData->bufLock);
		if (privData->dataWaiting)
		{
			pthread_mutex_unlock(&privData->bufLock);

			SendOutputBuffer(privData);
		}
		else
		{
			pthread_mutex_unlock(&privData->bufLock);
		}
	}

	LogDebug(VB_CHANNELOUT, "FPD output thread complete\n");
	privData->threadIsRunning = 0;
}

/*
 *
 */
void CreatePixelnetDMXfile(const char * file)
{
	FILE *fp;
	char settings[1024];
	char command[32];
	int i;
	int startChannel=1;
	fp = fopen(file, "w");
	if ( ! fp )
	{
		LogErr(VB_CHANNELOUT, "Error: Unable to create pixelnet file.\n");
		exit(EXIT_FAILURE);
	}
	LogDebug(VB_CHANNELOUT, "Creating file: %s\n",file);
  
  bzero(settings,1024);
  // File Header
  settings[0] = 0x55;
  settings[1] = 0x55;
  settings[2] = 0x55;
  settings[3] = 0x55;
  settings[4] = 0x55;
  settings[5] = 0xCC;
  int index = 6;
  
  // Set first 8 to Pixelnet 
	for(i=0;i<8;i++,startChannel+=4096)
	{
    settings[index++] = 1;                        // Enabled
    settings[index++] = (char)(startChannel%256); // Start Address LSB
    settings[index++] = (char)(startChannel/256); // Start Address MSB
    settings[index++] = 0;                        // Type 0=Pixlenet, 1=DMX 
	}
  
  // Set next four to DMX 
	for(i=0,startChannel=1;i<4;i++,startChannel+=512)
	{
    settings[index++] = 1;                        // Enabled
    settings[index++] = (char)(startChannel%256); // Start Address LSB
    settings[index++] = (char)(startChannel/256); // Start Address MSB
    settings[index++] = 1;                        // Type 0=Pixlenet, 1=DMX 
	}


  fwrite(settings,1,1024,fp);
	fclose(fp);
	sprintf(command,"sudo chmod 775 %s",file);
	system(command);
}


int InitializePixelnetDMX()
{
	int err;
	LogInfo(VB_CHANNELOUT, "Initializing SPI for FPD output\n");


	if (!DetectFalconHardware(1))
	{
		LogWarn(VB_CHANNELOUT, "Unable to detect attached Falcon "
			"hardware, setting SPI speed to 8000000.\n");

		if (wiringPiSPISetup (0, 8000000) < 0)
		{
		    LogErr(VB_CHANNELOUT, "Unable to open SPI device\n") ;
			return 0;
		}

		LogWarn(VB_CHANNELOUT, "Sending FPD v1.0 config\n");
		SendFPDConfig();
	}

	return 1;
}

void SendFPDConfig()
{
	int i,index;
	unsigned char bufferPixelnetDMX[PIXELNET_DMX_BUF_SIZE];

	memset(bufferPixelnetDMX,0,PIXELNET_DMX_BUF_SIZE);
	memcpy(bufferPixelnetDMX,PixelnetDMXcontrolHeader,PIXELNET_HEADER_SIZE);
	index = PIXELNET_HEADER_SIZE;
	for(i=0;i<pixelnetDMXcount;i++)
	{
		bufferPixelnetDMX[index++] = pixelnetDMX[i].type;
		bufferPixelnetDMX[index++] = (char)(pixelnetDMX[i].startChannel%256);
		bufferPixelnetDMX[index++] = (char)(pixelnetDMX[i].startChannel/256);
	}

	if (LogMaskIsSet(VB_CHANNELOUT) && LogLevelIsSet(LOG_DEBUG))
		HexDump("FPD Config Header & Data", bufferPixelnetDMX,
			PIXELNET_HEADER_SIZE + (pixelnetDMXcount*3));

	i = wiringPiSPIDataRW (0, bufferPixelnetDMX, PIXELNET_DMX_BUF_SIZE);
	if (i != PIXELNET_DMX_BUF_SIZE)
		LogErr(VB_CHANNELOUT, "Error: wiringPiSPIDataRW returned %d, expecting %d\n", i, PIXELNET_DMX_BUF_SIZE);

	delayMicroseconds (10000) ;
//	i = wiringPiSPIDataRW (0, bufferPixelnetDMX, PIXELNET_DMX_BUF_SIZE);
//	if (i != PIXELNET_DMX_BUF_SIZE)
//		LogErr(VB_CHANNELOUT, "Error: wiringPiSPIDataRW returned %d, expecting %d\n", i, PIXELNET_DMX_BUF_SIZE);
}

/*
 *
 */
int FPD_Open(char *configStr, void **privDataPtr) {
	LogDebug(VB_CHANNELOUT, "FPD_Open()\n");

	if (!FileExists(getPixelnetFile())) {
		LogDebug(VB_CHANNELOUT, "FPD config file does not exist, creating it.\n");
		CreatePixelnetDMXfile(getPixelnetFile());
	}

	if (!InitializePixelnetDMX())
		return 0;

	FPDPrivData *privData =
		(FPDPrivData *)malloc(sizeof(FPDPrivData));
	if (privData == NULL)
	{
		LogErr(VB_CHANNELOUT, "Error %d allocating private memory: %s\n",
			errno, strerror(errno));
		return 0;
	}

	bzero(privData, sizeof(FPDPrivData));

	pthread_mutex_init(&privData->bufLock, NULL);
	pthread_mutex_init(&privData->sendLock, NULL);
	pthread_cond_init(&privData->sendCond, NULL);

	FPD_Dump(privData);

	*privDataPtr = privData;

	return 1;
}

/*
 *
 */
int FPD_Close(void *data) {
	LogDebug(VB_CHANNELOUT, "FPD_Close(%p)\n", data);

	FPDPrivData *privData = (FPDPrivData*)data;
	FPD_Dump(privData);

	FPD_StopOutputThread(privData);

	pthread_mutex_destroy(&privData->bufLock);
	pthread_mutex_destroy(&privData->sendLock);
	pthread_cond_destroy(&privData->sendCond);
}

/*
 *
 */
int FPD_IsConfigured(void) {
	LogDebug(VB_CHANNELOUT, "FPD_IsConfigured()\n");

	if (!getSettingInt("FPDEnabled"))
		return 0;

  pixelnetDMXactive = 1;
	return pixelnetDMXactive;
}

/*
 *
 */
int FPD_IsActive(void *data) {
	LogDebug(VB_CHANNELOUT, "FPD_IsActive(%p)\n", data);

	return pixelnetDMXactive;
}

/*
 *
 */
int FPD_SendData(void *data, char *channelData, int channelCount)
{
	LogDebug(VB_CHANNELDATA, "FPD_SendData(%p, %p, %d)\n",
		data, channelData, channelCount);

	FPDPrivData *privData = (FPDPrivData *)data;

	if (channelCount > PIXELNET_DMX_DATA_SIZE)
	{
		LogErr(VB_CHANNELOUT,
			"FPD_SendData() tried to send %d bytes when max is %d\n",
			channelCount, PIXELNET_DMX_DATA_SIZE);
		return 0;
	}

	// Copy latest data to our input buffer for processing
	pthread_mutex_lock(&privData->bufLock);
	memcpy(privData->inBuf, channelData, channelCount);
	privData->dataWaiting = 1;
	pthread_mutex_unlock(&privData->bufLock);

	if (privData->threadIsRunning)
		pthread_cond_signal(&privData->sendCond);
	else
		SendOutputBuffer(privData);

	return 1;
}

/*
 *
 */
int FPD_MaxChannels(void *data)
{
	(void)data;

	return 32768;
}

/*
 *
 */
int FPD_StartOutputThread(void *data)
{
	LogDebug(VB_CHANNELOUT, "FPD_StartOutputThread(%p)\n", data);

	FPDPrivData *privData = (FPDPrivData*)data;

	if (privData->processThreadID)
	{
		LogErr(VB_CHANNELOUT, "ERROR: thread already exists\n");
		return -1;
	}

	privData->runThread = 1;

	int result = pthread_create(&privData->processThreadID, NULL, &RunFPDOutputThread, privData);

	if (result)
	{
		char msg[256];

		privData->runThread = 0;
		switch (result)
		{
			case EAGAIN: strcpy(msg, "Insufficient Resources");
				break;
			case EINVAL: strcpy(msg, "Invalid settings");
				break;
			case EPERM : strcpy(msg, "Invalid Permissions");
				break;
		}
		LogErr(VB_CHANNELOUT, "ERROR creating Triks-C output thread: %s\n", msg );
	}

	while (!privData->threadIsRunning)
		usleep(10000);

	return 0;
}

/*
 *
 */
int FPD_StopOutputThread(void *data)
{
	LogDebug(VB_CHANNELOUT, "FPD_StopOutputThread(%p)\n", data);

	FPDPrivData *privData = (FPDPrivData*)data;

	if (!privData->processThreadID)
		return -1;

	privData->runThread = 0;

	pthread_cond_signal(&privData->sendCond);

	int loops = 0;
	// Wait up to 110ms for data to be sent
	while ((privData->dataWaiting) &&
	       (privData->threadIsRunning) &&
	       (loops++ < 11))
		usleep(10000);

	pthread_mutex_lock(&privData->bufLock);

	if (!privData->processThreadID)
	{
		pthread_mutex_unlock(&privData->bufLock);
		return -1;
	}

	pthread_join(privData->processThreadID, NULL);
	privData->processThreadID = 0;
	pthread_mutex_unlock(&privData->bufLock);

	return 0;
}

/*
 * Declare our external interface struct
 */
FPPChannelOutput FPDOutput = {
	.maxChannels  = FPD_MaxChannels,
	.open         = FPD_Open,
	.close        = FPD_Close,
	.isConfigured = FPD_IsConfigured,
	.isActive     = FPD_IsActive,
	.send         = FPD_SendData,
	.startThread  = FPD_StartOutputThread,
	.stopThread   = FPD_StopOutputThread,
	};

