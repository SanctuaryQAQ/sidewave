#pragma once
#include <Windows.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// Ephemeral telemetry must never make capture or presentation wait for a disk.
// Keep one pending snapshot and publish the completed file atomically.
class PyrowaveLiveStatsWriter
{
public:
	explicit PyrowaveLiveStatsWriter(std::wstring output)
		: path(std::move(output)), tempPath(path + L".tmp"), worker([this]{ Run(); }) {}
	~PyrowaveLiveStatsWriter()
	{
		{ std::lock_guard<std::mutex> lock(mutex); stopped=true; }
		ready.notify_one();
		if(worker.joinable())worker.join();
	}
	PyrowaveLiveStatsWriter(const PyrowaveLiveStatsWriter&)=delete;
	PyrowaveLiveStatsWriter& operator=(const PyrowaveLiveStatsWriter&)=delete;
	void Publish(const char* data,size_t length)
	{
		{ std::lock_guard<std::mutex> lock(mutex); pending.assign(data,length); available=true; }
		ready.notify_one();
	}
private:
	void Run()
	{
		SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
		std::string current;
		for(;;)
		{
			{
				std::unique_lock<std::mutex> lock(mutex);
				ready.wait(lock,[this]{return stopped||available;});
				if(!available&&stopped)return;
				current.swap(pending);available=false;
			}
			HANDLE file=CreateFileW(tempPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
				CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
			if(file==INVALID_HANDLE_VALUE)continue;
			DWORD written=0;
			const bool complete=WriteFile(file,current.data(),DWORD(current.size()),&written,nullptr)&&written==current.size();
			CloseHandle(file);
			if(complete)MoveFileExW(tempPath.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
		}
	}
	std::wstring path,tempPath;
	std::mutex mutex;
	std::condition_variable ready;
	std::string pending;
	bool available=false,stopped=false;
	std::thread worker;
};
