#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
//#include <3ds.h>
//#include <citro3d.h>

#define __BYTEBOOL__
#define boolean bool

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_main.h"
#include "../m_menu.h"
#include "../i_system.h"
#include "../i_video.h"
#include "../i_sound.h"
#include "../i_joy.h"
#include "nds_utils.h"
#include "r_queue.h"

UINT8 graphics_started = 0;
UINT8 keyboard_started = 0;

bool isNew3DS = false;

static INT64 start_time; // as microseconds since the epoch

#define CPAD_MAX_DELTA	160

u32 __stacksize__ = 0x180000;
u32 __ctru_linear_heap_size = 48 * 1024 * 1024;

float sliderState = 0.0f;
//----------------------------

size_t I_GetFreeMem(size_t *total)
{
	*total = 128 * 1024 * 1024;
	
	return linearSpaceFree() + vramSpaceFree();
}

static struct timespec basetime;

void I_SleepToTic(tic_t tic)
{
	struct timespec ts;
	uint64_t curtime, targettime;
	int status;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	ts.tv_sec -= basetime.tv_sec;
	ts.tv_nsec -= basetime.tv_nsec;
	if (ts.tv_nsec < 0)
	{
		ts.tv_sec--;
		ts.tv_nsec += 1000000000;
	}
	curtime = ((uint64_t)ts.tv_sec * 1000000000) + ts.tv_nsec;
	targettime = ((uint64_t)tic * 1000000000) / TICRATE;
	if (targettime < curtime)
		return;

	ts.tv_sec = (targettime - curtime) / 1000000000;
	ts.tv_nsec = (targettime - curtime) % 1000000000;

	do status = nanosleep(&ts, &ts);
	while (status == EINTR);
	I_Assert(status == 0);
}

void I_StartupTimer(void)
{
	start_time = osGetTime(); 
}

tic_t I_GetTime(void)
{
	INT64 since_start = osGetTime() - start_time;
	return (since_start*TICRATE)/1000LL;
}

void I_Sleep(UINT32 ms)
{
	if (ms != -1)
		svcSleepThread(ms * 1000LL * 1000LL);
}

static bool isInGame()
{
	return !(paused || menuactive);
}

static aptHookCookie hookCookie;
static void AptEventHook(APT_HookType hookType, void* param)
{
	switch (hookType) {
		case APTHOOK_ONSLEEP:
			// inject pause key press
			event_t event;
			event.type = ev_keydown;
			event.data1 = gamecontrol[gc_pause][0];
			D_PostEvent(&event);
			/* fall thru */
		case APTHOOK_ONSUSPEND:
			queuePacket *packet = queueAllocPacketSafe();
			packet->type = CMD_TYPE_SUSPEND;
			queueEnqueuePacket(packet);
			//printf("suspend packet sent!\n");
			while (queuePollForDequeue())
				svcSleepThread(1000 * 1000);
			//printf("suspended\n");
			break;
		
		default:
			break;
	}
}

