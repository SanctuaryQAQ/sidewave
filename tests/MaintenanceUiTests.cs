using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Windows.Automation;
namespace PyroScreenTests {
class MaintenanceUiTests {
 static int pid;static string output;
 static AutomationElement Find(string id,bool name=false){
  var windows=AutomationElement.RootElement.FindAll(TreeScope.Children,new PropertyCondition(AutomationElement.ProcessIdProperty,pid));
  foreach(AutomationElement window in windows){
   var e=window.FindFirst(TreeScope.Descendants,new PropertyCondition(name?AutomationElement.NameProperty:AutomationElement.AutomationIdProperty,id));if(e!=null)return e;
  }return null;
 }
 static volatile string invokeError="";
 static AutomationElement Option(string name){return AutomationElement.RootElement.FindFirst(TreeScope.Descendants,new AndCondition(new PropertyCondition(AutomationElement.ProcessIdProperty,pid),new PropertyCondition(AutomationElement.NameProperty,name),new PropertyCondition(AutomationElement.ControlTypeProperty,ControlType.ListItem)));}
 static void Language(string value){using(var p=Process.Start(new ProcessStartInfo(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"GuiActions.exe"),pid+" \"#LanguageChoice\" \"choose="+value+"\""){UseShellExecute=false,CreateNoWindow=true})){if(!p.WaitForExit(7000)||p.ExitCode!=0)throw new Exception("Language selection failed: "+value);}}
 static void Click(string id,bool name=false){var e=Find(id,name);if(e==null)throw new Exception("Missing control: "+id);new Thread(()=>{try{((InvokePattern)e.GetCurrentPattern(InvokePattern.Pattern)).Invoke();}catch(Exception ex){invokeError=ex.Message;}}){IsBackground=true}.Start();Thread.Sleep(150);if(invokeError!="")throw new Exception(invokeError);}
 static string Report(){var e=Find("MaintenanceReport");return e==null?"":((ValuePattern)e.GetCurrentPattern(ValuePattern.Pattern)).Current.Value;}
 static string WaitReport(){DateTime end=DateTime.UtcNow.AddSeconds(50);string value="";while(DateTime.UtcNow<end){value=Report();if(value.Length>60&&value!="正在检查…"&&value!="Checking…")return value;Thread.Sleep(200);}throw new Exception("Report did not finish: "+value);}
 static void Page(string button,string expected,string language){Click(button);string text=WaitReport();File.WriteAllText(Path.Combine(output,button+"-"+language+".txt"),text);if(!text.Contains(expected)||text.Contains("检查未完成：")||text.Contains("Check incomplete:"))throw new Exception("Invalid "+button+" report: "+text);if(button=="Diagnostics"&&Find("ExportDiagnostics")==null)throw new Exception("Missing export");Click("CloseMaintenance");Thread.Sleep(200);}
 [STAThread] public static int Main(string[] args){
  output=args[1];Directory.CreateDirectory(output);pid=int.Parse(args[0]);
  try{
   if(args.Length>2&&args[2]=="dump"){
    using(var w=new StreamWriter(Path.Combine(output,"windows.txt"))){var windows=AutomationElement.RootElement.FindAll(TreeScope.Children,new PropertyCondition(AutomationElement.ProcessIdProperty,pid));foreach(AutomationElement win in windows){w.WriteLine("WINDOW "+win.Current.Name);foreach(AutomationElement e in win.FindAll(TreeScope.Descendants,Condition.TrueCondition)){if(e.Current.ControlType==ControlType.Edit||e.Current.ControlType==ControlType.Button||e.Current.ControlType==ControlType.ComboBox)w.WriteLine(e.Current.ControlType.ProgrammaticName+" id="+e.Current.AutomationId+" name="+e.Current.Name+" patterns="+String.Join(",",Array.ConvertAll(e.GetSupportedPatterns(),p=>p.ProgrammaticName)));}}}return 0;
   }
   if(args.Length>2&&args[2]=="export"){
    Click("Diagnostics");string expected=WaitReport();Click("ExportDiagnostics");
    AutomationElement file=null;DateTime end=DateTime.UtcNow.AddSeconds(8);while(file==null&&DateTime.UtcNow<end){file=Find("1001");Thread.Sleep(100);}
    if(file==null)throw new Exception("Save dialog file-name field missing");
    string target=Path.Combine(output,"exported-diagnostics.txt");object value;
    if(!file.TryGetCurrentPattern(ValuePattern.Pattern,out value)){file=file.FindFirst(TreeScope.Descendants,new PropertyCondition(AutomationElement.ControlTypeProperty,ControlType.Edit));value=file.GetCurrentPattern(ValuePattern.Pattern);}
    ((ValuePattern)value).SetValue(target);Click("1");end=DateTime.UtcNow.AddSeconds(8);while(!File.Exists(target)&&DateTime.UtcNow<end)Thread.Sleep(100);
    if(!File.Exists(target))throw new Exception("Exported report file missing: "+target);
    if(File.ReadAllText(target).Replace("\r\n","\n")!=expected.Replace("\r\n","\n"))throw new Exception("Exported report differs from the visible report");Click("CloseMaintenance");File.WriteAllText(Path.Combine(output,"export-result.txt"),"PASS: visible diagnostic report exported and read back exactly.");return 0;
   }
   if(Find("Subtitle")!=null)throw new Exception("Subtitle still present");
   if(args.Length<3){Language("中文");Page("Firewall","防火墙规则","zh");Page("Driver","当前发送后端：","zh");Page("Diagnostics","当前运行状态","zh");}
   Language("English");Thread.Sleep(300);
   try{Page("Firewall","Firewall rules","en");Page("Driver","Sender backend:","en");Page("Diagnostics","Current runtime state","en");}
   finally{Language("中文");}
   // Cancel forgetting pairings; verify the application remains responsive.
   Click("Forget");Thread.Sleep(300);var cancel=Find("取消",true)??Find("Cancel",true);if(cancel==null)throw new Exception("Pairing confirmation missing");((InvokePattern)cancel.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
   File.WriteAllText(Path.Combine(output,"ui-result.txt"),"PASS: subtitle removed; 3 pages in Chinese and English; pairing cancellation; Chinese restored.");return 0;
  }catch(Exception e){File.WriteAllText(Path.Combine(output,"ui-error.txt"),e.ToString());return 1;}
 }
}
}
