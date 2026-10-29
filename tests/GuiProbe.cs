using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Automation;
namespace PyroScreenTests{
class GuiProbe{
 [DllImport("user32.dll")]static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")]static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr window,out Rect rect);
 [StructLayout(LayoutKind.Sequential)]struct Rect{public int left,top,right,bottom;}
 [STAThread]public static int Main(string[] args){
  if(args.Length<2||args.Length>3)return 1;
  SetProcessDPIAware();
  try{
   Process p=Process.GetProcessById(int.Parse(args[0]));p.Refresh();IntPtr h=p.MainWindowHandle;if(h==IntPtr.Zero)throw new Exception("GUI window unavailable.");
   if(args.Length==2){Rect r;GetWindowRect(h,out r);
   using(var b=new Bitmap(r.right-r.left,r.bottom-r.top))using(var g=Graphics.FromImage(b)){
    IntPtr dc=g.GetHdc();try{if(!PrintWindow(h,dc,2))throw new Exception("PrintWindow failed.");}finally{g.ReleaseHdc(dc);}
    b.Save(args[1],ImageFormat.Png);
   }}
   var root=AutomationElement.FromHandle(h);var children=root.FindAll(TreeScope.Descendants,Condition.TrueCondition);
   using(var w=new StreamWriter(args[1]+".txt",false,System.Text.Encoding.UTF8)){
    foreach(AutomationElement e in children){string name=e.Current.Name;if(name=="客户端配对码")continue;
     if(name!="")w.WriteLine(e.Current.ControlType.ProgrammaticName+" "+name);
     if(e.Current.ControlType==ControlType.Button)w.WriteLine("Enabled "+e.Current.AutomationId+" "+e.Current.IsEnabled);
     object state;if(e.Current.ControlType==ControlType.CheckBox&&e.TryGetCurrentPattern(TogglePattern.Pattern,out state))w.WriteLine("Checked "+e.Current.AutomationId+" "+((TogglePattern)state).Current.ToggleState);
     if(e.Current.AutomationId=="Diagnostics")w.WriteLine("ConfigRoot "+e.Current.HelpText);}
   }return 0;
  }catch(Exception e){File.WriteAllText(args[1]+".error",e.ToString());return 1;}
 }
}
}
