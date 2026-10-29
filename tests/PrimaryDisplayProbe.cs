using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
namespace PyroScreen {
class PrimaryDisplayProbe {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct Device {
  public uint size;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string name;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)]public string description;public uint flags;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)]public string id;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)]public string key;
 }
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct Mode {
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string device;public ushort spec,driver,size,extra;public uint fields;
  public int x,y;public uint orientation,fixedOutput;public short color,duplex,yres,tt,collate;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string form;public ushort logPixels;
  public uint bits,width,height,flags,frequency,icm,icmIntent,media,dither,reserved1,reserved2,panningW,panningH;
 }
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern bool EnumDisplayDevices(string device,uint index,ref Device data,uint flags);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern bool EnumDisplaySettings(string device,int index,ref Mode data);
 [DllImport("user32.dll")]static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")]static extern uint GetDpiForSystem();
 static string Read(){
  for(uint i=0;i<64;i++){Device d=new Device();d.size=(uint)Marshal.SizeOf(d);if(!EnumDisplayDevices(null,i,ref d,0))break;
   if((d.flags&4)==0)continue;Mode m=new Mode();m.size=(ushort)Marshal.SizeOf(m);
   if(!EnumDisplaySettings(d.name,-1,ref m))throw new IOException("Primary mode unavailable.");
   return d.name+"|"+m.width+"x"+m.height+"|"+m.frequency+"Hz|"+m.x+","+m.y+"|dpi="+GetDpiForSystem();
  }throw new IOException("Primary display unavailable.");
 }
 [STAThread]public static int Main(string[] args){if(args.Length!=1)return 2;try{
  SetProcessDPIAware();string baseline=Read();var changes=new List<object>();int samples=0;
  var timer=System.Diagnostics.Stopwatch.StartNew();string previous=baseline;
  while(timer.Elapsed.TotalSeconds<180&&!File.Exists(args[0]+".stop")){
   string current=Read();samples++;if(current!=previous){changes.Add(new {elapsedMs=timer.ElapsedMilliseconds,mode=current});previous=current;}Thread.Sleep(10);
  }
  File.WriteAllText(args[0],Json.Write(new {passed=changes.Count==0,baseline=baseline,samples=samples,durationMs=timer.ElapsedMilliseconds,changes=changes}),new System.Text.UTF8Encoding(false));return changes.Count==0?0:1;
 }catch(Exception e){File.WriteAllText(args[0]+".error",e.ToString());return 1;}}
}
}
