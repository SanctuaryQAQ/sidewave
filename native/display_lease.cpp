#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <swdevice.h>
#include <string>
#include <set>
#include <cstdio>
#pragma comment(lib,"swdevice.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"advapi32.lib")
struct CreatedState{HANDLE event;HRESULT result;};
void WINAPI Created(HSWDEVICE,HRESULT r,void*c,PCWSTR){auto*s=(CreatedState*)c;s->result=r;SetEvent(s->event);}
void Write(const std::wstring&path,const std::wstring&value){
 std::wstring tmp=path+L".tmp";HANDLE f=CreateFileW(tmp.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
 if(f==INVALID_HANDLE_VALUE)return;DWORD n;WriteFile(f,value.data(),DWORD(value.size()*2),&n,nullptr);CloseHandle(f);
 MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
}
std::wstring SettingsPath(){
 wchar_t dir[MAX_PATH];DWORD size=sizeof(dir);
 if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver",L"VDDPATH",RRF_RT_REG_SZ,nullptr,dir,&size)==ERROR_SUCCESS)return std::wstring(dir)+L"\\vdd_settings.xml";
 DWORD n=GetEnvironmentVariableW(L"SystemDrive",dir,MAX_PATH);
 return (n&&n<MAX_PATH?std::wstring(dir):std::wstring(L"C:"))+L"\\VirtualDisplayDriver\\vdd_settings.xml";
}
// The driver reads its mode list when the device is created, so missing modes are added beforehand.
void EnsureModes(DWORD w,DWORD h){
 std::wstring path=SettingsPath();
 HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
 if(f==INVALID_HANDLE_VALUE)return;
 std::string xml;LARGE_INTEGER length{};DWORD n=0;
 if(GetFileSizeEx(f,&length)&&length.QuadPart>0&&length.QuadPart<(1<<20)){xml.resize(size_t(length.QuadPart));if(!ReadFile(f,&xml[0],DWORD(xml.size()),&n,nullptr)||n!=xml.size())xml.clear();}
 CloseHandle(f);
 size_t end=xml.find("</resolutions>");if(end==std::string::npos)return;
 // Keep in sync with the 16:9 presets in src/App.cs.
 const DWORD presets[][2]={{1280,720},{1600,900},{1920,1080},{2048,1152},{2560,1440},{3200,1800},{3840,2160}};
 std::string newline=xml.find("\r\n")!=std::string::npos?"\r\n":"\n",added;
 auto add=[&](DWORD mw,DWORD mh){
  std::string mode="<width>"+std::to_string(mw)+"</width><height>"+std::to_string(mh)+"</height>";
  if(xml.find(mode)==std::string::npos&&added.find(mode)==std::string::npos)added+="  <resolution>"+mode+"<refresh_rate>60</refresh_rate></resolution>"+newline;
 };
 add(w,h);for(auto&p:presets)add(p[0],p[1]);
 if(added.empty())return;
 size_t line=xml.rfind('\n',end);xml.insert(line==std::string::npos?end:line+1,added);
 std::wstring tmp=path+L".tmp";f=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
 if(f==INVALID_HANDLE_VALUE)return;
 bool ok=WriteFile(f,xml.data(),DWORD(xml.size()),&n,nullptr)&&n==xml.size();CloseHandle(f);
 if(!ok||!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))DeleteFileW(tmp.c_str());
}
std::set<std::wstring> Displays(){
 std::set<std::wstring> result;
 for(DWORD i=0;i<64;++i){DISPLAY_DEVICEW d{};d.cb=sizeof(d);if(!EnumDisplayDevicesW(nullptr,i,&d,0))break;if(d.StateFlags&DISPLAY_DEVICE_ACTIVE)result.insert(d.DeviceName);}
 return result;
}
bool Apply(const std::wstring&name,DWORD w,DWORD h,DWORD hz,const std::wstring&status,int id){
 DEVMODEW m{};m.dmSize=sizeof(m);
 if(!EnumDisplaySettingsExW(name.c_str(),ENUM_CURRENT_SETTINGS,&m,0))return false;
 DWORD actualHz=m.dmDisplayFrequency;
 bool sameSize=m.dmPelsWidth==w&&m.dmPelsHeight==h;
 if(sameSize&&actualHz==hz){
  Write(status,L"READY|"+name+L"|"+std::to_wstring(w)+L"|"+std::to_wstring(h)+L"|"+std::to_wstring(actualHz)+L"|"+std::to_wstring(id));return true;
 }
 m.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY;m.dmPelsWidth=w;m.dmPelsHeight=h;m.dmDisplayFrequency=hz;
 LONG test=ChangeDisplaySettingsExW(name.c_str(),&m,nullptr,CDS_TEST,nullptr);
 if(test!=DISP_CHANGE_SUCCESSFUL&&hz==240){m.dmDisplayFrequency=165;test=ChangeDisplaySettingsExW(name.c_str(),&m,nullptr,CDS_TEST,nullptr);}
 if(test!=DISP_CHANGE_SUCCESSFUL){Write(status,L"ERROR|MODE_UNAVAILABLE|"+std::to_wstring(id));return false;}
 if(sameSize&&actualHz==m.dmDisplayFrequency){
  Write(status,L"READY|"+name+L"|"+std::to_wstring(w)+L"|"+std::to_wstring(h)+L"|"+std::to_wstring(actualHz)+L"|"+std::to_wstring(id));return true;
 }
 if(ChangeDisplaySettingsExW(name.c_str(),&m,nullptr,0,nullptr)!=DISP_CHANGE_SUCCESSFUL){Write(status,L"ERROR|MODE_APPLY|"+std::to_wstring(id));return false;}
 DEVMODEW actual{};actual.dmSize=sizeof(actual);
 if(!EnumDisplaySettingsExW(name.c_str(),ENUM_CURRENT_SETTINGS,&actual,0)||actual.dmPelsWidth!=w||actual.dmPelsHeight!=h){
  Write(status,L"ERROR|MODE_READBACK|"+std::to_wstring(id));return false;
 }
 Write(status,L"READY|"+name+L"|"+std::to_wstring(w)+L"|"+std::to_wstring(h)+L"|"+std::to_wstring(actual.dmDisplayFrequency)+L"|"+std::to_wstring(id));return true;
}
int wmain(int argc,wchar_t**argv){
 std::wstring path,backend=L"vdd",deviceId;DWORD w=2560,h=1440,hz=120,parentId=0;
 for(int i=1;i<argc;++i){
  if(i+1>=argc)return 1;std::wstring a=argv[i],v=argv[++i];
  if(a==L"--status-file")path=v;else if(a==L"--backend")backend=v;
  else if(a==L"--device-id")deviceId=v;
  else if(a==L"--width")w=wcstoul(v.c_str(),nullptr,10);else if(a==L"--height")h=wcstoul(v.c_str(),nullptr,10);
  else if(a==L"--parent")parentId=wcstoul(v.c_str(),nullptr,10);
  else if(a==L"--refresh")hz=wcstoul(v.c_str(),nullptr,10);else return 1;
 }
 if(path.empty()||(backend!=L"vdd"&&backend!=L"lanscreen"))return 1;
 if(deviceId.size()!=32||deviceId.find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos)return 1;
 HANDLE parent=parentId?OpenProcess(SYNCHRONIZE,FALSE,parentId):nullptr;
 if(parentId&&!parent)return 5;
 if(backend==L"vdd")EnsureModes(w,h);
 auto before=Displays();const wchar_t*hw=backend==L"vdd"?L"MttVDD\0":L"LanScreenDisplay\0";
 // Stable adapter identity lets Windows reuse its display configuration on reconnect.
 std::wstring instance=L"PyroScreen-"+deviceId+L"-"+backend;
 SW_DEVICE_CREATE_INFO info{};info.cbSize=sizeof(info);info.pszInstanceId=instance.c_str();
 info.pszzHardwareIds=hw;info.pszzCompatibleIds=hw;info.pszDeviceDescription=L"PyroScreen virtual monitor";
 info.CapabilityFlags=SWDeviceCapabilitiesRemovable|SWDeviceCapabilitiesSilentInstall|SWDeviceCapabilitiesDriverRequired;
 CreatedState state{CreateEventW(nullptr,TRUE,FALSE,nullptr),E_PENDING};HSWDEVICE device=nullptr;
 HRESULT r=SwDeviceCreate(L"PyroScreen",L"HTREE\\ROOT\\0",&info,0,nullptr,Created,&state,&device);
 if(SUCCEEDED(r)){DWORD wait=WaitForSingleObject(state.event,30000);r=wait==WAIT_OBJECT_0?state.result:HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
 CloseHandle(state.event);
 if(FAILED(r)){if(device)SwDeviceClose(device);Write(path,L"ERROR|DRIVER_CREATE|"+std::to_wstring(unsigned(r)));return 2;}
 std::wstring display;
 for(int attempt=0;attempt<200&&display.empty();++attempt){
  for(DWORD i=0;i<64;++i){
   DISPLAY_DEVICEW d{};d.cb=sizeof(d);if(!EnumDisplayDevicesW(nullptr,i,&d,0))break;
   if(!before.count(d.DeviceName)&&(d.StateFlags&DISPLAY_DEVICE_ACTIVE)&&(wcsstr(d.DeviceString,L"Virtual")||wcsstr(d.DeviceString,L"LanScreen"))){display=d.DeviceName;break;}
  }
  if(display.empty())Sleep(100);
 }
 if(display.empty()){Write(path,L"ERROR|DISPLAY_ENUMERATION");SwDeviceClose(device);return 3;}
 if(!Apply(display,w,h,hz,path,0)){SwDeviceClose(device);return 4;}
 std::wstring stop=path+L".stop",mode=path+L".mode";
 while(GetFileAttributesW(stop.c_str())==INVALID_FILE_ATTRIBUTES&&(!parent||WaitForSingleObject(parent,0)==WAIT_TIMEOUT)){
  FILE*f=_wfopen(mode.c_str(),L"rt");if(f){
   int id=0;unsigned nw=0,nh=0,nr=0;int n=fscanf(f,"%d|%u|%u|%u",&id,&nw,&nh,&nr);fclose(f);DeleteFileW(mode.c_str());
   if(n==4&&id>0&&nw>=640&&nw<=3840&&nh>=480&&nh<=2160&&(nr==60||nr==120||nr==165||nr==240))Apply(display,nw,nh,nr,path,id);
  }Sleep(50);
 }
 SwDeviceClose(device);if(parent)CloseHandle(parent);DeleteFileW(stop.c_str());return 0;
}
