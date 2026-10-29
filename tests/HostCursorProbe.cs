using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
class HostCursorProbe {
 [StructLayout(LayoutKind.Sequential)]struct Point {public int x,y;}
 [DllImport("user32.dll")]static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")]static extern bool GetCursorPos(out Point point);
 [DllImport("user32.dll")]static extern bool SetCursorPos(int x,int y);
 [STAThread]public static int Main(string[]args){
  if(args.Length!=2)return 1;SetProcessDPIAware();Point original;GetCursorPos(out original);
  try{
   var screen=Screen.AllScreens.First(s=>s.DeviceName==args[0]);var rect=screen.Bounds;
   for(int i=0;i<8;++i){SetCursorPos(rect.Left+rect.Width/2+i*16,rect.Top+rect.Height/2);Thread.Sleep(200);}
   File.WriteAllText(args[1],"{\"completed\":true,\"sourceDisplay\":\""+args[0].Replace("\\","\\\\")+"\",\"cursorRestored\":true}");return 0;
  }catch(Exception e){File.WriteAllText(args[1]+".error",e.Message);return 1;}
  finally{SetCursorPos(original.x,original.y);}
 }
}
