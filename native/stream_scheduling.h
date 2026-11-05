#pragma once
#include <windows.h>
#include <timeapi.h>
#include <cstdio>
#pragma comment(lib,"winmm.lib")

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

// Streaming processes run windowless (sender) or behind other windows
// (receiver). Windows 11 then may run them as EcoQoS and serve millisecond
// waits (AcquireNextFrame timeouts, Sleep, MsgWait) at the 15.6 ms default
// tick. Opt out of both. The receiver also raises GPU scheduling so decode
// and present are not queued behind other applications' GPU work. The sender
// must not: its encode would then pre-empt DWM composing the very desktop it
// captures (measured: +1.3 ms source age at capture).
inline void ApplyStreamScheduling(const char* role,bool raiseGpuPriority)
{
	PROCESS_POWER_THROTTLING_STATE throttling{};
	throttling.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	throttling.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
	throttling.StateMask=0;
	const bool power=SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&throttling,sizeof(throttling))!=FALSE;
	const DWORD powerError=power?0:GetLastError();
	const bool timer=timeBeginPeriod(1)==TIMERR_NOERROR;
	// D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH. REALTIME could starve DWM itself.
	using SetGpuPriority=LONG(APIENTRY*)(HANDLE,int);
	LONG gpuStatus=-1;
	if(HMODULE gdi=raiseGpuPriority?LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32):nullptr)
		if(auto set=reinterpret_cast<SetGpuPriority>(GetProcAddress(gdi,"D3DKMTSetProcessSchedulingPriorityClass")))
			gpuStatus=set(GetCurrentProcess(),4);
	if(raiseGpuPriority)
		std::fprintf(stderr,"%s scheduling: power throttling off=%d (error %lu), 1 ms timer=%d, GPU priority high status=0x%08lx\n",
			role,power?1:0,powerError,timer?1:0,(unsigned long)gpuStatus);
	else
		std::fprintf(stderr,"%s scheduling: power throttling off=%d (error %lu), 1 ms timer=%d\n",role,power?1:0,powerError,timer?1:0);
}