void I_GetEvent(void)
{
	static touchPosition last_touch_position;
	// set of keys we care about
	const u32 dskeys[] =
	{
		KEY_A,
		KEY_B,
		KEY_X,
		KEY_Y,
		KEY_L,
		KEY_ZL,
		KEY_R,
		KEY_ZR,
		KEY_START,
		KEY_SELECT,
		
		KEY_DLEFT,
		KEY_DRIGHT,
		KEY_DUP,
		KEY_DDOWN,

		KEY_CSTICK_RIGHT,
		KEY_CSTICK_LEFT,
		KEY_CSTICK_UP,
		KEY_CSTICK_DOWN
	};

	circlePosition cpos;
	event_t event;
	UINT32 up, down;
	UINT32 i;
	bool ingame = isInGame();

	hidScanInput();
	up = keysUp();
	down = keysDown();


	sliderState = osGet3DSliderState();

	{
		/* Circle Pad */

		const int amplifier = JOYAXISRANGE / CPAD_MAX_DELTA;
		
		hidCircleRead(&cpos);

		event.type = ev_joystick;
		event.data1 = 0;
		event.data3 = cpos.dy * amplifier;

		int res = cpos.dx * amplifier;
		// for the x axis (turning) we want a small damper
		event.data2 = (int) ((float) res * 0.88f);
		
		D_PostEvent(&event);

		
		hidCstickRead(&cpos);
		event.data1=1;
		event.data2 = cpos.dx*amplifier;
		event.data3 = cpos.dy*amplifier;
		D_PostEvent(&event);
		
		
		/* Touchscreen emulates a trackpad */
		if(keysHeld() & KEY_TOUCH) {
			touchPosition current_touch_position;
			hidTouchRead(&current_touch_position);
			if (!(keysDown() & KEY_TOUCH)) {
				event.type = ev_mouse;
				event.data1 = 0;
				event.data2 = current_touch_position.px - last_touch_position.px;
				event.data3 = current_touch_position.py - last_touch_position.py;
				D_PostEvent(&event);
			}
			last_touch_position = current_touch_position;
		}

		/* For the buttons, we need to report changes in state */
		for (i = 0; i < sizeof(dskeys)/sizeof(dskeys[0]); i++)
		{
			// Has this button's state changed?
			if ((up | down) & dskeys[i])
			{
				event.type = (up & dskeys[i]) ? ev_keyup : ev_keydown;

				switch(dskeys[i])
				{
					case KEY_B:
						if (ingame)
							event.data1 = KEY_JOY1+1;
						else
							event.data1 = KEY_ESCAPE;
						break;
					case KEY_A:
						if (ingame)
							event.data1 = KEY_JOY1+0;
						else
							event.data1 = KEY_ENTER;
						break;
					case KEY_X:
						event.data1 = KEY_JOY1+2;
						break;
					case KEY_Y:
						event.data1 = KEY_JOY1+3;
						break;
					case KEY_L:
						event.data1 = KEY_JOY1+4;
						break;
					case KEY_R:
						event.data1 = KEY_JOY1+5;
						break;
					case KEY_ZL:
						event.data1 = KEY_JOY1+8;
						break;
					case KEY_ZR:
						event.data1 = KEY_JOY1+9;
						break;
					case KEY_START:
						event.data1 = KEY_JOY1+6;
						break;
					case KEY_SELECT:
						if (ingame)
							event.data1 = KEY_ESCAPE;
						else
							event.data1 = KEY_BACKSPACE;
						break;
					case KEY_DUP:
						event.data1 = KEY_UPARROW;
						break;
					case KEY_DDOWN:
						event.data1 = KEY_DOWNARROW;
						break;
					case KEY_DLEFT:
						event.data1 = KEY_LEFTARROW;
						break;
					case KEY_DRIGHT:
						event.data1 = KEY_RIGHTARROW;
						break;
					case KEY_CSTICK_RIGHT:
						event.data1 = KEY_JOY1+10;
						break;
					case KEY_CSTICK_LEFT:
						event.data1 = KEY_JOY1+11;
						break;
					case KEY_CSTICK_UP:
						event.data1 = KEY_JOY1+12;
						break;
					case KEY_CSTICK_DOWN:
						event.data1 = KEY_JOY1+13;
						break;

					default:
						continue;
				}

				// printf("%s %li\n", G_KeynumToString(event.data1), event.data1);
				D_PostEvent(&event);
			}
		}

	}
}

void I_OsPolling(void)
{
	I_GetEvent();
}

ticcmd_t *I_BaseTiccmd(void)
{
	static ticcmd_t emptyticcmd;
	return &emptyticcmd;
}

ticcmd_t *I_BaseTiccmd2(void)
{
	static ticcmd_t emptyticcmd2;
	return &emptyticcmd2;
}

// lazy
#include <SDL2/SDL.h>

static Uint64 timer_frequency;

precise_t I_GetPreciseTime(void)
{
	return SDL_GetPerformanceCounter();
}

UINT64 I_GetPrecisePrecision(void)
{
	return SDL_GetPerformanceFrequency();
}

void I_SleepDuration(precise_t duration)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__HAIKU__)
	UINT64 precision = I_GetPrecisePrecision();
	precise_t dest = I_GetPreciseTime() + duration;
	precise_t slack = (precision / 5000); // 0.2 ms slack
	if (duration > slack)
	{
		duration -= slack;
		struct timespec ts = {
			.tv_sec = duration / precision,
			.tv_nsec = duration * 1000000000 / precision % 1000000000,
		};
		int status;
		do status = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts);
		while (status == EINTR);
	}

	// busy-wait the rest
	while (((INT64)dest - (INT64)I_GetPreciseTime()) > 0);
#else
	UINT64 precision = I_GetPrecisePrecision();
	INT32 sleepvalue = cv_sleep.value;
	UINT64 delaygranularity;
	precise_t cur;
	precise_t dest;

	{
		double gran = round(((double)(precision / 1000) * sleepvalue * 2.1));
		delaygranularity = (UINT64)gran;
	}

	cur = I_GetPreciseTime();
	dest = cur + duration;

	// the reason this is not dest > cur is because the precise counter may wrap
	// two's complement arithmetic is our friend here, though!
	// e.g. cur 0xFFFFFFFFFFFFFFFE = -2, dest 0x0000000000000001 = 1
	// 0x0000000000000001 - 0xFFFFFFFFFFFFFFFE = 3
	while ((INT64)(dest - cur) > 0)
	{
		// If our cv_sleep value exceeds the remaining sleep duration, use the
		// hard sleep function.
		if (sleepvalue > 0 && (dest - cur) > delaygranularity)
		{
			I_Sleep(sleepvalue);
		}

		// Otherwise, this is a spinloop.

		cur = I_GetPreciseTime();
	}
#endif
}

