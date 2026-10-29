using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
class ClientCursorProbe {
 [StructLayout(LayoutKind.Sequential)]struct Point{public int x,y;}
 [StructLayout(LayoutKind.Sequential)]struct Cursor{public uint size,flags;public IntPtr handle;public Point point;}
 [DllImport("user32.dll")]static extern bool GetCursorInfo(ref Cursor info);
 [DllImport("user32.dll")]static extern bool GetCursorPos(out Point point);
 [DllImport("user32.dll")]static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]static extern bool SetProcessDPIAware();
 static bool Visible(){Cursor c=new Cursor();c.size=(uint)Marshal.SizeOf(c);if(!GetCursorInfo(ref c))throw new Exception("Cursor state unavailable");return(c.flags&1)!=0;}
 [STAThread]public static int Main(string[]args){
  if(args.Length!=1)return 1;SetProcessDPIAware();Point original;GetCursorPos(out original);
  try{
   Thread.Sleep(2300);bool hiddenInitially=!Visible();
   SetCursorPos(original.x+10,original.y+10);Thread.Sleep(300);bool shownAfterMovement=Visible();
   Thread.Sleep(2300);bool hiddenAfterIdle=!Visible();
   File.WriteAllText(args[0],"{\"passed\":"+(hiddenInitially&&shownAfterMovement&&hiddenAfterIdle?"true":"false")+",\"initiallyHidden\":"+hiddenInitially.ToString().ToLowerInvariant()+",\"shownAfterProgrammaticMovement\":"+shownAfterMovement.ToString().ToLowerInvariant()+",\"hiddenAfterIdle\":"+hiddenAfterIdle.ToString().ToLowerInvariant()+"}");
   return hiddenInitially&&shownAfterMovement&&hiddenAfterIdle?0:2;
  }finally{SetCursorPos(original.x,original.y);Thread.Sleep(2300);}
 }
}
