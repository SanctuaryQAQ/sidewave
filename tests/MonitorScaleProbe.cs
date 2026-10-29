using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Web.Script.Serialization;
class MonitorScaleProbe {
 [StructLayout(LayoutKind.Sequential)] struct Rect {public int left,top,right,bottom;}
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct Info {public uint size;public Rect monitor,work;public uint flags;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string device;}
 delegate bool Callback(IntPtr monitor,IntPtr dc,ref Rect bounds,IntPtr data);
 [DllImport("user32.dll")]static extern bool EnumDisplayMonitors(IntPtr dc,IntPtr clip,Callback callback,IntPtr data);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern bool GetMonitorInfo(IntPtr monitor,ref Info info);
 [DllImport("user32.dll")]static extern bool SetProcessDpiAwarenessContext(IntPtr context);
 [DllImport("shcore.dll")]static extern int GetScaleFactorForMonitor(IntPtr monitor,out int scale);
 [DllImport("shcore.dll")]static extern int GetDpiForMonitor(IntPtr monitor,int type,out uint x,out uint y);
 static int Main(string[] args){if(args.Length!=1)return 2;try{
  SetProcessDpiAwarenessContext(new IntPtr(-4));var rows=new List<object>();
  Callback callback=delegate(IntPtr monitor,IntPtr dc,ref Rect bounds,IntPtr data){
   Info info=new Info();info.size=(uint)Marshal.SizeOf(info);if(!GetMonitorInfo(monitor,ref info))throw new Exception("GetMonitorInfo failed");
   int scale;int result=GetScaleFactorForMonitor(monitor,out scale);if(result<0)throw new Exception("Scale read failed "+result);
   uint dx,dy;int dpiResult=GetDpiForMonitor(monitor,0,out dx,out dy);if(dpiResult<0)throw new Exception("DPI read failed "+dpiResult);
   rows.Add(new{device=info.device,primary=(info.flags&1)!=0,scalePercent=dx*100.0/96,dpiX=dx,dpiY=dy,legacyScaleFactor=scale,width=info.monitor.right-info.monitor.left,height=info.monitor.bottom-info.monitor.top,x=info.monitor.left,y=info.monitor.top});return true;
  };
  if(!EnumDisplayMonitors(IntPtr.Zero,IntPtr.Zero,callback,IntPtr.Zero))throw new Exception("Monitor enumeration failed");
  File.WriteAllText(args[0],new JavaScriptSerializer().Serialize(rows));return 0;
 }catch(Exception e){File.WriteAllText(args[0]+".error",e.ToString());return 1;}}
}