void I_Quit(void)
{
	printf("EXIT!\n");
	I_ShutdownMusic();
	I_ShutdownGraphics();
	exit(0);
}

void I_Error(const char *error, ...)
{
	char tmp[512];

	va_list args;
	va_start(args, error);
	vsnprintf(tmp, sizeof tmp, error, args);
	va_end(args);
		
	printf("%s", tmp);

	NDS3D_driverPanic("I_Error!");
}

void I_Tactile(FFType Type, const JoyFF_t *Effect)
{
	(void)Type;
	(void)Effect;
}

void I_Tactile2(FFType Type, const JoyFF_t *Effect)
{
	(void)Type;
	(void)Effect;
}

void I_JoyScale(void){}

void I_JoyScale2(void){}

void I_InitJoystick(void)
{
	Joystick.bGamepadStyle = false;
	Joystick2.bGamepadStyle = false;
}

void I_InitJoystick2(void){}

INT32 I_NumJoys(void)
{
	return 1;
}

const char *I_GetJoyName(INT32 joyindex)
{
	return "Nintendo 3DS";
}

void I_SetupMumble(void)
{
}

#ifndef NOMUMBLE
void I_UpdateMumble(const mobj_t *mobj, const listener_t listener)
{
	(void)mobj;
	(void)listener;
}
#endif

void I_OutputMsg(const char *error, ...)
{
	char tmp[512];

	va_list args;
	va_start(args, error);
	vsnprintf(tmp, sizeof tmp, error, args);
	va_end(args);
		
	printf("%s", tmp);
}

void I_StartupMouse(void){}

void I_StartupMouse2(void){}

void I_StartupKeyboard(void){}

INT32 I_GetKey(void)
{
	return 0;
}

void I_AddExitFunc(void (*func)())
{
	(void)func;
}

void I_RemoveExitFunc(void (*func)())
{
	(void)func;
}

static char exePath[0x400];
static bool wadAtExePath;

static bool parsePath(const char *path)
{
	if (strncmp(path, "sdmc:", 5) != 0)
		return false;

	const char *end = path + strlen(path);

	while (end != path)
	{
		if (*end == '/')
			break;
		end--;
	}

	if (*end == '/')
	{
		if (end - path >= 0x400)
			return false;

		memcpy(exePath, path, end - path);
		exePath[end - path] = '\0';
	}
	else return false;

	CONS_Printf("exePath: %s\n", exePath);

	return true;
}

INT32 I_StartupSystem(void)
{
	extern INT32 myargc;
	extern char **myargv;

	APT_CheckNew3DS(&isNew3DS);
	if (isNew3DS)
	{
		isNew3DS = true;
		osSetSpeedupEnable(true);
		// enable fast clock + L2 cache on new3ds
		PTMSYSM_ConfigureNew3DSCPU(3);
		osSetSpeedupEnable(true);
	}

	aptHook(&hookCookie, AptEventHook, NULL);

	// early Initialize graphics
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);

	/* Figure out where srb2 is stored */
	if (myargc >= 1 && parsePath(myargv[0]))
	{
		if (chdir(exePath) != 0)
			printf("chdir#1 failed!\n");

		FILE *f;
		if ((f = fopen("srb2.srb", "rb")) != NULL)
        {
        	wadAtExePath = true;
        	fclose(f);
        	return 0;
        }

		/* Could not open file */
	}
	else
	{
		// CIA version
		chdir("sdmc:/3ds/srb2_3ds/");
		strcpy(exePath, "sdmc:/3ds/srb2_3ds");
		wadAtExePath = true;
		return 0;
	}
	
	if (chdir("sdmc:/") != 0)
		printf("chdir#2 failed!\n");

	return 0;
}

void I_ShutdownSystem(void){}

void I_GetDiskFreeSpace(INT64* freespace)
{
	*freespace = 0xF0000000;
}

char *I_GetUserName(void)
{
	printf("I_GetUserName()\n");
	return NULL;
}

INT32 I_mkdir(const char *dirname, INT32 unixright)
{
	if (mkdir(dirname, unixright) != 0)
	{
		printf("I_mkdir() failed!\n");
		return -1;
	}
	
	return 0;
}

const char *I_LocateWad(void)
{
	if (wadAtExePath)
	{
		return exePath;
	}

	return "/";
}

void I_GetJoystickEvents(void){}

void I_GetJoystick2Events(void){}

void I_GetMouseEvents(void){}

void I_UpdateMouseGrab(void){}

void I_SetTextInputMode(boolean active)
{
	(void)active;
}

char *I_GetEnv(const char *name)
{
	(void)name;
	return NULL;
}

INT32 I_PutEnv(char *variable)
{
	(void)variable;
	return -1;
}

INT32 I_ClipboardCopy(const char *data, size_t size)
{
	(void)data;
	(void)size;
	return -1;
}

const char *I_ClipboardPaste(void)
{
	return NULL;
}

void I_RegisterSysCommands(void) {}
