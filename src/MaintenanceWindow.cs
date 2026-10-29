using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace PyroScreen {
static class SystemInspection {
 public static string Script(string name,string app,int timeout){
  string file=Path.Combine(Paths.Bin,name);
  if(!File.Exists(file))throw new IOException(Language.Tr("缺少配置组件：")+name);
  string command="-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+Engine.Q(file);
  if(app!=null)command+=" -AppPath "+Engine.Q(app);
  var psi=new ProcessStartInfo(Path.Combine(Environment.SystemDirectory,@"WindowsPowerShell\v1.0\powershell.exe"),command){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8};
  var output=new StringBuilder();var error=new StringBuilder();
  using(var p=new Process{StartInfo=psi}){
   p.OutputDataReceived+=(s,e)=>{if(e.Data!=null)output.AppendLine(e.Data);};
   p.ErrorDataReceived+=(s,e)=>{if(e.Data!=null)error.AppendLine(e.Data);};
   p.Start();p.BeginOutputReadLine();p.BeginErrorReadLine();
   if(!p.WaitForExit(timeout*1000)){try{p.Kill();p.WaitForExit();}catch{}throw new IOException(Language.Tr("检查或配置超时，请查看诊断日志。"));}
   p.WaitForExit();
   if(p.ExitCode!=0){Paths.Log("setup",error.ToString()+output);throw new IOException("PowerShell exit "+p.ExitCode+": "+(error.Length==0?output.ToString():error.ToString()));}
   if(name!="Inspect-System.ps1")Paths.Log("setup",output.ToString());
   return output.ToString();
  }
 }
 static IEnumerable<Dictionary<string,object>> Rows(Dictionary<string,object> data,string key){
  object value;if(!data.TryGetValue(key,out value))yield break;
  var rows=value as IEnumerable;if(rows==null)yield break;
  foreach(object row in rows){var d=row as Dictionary<string,object>;if(d!=null)yield return d;}
 }
 static bool Yes(Dictionary<string,object> data,string key){return Json.S(data,key)=="True";}
 static string T(string text){return Language.Tr(text);}
 public static string Report(string page,string state,string inspection){
  var d=Json.Read(inspection);var live=Json.Read(state);var b=new StringBuilder();
  b.AppendLine("SideWave · "+DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
  if(page!="driver"){
   b.AppendLine(T("网络连接"));int count=0,privateCount=0;
   foreach(var p in Rows(d,"profiles")){count++;string category=Json.S(p,"category");if(category=="Private")privateCount++;b.AppendLine("  "+Json.S(p,"name")+" · "+category+" · "+Json.S(p,"addresses"));}
   if(count==0)b.AppendLine(T("未检测到网络连接。"));
   else if(privateCount==0)b.AppendLine(T("没有专用网络，专用网络规则不会生效。请打开 Windows 网络设置，仅将可信局域网设为专用。"));
   b.AppendLine();b.AppendLine(T("防火墙规则（本软件路径 · 专用网络 · 本地子网）"));
   foreach(var r in Rows(d,"rules"))b.AppendLine("  "+Json.S(r,"name")+" · "+Json.S(r,"protocol")+" "+Json.S(r,"port")+" · "+T(Yes(r,"valid")?"已验证":"缺失或配置不匹配"));
   b.AppendLine(T("规则缺失或路径变化时，点击“修复网络配置”。两台电脑都需要检查。"));
   b.AppendLine(T("显示延迟采样：")+T(Yes(d,"sampler")?"已安装":"未安装"));b.AppendLine();
  }
  if(page!="network"){
   b.AppendLine(T("虚拟显示驱动"));object packages;var list=d.TryGetValue("driverPackages",out packages)?packages as ICollection:null;
   b.AppendLine(T("当前发送后端：")+Json.S(live,"backend"));
   if(Json.S(live,"backend")=="lanscreen"){object legacy;var installed=d.TryGetValue("legacyPackages",out legacy)?legacy as ICollection:null;b.AppendLine(T(installed!=null&&installed.Count>0?"LanScreen 驱动包已安装。":"未找到 LanScreen 驱动包。"));b.AppendLine(T("安装按钮配置签名 VDD 驱动；使用 LanScreen 后端时需保留现有驱动。"));}
   b.AppendLine(T(list!=null&&list.Count>0?"驱动包已安装。连接副机时才创建虚拟屏，空闲时没有设备属于正常状态。":Json.S(live,"backend")=="lanscreen"?"签名 VDD 驱动包未安装。":"未找到虚拟显示驱动包，请点击“安装 / 修复驱动”。"));
   foreach(var device in Rows(d,"devices"))b.AppendLine("  "+Json.S(device,"name")+" · "+Json.S(device,"status"));
   b.AppendLine(T("接收模式无需安装虚拟显示驱动。"));b.AppendLine();
  }
  if(page=="diagnostics"){
   b.AppendLine(T("当前运行状态"));
   b.AppendLine(T("模式：")+T(Json.S(live,"mode")=="send"?"发送模式":"接收模式"));
   b.AppendLine(T("连接：")+T(Yes(live,"connected")?"已连接":"已断开"));
   b.AppendLine(T("视频：")+T(Yes(live,"streaming")?"运行中":"未运行"));
   b.AppendLine(T("状态：")+Language.Tr(Json.S(live,"status")));
   b.AppendLine("  "+Json.I(live,"width")+" × "+Json.I(live,"height")+" · "+Json.I(live,"fps")+" Hz · "+Json.S(live,"backend"));
   b.AppendLine("PID · GUI "+Process.GetCurrentProcess().Id+" · sender "+Json.I(live,"senderPid")+" · receiver "+Json.I(live,"receiverPid"));
   b.AppendLine(T("遥测距今（ms）：")+Json.S(live,"telemetryAgeMs"));
   if(!Yes(live,"streaming")||Json.L(live,"telemetryAgeMs")>5000||Json.L(live,"telemetryAgeMs")<0)b.AppendLine(T("当前没有新鲜视频遥测，下方历史值不能用于判断当前画面。"));
   object stats;if(live.TryGetValue("stats",out stats)&&stats is Dictionary<string,object>){b.AppendLine(T("视频遥测："));b.AppendLine(Json.Write(stats));}
   foreach(var listener in Rows(d,"listeners"))b.AppendLine("TCP 55166 · "+Json.S(listener,"address")+" · PID "+Json.I(listener,"pid"));
   b.AppendLine();b.AppendLine(T("程序路径：")+Process.GetCurrentProcess().MainModule.FileName);
   b.AppendLine(T("日志路径：")+Paths.Root);
  }
  object issues;if(d.TryGetValue("issues",out issues)&&issues is IEnumerable)foreach(var issue in (IEnumerable)issues)b.AppendLine(T("检查未完成：")+issue);
  return b.ToString();
 }
}

class MaintenanceWindow {
 readonly Window dialog;readonly TextBox report;readonly TextBlock feedback;
 readonly Button refresh,configure,save;readonly string page;readonly Func<string> state;
 readonly Action<string,Action<int,string>> setup;bool checking,configuring,closed;
 static string T(string s){return Language.Tr(s);}
 public static void Show(Window owner,string page,Func<string> state,Action<string,Action<int,string>> setup){new MaintenanceWindow(owner,page,state,setup).dialog.ShowDialog();}
 MaintenanceWindow(Window owner,string page,Func<string> state,Action<string,Action<int,string>> setup){
  this.page=page;this.state=state;this.setup=setup;
  string title=page=="network"?"网络设置":page=="driver"?"虚拟屏驱动":"诊断";
  dialog=new Window{Owner=owner,Title="SideWave · "+T(title),Width=800,Height=620,MinWidth=680,MinHeight=460,Background=owner.Background,Foreground=owner.Foreground,FontFamily=owner.FontFamily,FontSize=14,WindowStartupLocation=WindowStartupLocation.CenterOwner,Resources=owner.Resources};
  var grid=new Grid{Margin=new Thickness(24)};grid.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});grid.RowDefinitions.Add(new RowDefinition());grid.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});grid.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});
  var heading=new TextBlock{Text=T(title),FontSize=24,Margin=new Thickness(0,0,0,16)};grid.Children.Add(heading);
  report=new TextBox{Name="MaintenanceReport",IsReadOnly=true,TextWrapping=TextWrapping.Wrap,VerticalScrollBarVisibility=ScrollBarVisibility.Auto,HorizontalScrollBarVisibility=ScrollBarVisibility.Disabled,Padding=new Thickness(14),Text=T("正在检查…")};Grid.SetRow(report,1);grid.Children.Add(report);
  feedback=new TextBlock{Name="MaintenanceFeedback",TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,12,0,12)};Grid.SetRow(feedback,2);grid.Children.Add(feedback);
  var buttons=new WrapPanel();Grid.SetRow(buttons,3);grid.Children.Add(buttons);
  refresh=Button(buttons,"RefreshMaintenance","重新检查",Refresh);
  if(page=="network"){
   configure=Button(buttons,"RepairNetwork","修复网络配置",()=>Configure("--setup-firewall"));
   Button(buttons,"WindowsNetwork","Windows 网络设置",()=>Open("ms-settings:network-status"));
  }else if(page=="driver"){
   configure=Button(buttons,"RepairDriver","安装 / 修复驱动",()=>Configure("--setup-driver"));
   Button(buttons,"DisplaySettings","显示设置",()=>Open("ms-settings:display"));
  }else{
   save=Button(buttons,"ExportDiagnostics","导出报告",Save);
   Button(buttons,"OpenLogs","打开日志",()=>Open(Paths.Root));
  }
  Button(buttons,"CloseMaintenance","关闭",()=>dialog.Close());
  dialog.Content=grid;dialog.Closed+=(s,e)=>closed=true;dialog.Closing+=(s,e)=>{if(configuring)e.Cancel=true;};dialog.Loaded+=(s,e)=>Refresh();
 }
 Button Button(Panel panel,string name,string title,Action action){var button=new Button{Name=name,Content=T(title),Padding=new Thickness(12,8,12,8),Margin=new Thickness(0,0,8,8)};button.Click+=(s,e)=>{try{action();}catch(Exception ex){feedback.Text=ex.Message;Paths.Log("setup",ex.ToString());}};panel.Children.Add(button);return button;}
 void Controls(){refresh.IsEnabled=!checking&&!configuring;if(configure!=null)configure.IsEnabled=!checking&&!configuring;if(save!=null)save.IsEnabled=!checking;}
 void Refresh(){
  if(checking||closed)return;checking=true;Controls();report.Text=T("正在检查…");string snapshot=state();
  new Thread(()=>{string text;try{text=SystemInspection.Report(page,snapshot,SystemInspection.Script("Inspect-System.ps1",Process.GetCurrentProcess().MainModule.FileName,35));}catch(Exception e){text=T("检查未完成：")+e.Message;Paths.Log("setup",e.ToString());}
   if(!closed)dialog.Dispatcher.BeginInvoke(new Action(()=>{if(closed)return;report.Text=text;checking=false;Controls();}));}){IsBackground=true,Name="Maintenance inspection"}.Start();
 }
 void Configure(string option){
  if(configuring)return;configuring=true;Controls();feedback.Text=T("正在打开配置，请完成 Windows 权限提示…");
  setup(option,(code,message)=>{configuring=false;if(closed)return;feedback.Text=Language.Tr(message);Controls();Refresh();});
 }
 // Exported reports may be shared publicly, so the profile path and computer name are masked.
 static string Redact(string text){
  string profile=Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
  if(profile.Length>3)text=System.Text.RegularExpressions.Regex.Replace(text,System.Text.RegularExpressions.Regex.Escape(profile),"%USERPROFILE%",System.Text.RegularExpressions.RegexOptions.IgnoreCase);
  return System.Text.RegularExpressions.Regex.Replace(text,@"\b"+System.Text.RegularExpressions.Regex.Escape(Environment.MachineName)+@"\b","<computer>",System.Text.RegularExpressions.RegexOptions.IgnoreCase);
 }
 void Open(string path){Process.Start(new ProcessStartInfo(path){UseShellExecute=true});}
 void Save(){var picker=new SaveFileDialog{FileName="PyroScreen-diagnostics-"+DateTime.Now.ToString("yyyyMMdd-HHmmss")+".txt",Filter="Text (*.txt)|*.txt"};if(picker.ShowDialog(dialog)==true){File.WriteAllText(picker.FileName,Redact(report.Text),new UTF8Encoding(false));feedback.Text=T("报告已保存：")+picker.FileName;}}
}
}
