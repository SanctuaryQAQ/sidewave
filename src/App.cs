using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Principal;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using System.IO.Pipes;
using System.Security.AccessControl;
using System.Runtime.InteropServices;
using System.Windows.Interop;
using Microsoft.Win32;
using Forms=System.Windows.Forms;
namespace PyroScreen {
class App {
 static string backendOverride;
 static bool? segmentationOverride;
 readonly bool client;
 readonly Settings settings;
 readonly Window window;
 readonly ConcurrentQueue<Action> commands=new ConcurrentQueue<Action>();
 readonly Dictionary<string,Peer> peers=new Dictionary<string,Peer>();
 Forms.NotifyIcon tray;Discovery discovery;HostLink link;ClientService server;
 volatile bool stop,busy;bool exiting,manualStop;string busyAction="";DateTime nextRetry=DateTime.MinValue;int retrySeconds=1;
 DateTime lastStats,lastClock,lastScan;string lastStatus="";
 NetworkAddressChangedEventHandler networkChanged;long networkChangeAt;int poorNetworkSamples,networkVideoRecoveries;
 readonly VideoRecovery senderRecovery=new VideoRecovery();int senderVideoRecoveries;
 NamedPipeServerStream activation;
 List<Action> translations;bool changingLanguage;string clientAddressText="";
 static readonly int[] Budgets={400,600,750,900};
 static readonly int[,] Resolutions={{0,0},{1280,720},{1600,900},{1920,1080},{2048,1152},{2560,1440},{3200,1800},{3840,2160}};
 volatile string telemetryJson="{}";long telemetryTime;
 volatile string controlId="",controlState="",controlError="";bool syncingControls;
 public App(bool isClient,bool suppressAuto=false){
  manualStop=suppressAuto;
  client=isClient;Paths.Atomic(Path.Combine(Paths.Root,"app.json"),Json.Write(new{mode=client?"receive":"send"}));settings=Settings.Load(client?"client":"host");
  if(!client&&backendOverride!=null)settings.Backend=backendOverride;
  if(!client&&segmentationOverride.HasValue)settings.UdpSegmentation=segmentationOverride.Value;
  settings.Save(client?"client":"host");
  using(var source=File.OpenRead(Path.Combine(Paths.Bin,"MainWindow.xaml")))window=(Window)XamlReader.Load(source);
  // Bundled Noto Sans SC (SIL OFL); Segoe UI supplies half-width × and ·, Microsoft YaHei UI covers the rest of the subset gaps.
  string fonts=Path.Combine(Paths.Bin,"fonts")+Path.DirectorySeparatorChar;
  if(File.Exists(fonts+"NotoSansSC-Regular.otf"))window.FontFamily=new System.Windows.Media.FontFamily(new Uri(fonts),"./#Noto Sans SC, Segoe UI, Microsoft YaHei UI");
  window.Title="SideWave · "+Language.Tr(client?"接收模式":"发送模式");
  window.SourceInitialized+=(s,e)=>DarkCaption(new WindowInteropHelper(window).Handle);
  string image=Path.Combine(Paths.Bin,client?"client.png":"host.png");
  C<Image>("RoleIcon").Source=(System.Windows.Media.ImageSource)window.FindResource("IconApp");
  window.Icon=(System.Windows.Media.ImageSource)window.FindResource("IconApp");
  C<Button>("SendMode").IsEnabled=client;C<Button>("ReceiveMode").IsEnabled=!client;
  C<Button>("SendMode").Click+=(s,e)=>SwitchMode(false);
  C<Button>("ReceiveMode").Click+=(s,e)=>SwitchMode(true);
  C<Button>("Exit").Click+=(s,e)=>Exit();
  System.Windows.Automation.AutomationProperties.SetHelpText(C<Button>("Diagnostics"),Paths.Root);
  C<Button>("Diagnostics").ToolTip=Paths.Root;
  Paths.Log("startup","mode="+(client?"receive":"send")+" user="+WindowsIdentity.GetCurrent().Name+" admin="+Admin()+" root="+Paths.Root);
  C<Button>("Diagnostics").Click+=(s,e)=>Maintenance("diagnostics");
  C<Button>("Firewall").Click+=(s,e)=>Maintenance("network");
  C<Button>("Driver").Click+=(s,e)=>Maintenance("driver");
  C<Button>("Forget").Click+=(s,e)=>{
   if(MessageBox.Show(window,Language.Tr("删除已配对设备，下一次连接需要重新输入配对码。"),Language.Tr("忘记配对"),MessageBoxButton.OKCancel)!=MessageBoxResult.OK)return;
   Queue(()=>{manualStop=true;if(link!=null)link.Disconnect();if(server!=null)server.ForgetPairings();else{settings.ForgetTokens("host");Buttons();}SetStatus("配对已删除");});
  };
  window.Closing+=(s,e)=>{if(!exiting){e.Cancel=true;window.Hide();tray.ShowBalloonTip(1200,"SideWave",Language.Tr("软件继续在托盘运行。使用“退出”结束副屏。"),Forms.ToolTipIcon.Info);}};
  tray=new Forms.NotifyIcon{Visible=true,Text=client?"SideWave 接收模式":"SideWave 发送模式",
    Icon=new System.Drawing.Icon(Path.Combine(Paths.Bin,"app.ico"))};
  tray.DoubleClick+=(s,e)=>Show();
  var menu=new Forms.ContextMenuStrip();menu.Items.Add("打开",null,(s,e)=>Show());menu.Items.Add("退出",null,(s,e)=>Exit());tray.ContextMenuStrip=menu;
  if(client)InitClient();else InitHost();InitStartup();
  if(!client){networkChanged=(s,e)=>Interlocked.Exchange(ref networkChangeAt,DateTime.UtcNow.Ticks);NetworkChange.NetworkAddressChanged+=networkChanged;}
  translations=Language.Capture(window);InitLanguage();
  new Thread(Worker){IsBackground=true,Name="Application control"}.Start();
  new Thread(Activation){IsBackground=true,Name="Show existing window"}.Start();
  window.Show();
 }
 static string PipeName(){return "PyroScreen-"+WindowsIdentity.GetCurrent().User.Value;}
 void Activation(){
  var security=new PipeSecurity();security.AddAccessRule(new PipeAccessRule(WindowsIdentity.GetCurrent().User,PipeAccessRights.FullControl,AccessControlType.Allow));
  while(!stop)try{
   using(var pipe=new NamedPipeServerStream(PipeName(),PipeDirection.InOut,1,PipeTransmissionMode.Byte,PipeOptions.Asynchronous,256,256,security)){
    activation=pipe;pipe.WaitForConnection();byte[] bytes=new byte[4];int n=0;
    while(n<4){int got=pipe.Read(bytes,n,4-n);if(got==0)break;n+=got;}
    if(n==4){string command=System.Text.Encoding.ASCII.GetString(bytes);if(command=="INFO"){byte[] reply=System.Text.Encoding.UTF8.GetBytes(Json.Write(new{controlApi=1,busy=busy,lastStatus=lastStatus,controlId=controlId,controlState=controlState,controlError=controlError,mode=client?"receive":"send",deviceId=settings.DeviceId,activeHostId=server==null?"":server.ActiveHostId,protectionPacketBytes=settings.ProtectionPacketBytes,sendPacingMbps=settings.SendPacingMbps,captureBackpressure=settings.CaptureBackpressure,pipelinedSend=settings.PipelinedSend,skipImmediateSend=settings.SkipImmediateSend,partialFrames=settings.PartialFrames,clientCursor=settings.ClientCursor,captureOnUpdate=settings.CaptureOnUpdate,rioSend=settings.RioSend,rioReceive=settings.RioReceive,receiveCpu=settings.ReceiveCpu,udpSegmentation=settings.UdpSegmentation,connectedUdp=settings.ConnectedUdp,sendBufferBytes=settings.SendBufferBytes,sendWindow=settings.SendWindow,networkVideoRecoveries=networkVideoRecoveries,senderVideoRecoveries=senderVideoRecoveries,senderRecoveryAttempts=senderRecovery.Attempts,senderPid=link==null?0:link.SenderPid,senderExitDiagnostic=link==null?"":link.SenderExitDiagnostic,videoRequested=link!=null&&link.VideoRequested,pid=Process.GetCurrentProcess().Id,root=Paths.Root,runtime=Paths.Runtime,connection=link==null?"":link.ConnectionId,display=link==null?"":link.DisplayName,streaming=link!=null&&link.Streaming,receiverPid=link==null?0:link.ReceiverPid,width=settings.Width,height=settings.Height,fps=settings.Fps,budgetMbps=settings.BudgetMbps,effectiveFps=link==null?0:link.EffectiveFps,linkBudgetMbps=link==null?0:link.LinkBudgetMbps,packetProtection=settings.PacketProtection,videoEncryption=settings.VideoEncryption,allowTearing=settings.AllowTearing,autoConnect=settings.AutoConnect,language=Language.Code,backend=settings.Backend,telemetryAgeMs=telemetryTime==0?-1:(DateTime.UtcNow.Ticks-Interlocked.Read(ref telemetryTime))/10000,stats=Json.Read(telemetryJson)}));pipe.Write(BitConverter.GetBytes(reply.Length),0,4);pipe.Write(reply,0,reply.Length);}else if(command=="CTRL"){try{int size=BitConverter.ToInt32(LocalControl.Read(pipe,4),0);if(size<2||size>16384)throw new IOException("Invalid local request size.");var request=Json.Read(System.Text.Encoding.UTF8.GetString(LocalControl.Read(pipe,size)));LocalControl.Reply(pipe,AcceptControl(request));}catch(Exception e){LocalControl.Reply(pipe,new{accepted=false,error=e.Message});}}else if(command=="SHOW")UI(Show);else if(command=="SEND"||command=="RECV")UI(()=>SwitchMode(command=="RECV"));}
   }
  }catch(ObjectDisposedException){break;}catch(IOException){Thread.Sleep(100);}
 }
 T C<T>(string name)where T:class{return window.FindName(name)as T;}
 static int ControlInt(Dictionary<string,object> r,string name,int fallback){object v;if(!r.TryGetValue(name,out v))return fallback;if(v is int)return (int)v;throw new IOException("Expected integer: "+name);}
 static bool ControlBool(Dictionary<string,object> r,string name,bool fallback){object v;if(!r.TryGetValue(name,out v))return fallback;if(v is bool)return (bool)v;throw new IOException("Expected boolean: "+name);}
 object AcceptControl(Dictionary<string,object> request){
  string operation=Json.S(request,"operation");
  if(operation!="apply"&&operation!="stop-video"&&operation!="disconnect"&&operation!="quit")throw new IOException("Unknown local operation.");
  if(client&&operation!="quit")throw new IOException("This operation requires sender mode.");
  if(exiting||busy||controlState=="queued"||controlState=="running")throw new IOException("Application is busy.");
  string[] fields={"operation","width","height","fps","budgetMbps","videoEncryption","packetProtection","protectionPacketBytes","sendPacingMbps","captureBackpressure","pipelinedSend","skipImmediateSend","partialFrames","captureOnUpdate","rioSend","rioReceive","receiveCpu","udpSegmentation","connectedUdp","sendBufferBytes","sendWindow","allowTearing","clientCursor"};
  if(request.Keys.Any(k=>!fields.Contains(k)))throw new IOException("Unknown control field.");
  if(operation!="apply"&&request.Count!=1)throw new IOException("This operation does not accept display settings.");
  string id=Guid.NewGuid().ToString("N");controlId=id;controlError="";controlState="queued";
  Queue(()=>{
   controlState="running";
   try{
    if(operation=="quit")UI(()=>Exit());
    else if(operation=="apply")ApplyControl(request);
    else{manualStop=true;BeginOperation("disconnect","正在应用画面设置…");try{if(operation=="stop-video")link.StopVideo();else link.Disconnect();ClearTelemetry();SetStatus(operation=="stop-video"?"保持连接 · 视频已停止":"已断开");}finally{busy=false;Buttons();}}
    controlState="ok";
   }catch(Exception e){controlError=e.Message;controlState="error";SetStatus(e.Message);}
  });
  return new{accepted=true,id=id};
 }
 void ApplyControl(Dictionary<string,object> r){
  int w=ControlInt(r,"width",settings.NativeResolution==true?0:settings.Width),h=ControlInt(r,"height",settings.NativeResolution==true?0:settings.Height);
  int fps=ControlInt(r,"fps",settings.Fps),budget=ControlInt(r,"budgetMbps",settings.BudgetMbps);
  int protectionBytes=ControlInt(r,"protectionPacketBytes",settings.ProtectionPacketBytes);
  if(!new[]{4096,8192,16384,32768}.Contains(protectionBytes))throw new IOException("Unsupported protection packet size.");
  int pacing=ControlInt(r,"sendPacingMbps",settings.SendPacingMbps);
  bool rio=ControlBool(r,"rioSend",settings.RioSend),oldRio=settings.RioSend;
  bool rioReceive=ControlBool(r,"rioReceive",settings.RioReceive),oldRioReceive=settings.RioReceive;
  int receiveCpu=ControlInt(r,"receiveCpu",settings.ReceiveCpu),oldReceiveCpu=settings.ReceiveCpu;
  if(receiveCpu<-1||receiveCpu>63)throw new IOException("Invalid receiver processor index.");
  bool segmentation=ControlBool(r,"udpSegmentation",settings.UdpSegmentation),oldSegmentation=settings.UdpSegmentation;
  bool connectedUdp=ControlBool(r,"connectedUdp",settings.ConnectedUdp),oldConnected=settings.ConnectedUdp;
  int sendBuffer=ControlInt(r,"sendBufferBytes",settings.SendBufferBytes),oldSendBuffer=settings.SendBufferBytes;
  int sendWindow=ControlInt(r,"sendWindow",settings.SendWindow),oldSendWindow=settings.SendWindow;
  if(!new[]{32,64,128,256}.Contains(sendWindow))throw new IOException("Unsupported bounded datagram window.");
  if(!new[]{0,65536,131072,262144,524288,1048576,2097152,4194304}.Contains(sendBuffer))throw new IOException("Unsupported bounded send buffer size.");
  if(rio&&connectedUdp)throw new IOException("Connected sends are tested separately from RIO.");
  if(rio&&segmentation)throw new IOException("Select one sender transport.");
  if(pacing!=0&&(pacing<100||pacing>900)||pacing>0&&(segmentation||rio))throw new IOException("Unsupported datagram pacing setting.");
  bool encryption=ControlBool(r,"videoEncryption",settings.VideoEncryption),protection=ControlBool(r,"packetProtection",settings.PacketProtection),tearing=ControlBool(r,"allowTearing",settings.AllowTearing);
  bool backpressure=ControlBool(r,"captureBackpressure",settings.CaptureBackpressure),oldBackpressure=settings.CaptureBackpressure;
  bool pipeline=ControlBool(r,"pipelinedSend",settings.PipelinedSend),oldPipeline=settings.PipelinedSend;
  bool skipImmediate=ControlBool(r,"skipImmediateSend",settings.SkipImmediateSend),oldSkipImmediate=settings.SkipImmediateSend;
  bool partialFrames=ControlBool(r,"partialFrames",settings.PartialFrames),oldPartialFrames=settings.PartialFrames;
  bool clientCursor=ControlBool(r,"clientCursor",settings.ClientCursor),oldClientCursor=settings.ClientCursor;
  bool captureOnUpdate=ControlBool(r,"captureOnUpdate",settings.CaptureOnUpdate),oldCaptureOnUpdate=settings.CaptureOnUpdate;
  if(!new[]{60,120,165,240}.Contains(fps)||!Budgets.Contains(budget))throw new IOException("Unsupported frame rate or bandwidth preset.");
  bool resolution=false;for(int i=0;i<Resolutions.GetLength(0);++i)if(w==Resolutions[i,0]&&h==Resolutions[i,1])resolution=true;
  if(!resolution)throw new IOException("Unsupported resolution preset.");
  int oldW=settings.Width,oldH=settings.Height,oldF=settings.Fps,oldB=settings.BudgetMbps,oldPid=link.ReceiverPid,oldProtectionBytes=settings.ProtectionPacketBytes,oldPacing=settings.SendPacingMbps;
  bool oldEncryption=settings.VideoEncryption,oldProtection=settings.PacketProtection,oldTearing=settings.AllowTearing;bool? oldNative=settings.NativeResolution;
  manualStop=false;senderRecovery.Reset();BeginOperation("apply","正在应用画面设置…");
  try{
   settings.VideoEncryption=encryption;settings.PacketProtection=protection;settings.ProtectionPacketBytes=protectionBytes;settings.SendPacingMbps=pacing;settings.CaptureBackpressure=backpressure;settings.AllowTearing=tearing;settings.NativeResolution=w==0;
   settings.PipelinedSend=pipeline;
   settings.SkipImmediateSend=skipImmediate;
   settings.PartialFrames=partialFrames;
   settings.ClientCursor=clientCursor;
   settings.CaptureOnUpdate=captureOnUpdate;
   settings.RioSend=rio;settings.UdpSegmentation=segmentation;
   settings.RioReceive=rioReceive;
   settings.ReceiveCpu=receiveCpu;
   settings.ConnectedUdp=connectedUdp;
   settings.SendBufferBytes=sendBuffer;
   settings.SendWindow=sendWindow;
   if(!link.Connected){Connect(settings.LastIp,"",w,h,fps,budget);if(!link.Streaming)throw new IOException(lastStatus);}
   else{if(w==0){w=link.NativeWidth;h=link.NativeHeight;}link.Apply(w,h,fps,budget);}
   settings.Save("host");
   UI(()=>{syncingControls=true;try{
    C<CheckBox>("VideoEncryption").IsChecked=settings.VideoEncryption;C<CheckBox>("PacketProtection").IsChecked=settings.PacketProtection;C<CheckBox>("AllowTearing").IsChecked=settings.AllowTearing;C<CheckBox>("PartialFrames").IsChecked=settings.PartialFrames;C<CheckBox>("ClientCursor").IsChecked=settings.ClientCursor;
    C<ComboBox>("FrameRate").SelectedIndex=Array.IndexOf(new[]{60,120,165,240},settings.Fps);C<ComboBox>("Quality").SelectedIndex=Array.IndexOf(Budgets,settings.BudgetMbps);
    int ri=0;if(settings.NativeResolution!=true)for(int i=1;i<Resolutions.GetLength(0);++i)if(settings.Width==Resolutions[i,0]&&settings.Height==Resolutions[i,1])ri=i;C<ComboBox>("Resolution").SelectedIndex=ri;
   }finally{syncingControls=false;}});
  }catch{
   settings.VideoEncryption=oldEncryption;settings.PacketProtection=oldProtection;settings.ProtectionPacketBytes=oldProtectionBytes;settings.SendPacingMbps=oldPacing;settings.CaptureBackpressure=oldBackpressure;settings.AllowTearing=oldTearing;settings.NativeResolution=oldNative;
   settings.PipelinedSend=oldPipeline;
   settings.SkipImmediateSend=oldSkipImmediate;
   settings.PartialFrames=oldPartialFrames;
   settings.ClientCursor=oldClientCursor;
   settings.CaptureOnUpdate=oldCaptureOnUpdate;
   settings.RioSend=oldRio;settings.UdpSegmentation=oldSegmentation;
   settings.RioReceive=oldRioReceive;
   settings.ReceiveCpu=oldReceiveCpu;
   settings.ConnectedUdp=oldConnected;
   settings.SendBufferBytes=oldSendBuffer;
   settings.SendWindow=oldSendWindow;
   if(link.Connected)try{link.Apply(oldW,oldH,oldF,oldB);}catch{}
   throw;
  }finally{if(link.ReceiverPid!=oldPid)ClearTelemetry();busy=false;Buttons();}
 }
 [DllImport("dwmapi.dll")]static extern int DwmSetWindowAttribute(IntPtr window,int attribute,ref int value,int bytes);
 static void DarkCaption(IntPtr handle){int dark=1,color=0x141414,text=0xF3F3F3;DwmSetWindowAttribute(handle,20,ref dark,4);DwmSetWindowAttribute(handle,35,ref color,4);DwmSetWindowAttribute(handle,36,ref text,4);}
 void UI(Action a){if(!stop)window.Dispatcher.BeginInvoke(a);}
 void Show(){window.Show();window.WindowState=WindowState.Normal;window.Activate();}
 void Queue(Action a){commands.Enqueue(a);}
 void SetStatus(string s){if(s!=lastStatus)Paths.Log("app",s);lastStatus=s;UI(()=>C<TextBlock>("Status").Text=Language.Tr(s));}
 void InitClient(){
  C<Grid>("HostPanel").Visibility=Visibility.Collapsed;C<Border>("ClientPanel").Visibility=Visibility.Visible;
  C<Button>("Driver").Visibility=Visibility.Collapsed;
  C<TextBlock>("Telemetry").Text="";
  clientAddressText="本机地址："+String.Join(" · ",NetworkInterface.GetAllNetworkInterfaces()
   .Where(n=>n.OperationalStatus==OperationalStatus.Up).SelectMany(n=>n.GetIPProperties().UnicastAddresses)
   .Where(a=>a.Address.AddressFamily==AddressFamily.InterNetwork&&!System.Net.IPAddress.IsLoopback(a.Address)).Select(a=>a.Address.ToString()));
  server=new ClientService(settings);server.Status+=SetStatus;
  Action showCode=()=>{string code=server.Code;C<TextBox>("ClientCode").Text=String.Join(" - ",Enumerable.Range(0,4).Select(i=>code.Substring(i*4,4)));};
  showCode();
  C<Button>("CopyCode").Click+=(s,e)=>{Clipboard.SetText(server.Code);SetStatus("配对码已复制。");};
  C<Button>("RenewCode").Click+=(s,e)=>{server.RenewCode();showCode();SetStatus("配对码已刷新，10 分钟内有效。");};
  Queue(()=>server.Start());
 }
 void InitLanguage(){
  var choice=C<ComboBox>("LanguageChoice");choice.SelectedIndex=Language.Code=="en"?1:0;
  choice.SelectionChanged+=(sender,e)=>{
   if(changingLanguage)return;string old=Language.Code;
   try{Language.Select(choice.SelectedIndex==1?"en":"zh");RefreshLanguage();}
   catch(Exception error){changingLanguage=true;choice.SelectedIndex=old=="en"?1:0;changingLanguage=false;SetStatus("无法保存语言设置："+error.Message);}
  };
  RefreshLanguage();
 }
 void RefreshLanguage(){
  foreach(var translate in translations)translate();
  window.Title="SideWave · "+Language.Tr(client?"接收模式":"发送模式");
  tray.Text="SideWave "+Language.Tr(client?"接收模式":"发送模式");
  tray.ContextMenuStrip.Items[0].Text=Language.Tr("打开");tray.ContextMenuStrip.Items[1].Text=Language.Tr("退出");
  if(client){C<TextBlock>("ClientAddresses").Text=Language.Tr(clientAddressText);}
  else{Buttons();C<Button>("Scan").Content=Language.Tr(C<Button>("Scan").IsEnabled?"刷新":"搜索中…");}
  C<TextBlock>("Status").Text=Language.Tr(lastStatus);
 }
 void InitStartup(){
  bool enabled=false;
  using(var key=Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run")){
   if(key!=null){enabled=key.GetValue("PyroScreen")!=null;
    if(!enabled&&key.GetValue("PyroScreenClient")!=null){SetStartup(true);enabled=true;}}
  }
  C<CheckBox>("Startup").IsChecked=enabled;
  C<CheckBox>("Startup").ToolTip="登录后在托盘运行上次模式；发送模式使用已保存的自动连接设置。";
  bool updating=false;RoutedEventHandler changed=(s,e)=>{
   if(updating)return;
   bool requested=C<CheckBox>("Startup").IsChecked==true;
   try{SetStartup(requested);enabled=requested;SetStatus(requested?"已开启随 Windows 启动，使用上次模式。":"已关闭随 Windows 启动。");}
   catch(Exception error){updating=true;C<CheckBox>("Startup").IsChecked=enabled;updating=false;SetStatus("无法修改自启动："+error.Message);}
  };
  C<CheckBox>("Startup").Checked+=changed;C<CheckBox>("Startup").Unchecked+=changed;
 }
 void SetStartup(bool enabled){
  using(var key=Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run")){
   if(enabled)key.SetValue("PyroScreen",Engine.Q(Process.GetCurrentProcess().MainModule.FileName)+" --tray");
   else key.DeleteValue("PyroScreen",false);
   key.DeleteValue("PyroScreenClient",false);
  }
 }
 void InitHost(){
  link=new HostLink(settings);link.Status+=SetStatus;
  var resolutions=C<ComboBox>("Resolution");
  for(int i=0;i<Resolutions.GetLength(0);++i)resolutions.Items.Add(new ComboBoxItem{Content=i==0?"副机原生分辨率":Resolutions[i,0]+" × "+Resolutions[i,1]});
  int selected=0;
  if(settings.NativeResolution==false||(!settings.NativeResolution.HasValue&&settings.Width!=2560))
   for(int i=1;i<Resolutions.GetLength(0);++i)if(Resolutions[i,0]==settings.Width&&Resolutions[i,1]==settings.Height)selected=i;
  resolutions.SelectedIndex=selected;
  // Reconnect and the control API follow the same default the selector shows.
  if(!settings.NativeResolution.HasValue)settings.NativeResolution=selected==0;
  C<ComboBox>("FrameRate").SelectedIndex=Array.IndexOf(new[]{60,120,165,240},settings.Fps);
  C<ComboBox>("Quality").SelectedIndex=Math.Max(0,Array.IndexOf(Budgets,settings.BudgetMbps));
  C<CheckBox>("AutoConnect").IsChecked=settings.AutoConnect;
  C<CheckBox>("VideoEncryption").IsChecked=settings.VideoEncryption;
  RoutedEventHandler encryptionChanged=(s,e)=>{if(!syncingControls)SetStatus("视频加密设置已更改，点击“应用画面设置”生效。");};
  C<CheckBox>("VideoEncryption").Checked+=encryptionChanged;C<CheckBox>("VideoEncryption").Unchecked+=encryptionChanged;
  C<CheckBox>("AllowTearing").IsChecked=settings.AllowTearing;
  RoutedEventHandler tearingChanged=(s,e)=>{if(!syncingControls)SetStatus("低延迟显示设置已更改，点击“应用画面设置”生效。");};
  C<CheckBox>("AllowTearing").Checked+=tearingChanged;C<CheckBox>("AllowTearing").Unchecked+=tearingChanged;
  C<CheckBox>("PartialFrames").IsChecked=settings.PartialFrames;
  RoutedEventHandler partialChanged=(s,e)=>{if(!syncingControls)SetStatus("变化区域传输设置已更改，点击“应用画面设置”生效。");};
  C<CheckBox>("PartialFrames").Checked+=partialChanged;C<CheckBox>("PartialFrames").Unchecked+=partialChanged;
  C<CheckBox>("ClientCursor").IsChecked=settings.ClientCursor;
  RoutedEventHandler cursorChanged=(s,e)=>{if(!syncingControls)SetStatus("光标显示方式已更改，点击“应用画面设置”生效。");};
  C<CheckBox>("ClientCursor").Checked+=cursorChanged;C<CheckBox>("ClientCursor").Unchecked+=cursorChanged;
  C<CheckBox>("PacketProtection").IsChecked=settings.PacketProtection;
  RoutedEventHandler autoChanged=(s,e)=>{settings.AutoConnect=C<CheckBox>("AutoConnect").IsChecked==true;settings.Save("host");SetStatus(settings.AutoConnect?"已开启启动时自动连接上次副机。":"已关闭自动连接。");if(!settings.AutoConnect)Queue(()=>{if(!link.Connected)link.Disconnect();Buttons();});};
  C<CheckBox>("AutoConnect").Checked+=autoChanged;C<CheckBox>("AutoConnect").Unchecked+=autoChanged;
  RoutedEventHandler protectionChanged=(s,e)=>{if(!syncingControls)SetStatus("丢包保护设置已更改，点击“应用画面设置”生效。");};
  C<CheckBox>("PacketProtection").Checked+=protectionChanged;C<CheckBox>("PacketProtection").Unchecked+=protectionChanged;
  C<TextBox>("Address").Text=settings.LastIp;
  discovery=new Discovery(settings.DeviceId,null);
  discovery.Found+=p=>UI(()=>{
   peers[p.Id]=p;
   RefreshPeers();
  });
  C<Button>("Scan").Click+=(s,e)=>{
   var button=C<Button>("Scan");button.IsEnabled=false;button.Content=Language.Tr("搜索中…");SetStatus("正在搜索局域网副机…");
   string ip=C<TextBox>("Address").Text.Trim();Queue(()=>discovery.Scan(ip));
   var feedback=new DispatcherTimer{Interval=TimeSpan.FromSeconds(2)};
   feedback.Tick+=(sender,eventArgs)=>{feedback.Stop();if(stop)return;RefreshPeers();button.Content=Language.Tr("刷新");button.IsEnabled=true;
    if(!busy)SetStatus(peers.Count==0?"未找到副机，请确认副机处于接收模式并允许专用网络。":"已找到 "+peers.Count+" 台副机。");};feedback.Start();
  };
  C<ListBox>("Devices").SelectionChanged+=(s,e)=>{var p=C<ListBox>("Devices").SelectedItem as Peer;if(p!=null)C<TextBox>("Address").Text=p.Ip;};
  C<Button>("Connect").Click+=(s,e)=>{
   string ip=C<TextBox>("Address").Text.Trim(),code=C<TextBox>("PairCode").Text;
   if(busy)return;manualStop=false;ReadMode((w,h,fps,budget)=>{BeginOperation("connect","正在安全连接副机…");Queue(()=>Connect(ip,code,w,h,fps,budget));});
  };
  C<Button>("Disconnect").Click+=(s,e)=>{if(busy)return;manualStop=true;BeginOperation("disconnect","正在断开副机…");Queue(()=>{try{link.Disconnect();SetStatus("已断开");}finally{busy=false;Buttons();}});};
  C<Button>("Apply").Click+=(s,e)=>{if(busy)return;ReadMode((w,h,fps,budget)=>{BeginOperation("apply","正在应用画面设置…");Queue(()=>Apply(w,h,fps,budget));});};
  SetStatus("选择局域网副机，或输入 IP 地址");
 }
 void RefreshPeers(){
  DateTime now=DateTime.UtcNow;
  foreach(string id in peers.Where(v=>now-v.Value.Seen>TimeSpan.FromSeconds(12)).Select(v=>v.Key).ToArray())peers.Remove(id);
  var selected=C<ListBox>("Devices").SelectedItem as Peer;
  C<ListBox>("Devices").ItemsSource=peers.Values.OrderBy(v=>v.Name).ToArray();
  if(selected!=null)C<ListBox>("Devices").SelectedItem=peers.Values.FirstOrDefault(v=>v.Id==selected.Id);
 }
 void ReadMode(Action<int,int,int,int> action){
  settings.VideoEncryption=C<CheckBox>("VideoEncryption").IsChecked==true;
  settings.AllowTearing=C<CheckBox>("AllowTearing").IsChecked==true;
  settings.PartialFrames=C<CheckBox>("PartialFrames").IsChecked==true;
  settings.ClientCursor=C<CheckBox>("ClientCursor").IsChecked==true;
  settings.PacketProtection=C<CheckBox>("PacketProtection").IsChecked==true;
  int index=C<ComboBox>("Resolution").SelectedIndex,w=0,h=0;
  index=Math.Max(0,Math.Min(index,Resolutions.GetLength(0)-1));w=Resolutions[index,0];h=Resolutions[index,1];settings.NativeResolution=index==0;
  int fi=Math.Max(0,C<ComboBox>("FrameRate").SelectedIndex),q=Math.Max(0,C<ComboBox>("Quality").SelectedIndex);
  action(w,h,new[]{60,120,165,240}[fi],Budgets[Math.Min(q,Budgets.Length-1)]);
 }
 void Connect(string ip,string code,int w,int h,int fps,int budget){
  senderRecovery.Reset();
  BeginOperation("connect","正在安全连接副机…");
  try{link.Connect(ip,code);if(w==0){w=link.NativeWidth;h=link.NativeHeight;}
   link.Apply(w,h,fps,budget);retrySeconds=1;UI(()=>C<TextBox>("PairCode").Clear());
  }catch(Exception e){link.DropConnection(settings.AutoConnect&&!manualStop);SetStatus(e.Message);ScheduleRetry();}finally{busy=false;Buttons();}
 }
 void Apply(int w,int h,int fps,int budget){
  manualStop=false;senderRecovery.Reset();
  BeginOperation("apply","正在应用画面设置…");
  int oldW=settings.Width,oldH=settings.Height,oldF=settings.Fps,oldB=settings.BudgetMbps,oldPid=link.ReceiverPid;
  try{if(w==0){w=link.NativeWidth;h=link.NativeHeight;}link.Apply(w,h,fps,budget);}
  catch(Exception e){
   SetStatus("切换失败："+e.Message+" 正在恢复原模式。");
   try{link.Apply(oldW,oldH,oldF,oldB);}catch(Exception restore){SetStatus("恢复失败："+restore.Message);}
  }finally{if(link.ReceiverPid!=oldPid)ClearTelemetry();busy=false;Buttons();}
 }
 void ClearTelemetry(){telemetryJson="{}";Interlocked.Exchange(ref telemetryTime,0);UI(()=>C<TextBlock>("Telemetry").Text="");}
 void BeginOperation(string action,string status){if(action=="connect")ClearTelemetry();busyAction=action;busy=true;SetStatus(status);Buttons();}
 void Buttons(){UI(()=>{
  C<Button>("Connect").IsEnabled=!busy&&!link.Connected;C<Button>("Disconnect").IsEnabled=!busy&&(link.Connected||link.HasDisplay);
  C<Button>("Apply").IsEnabled=!busy&&link.Connected;
  bool slowLink=link.Connected&&((link.LocalLinkMbps>0&&link.LocalLinkMbps<1000)||(link.RemoteLinkMbps>0&&link.RemoteLinkMbps<1000));
  C<TextBlock>("BandwidthNote").Text=Language.Tr((slowLink?"网卡连接低于千兆（本机 "+link.LocalLinkMbps+" / 副机 "+link.RemoteLinkMbps+" Mbps），帧率和画质会受限。请检查网线后重新连接。":""));
  C<Button>("Connect").Content=Language.Tr(busy&&busyAction=="connect"?"正在连接…":"连接副机");
  C<Button>("Disconnect").Content=Language.Tr(busy&&busyAction=="disconnect"?"正在断开…":"断开");
  C<Button>("Apply").Content=Language.Tr(busy&&busyAction=="apply"?"正在应用…":"应用画面设置");
  C<ComboBox>("Resolution").IsEnabled=!busy;C<ComboBox>("FrameRate").IsEnabled=!busy;C<ComboBox>("Quality").IsEnabled=!busy;
  C<CheckBox>("VideoEncryption").IsEnabled=!busy;C<CheckBox>("PacketProtection").IsEnabled=!busy;C<CheckBox>("AllowTearing").IsEnabled=!busy;C<CheckBox>("PartialFrames").IsEnabled=!busy;C<CheckBox>("ClientCursor").IsEnabled=!busy;
  if(!link.Connected)C<TextBlock>("Telemetry").Text="";
 });}
 void ScheduleRetry(){nextRetry=DateTime.UtcNow.AddSeconds(retrySeconds);retrySeconds=Math.Min(30,retrySeconds*2);}
 static double Number(Dictionary<string,object> data,string key){double value;return data!=null&&Double.TryParse(Json.S(data,key,"0"),System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out value)?value:0;}
 void RecoverExitedSender(DateTime now){
  if(manualStop||!link.VideoRequested){senderRecovery.Reset();return;}
  if(link.Streaming){senderRecovery.Healthy(now);return;}
  if(senderRecovery.Failed(now)){
   Paths.Log("host",link.SenderExitDiagnostic);
   ClearTelemetry();SetStatus(senderRecovery.Exhausted?"发送器连续退出，点击应用可重试。":"发送器意外退出，正在准备恢复画面…");
  }
  if(!senderRecovery.TryAttempt(now))return;
  BeginOperation("apply","正在恢复画面（"+senderRecovery.Attempts+"/3）…");
  try{
   link.RecoverVideo();++senderVideoRecoveries;senderRecovery.Recovered(DateTime.UtcNow);
   SetStatus("发送器已重启 · 保持原连接和虚拟屏");
  }catch(Exception e){
   Paths.Log("host","Sender recovery attempt "+senderRecovery.Attempts+" failed: "+e.Message);
   SetStatus(senderRecovery.Exhausted?"画面恢复失败，点击应用可重试："+e.Message:"画面恢复失败，稍后重试："+e.Message);
  }finally{busy=false;Buttons();}
 }
 void RecoverVideoAfterNetworkChange(Dictionary<string,object> data){
  long changed=Interlocked.Read(ref networkChangeAt);if(changed==0)return;
  double age=(DateTime.UtcNow.Ticks-changed)/10000000.0;
  if(age<3)return;
  if(age>90){Interlocked.CompareExchange(ref networkChangeAt,0,changed);poorNetworkSamples=0;return;}
  if(manualStop||!link.Streaming||Number(data,"elapsed_s")<5){poorNetworkSamples=0;return;}
  double received=Number(data,"network_complete_fps_1s");
  Dictionary<string,object> tx;
  try{tx=Json.Read(File.ReadAllText(Path.Combine(Paths.Runtime,"sender-live.json")));}catch{return;}
  double sent=Number(tx,"udp_send_fps");
  if(sent<link.EffectiveFps*0.8||received>=sent*0.25){poorNetworkSamples=0;return;}
  if(++poorNetworkSamples<3)return;
  if(Interlocked.CompareExchange(ref networkChangeAt,0,changed)!=changed)return;
  poorNetworkSamples=0;BeginOperation("apply","正在应用画面设置…");
  try{
   Paths.Log("transport","Video delivery did not resume after a local network change; recreating video resources.");
   link.StopVideo();ClearTelemetry();link.Apply(settings.Width,settings.Height,settings.Fps,settings.BudgetMbps);++networkVideoRecoveries;
  }finally{busy=false;Buttons();}
 }
 void Worker(){
  while(!stop){
   Action a;if(commands.TryDequeue(out a)){try{a();}catch(Exception e){SetStatus(e.Message);}continue;}
   if(!client){
    DateTime now=DateTime.UtcNow;
    if((now-lastScan).TotalSeconds>=3){lastScan=now;discovery.Scan();UI(RefreshPeers);}
    if(!busy&&link.Connected&&(now-lastStats).TotalSeconds>=1){
     lastStats=now;
     try{
      var stats=link.Stats();var data=stats.ContainsKey("data")?stats["data"]as Dictionary<string,object>:null;
      if(data!=null&&(link.ReceiverPid==0||Json.I(data,"pid")==link.ReceiverPid)){
       telemetryJson=Json.Write(data);Interlocked.Exchange(ref telemetryTime,DateTime.UtcNow.Ticks);
       Paths.Atomic(Path.Combine(Paths.Runtime,"remote-stats.json"),telemetryJson);
       bool displayFeedback=Json.S(data,"display_feedback_available")=="True";
       string fps=displayFeedback?Json.S(data,"display_observed_fps_1s","--"):Json.S(data,"present_fps_1s","--");
       bool valid=Json.S(data,"hud_latency_available")=="True";
       bool displayEndpoint=Json.S(data,"hud_latency_endpoint")=="system_display";
       string latency=valid?Json.S(data,"hud_latency_ms","--"):"--",bound=Json.S(data,"clock_error_bound_ms","--");
       double number;
       if(Double.TryParse(fps,System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out number))fps=Math.Round(number).ToString("0");
       if(Double.TryParse(latency,System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out number))latency=number.ToString("0.0");
       UI(()=>{var label=C<TextBlock>("Telemetry");label.Text=Language.Tr("FPS "+fps+" · 全链路延迟 "+latency+" ms · 时钟 ±"+bound+" ms");label.ToolTip=Language.Tr("台式机桌面内容更新到笔记本系统显示反馈，包含渲染和显示等待；不包含液晶面板响应时间。");});
      }
      // A receiver window closed on the laptop cancels video intent even if the sender
      // exited concurrently. Never resurrect a deliberately closed receiver.
      if(link.VideoRequested&&link.ReceiverPid>0&&Json.S(stats,"alive")=="False"){
       link.StopVideo();senderRecovery.Reset();ClearTelemetry();
       SetStatus(Json.I(stats,"exitCode")==0?"副机已返回本地桌面，点击应用可继续。":"副机视频组件已退出，点击应用可重试："+Json.S(stats,"exitDiagnostic"));
      }
      RecoverExitedSender(now);
      if(data!=null&&link.ReceiverPid>0&&Json.I(data,"pid")==link.ReceiverPid)RecoverVideoAfterNetworkChange(data);
      if((now-lastClock).TotalSeconds>=5){link.Calibrate();lastClock=now;}
     }catch(Exception e){link.DropConnection(settings.AutoConnect&&!manualStop);SetStatus("连接中断，正在等待副机："+e.Message);ScheduleRetry();Buttons();}
    }
    if(!busy&&!link.Connected&&!manualStop&&settings.AutoConnect&&settings.LastId!=""&&now>=nextRetry){
     string ip=settings.LastIp;
     // Snapshot discovered peers on the UI thread; avoid dictionary races.
     window.Dispatcher.Invoke(new Action(()=>{Peer p;if(peers.TryGetValue(settings.LastId,out p)&&now-p.Seen<TimeSpan.FromSeconds(12))ip=p.Ip;}));
     Connect(ip,"",settings.NativeResolution==true?0:settings.Width,settings.NativeResolution==true?0:settings.Height,settings.Fps,settings.BudgetMbps);
    }
   }Thread.Sleep(80);
  }
 }
 string MaintenanceState(){
  string stats=telemetryJson;long age=telemetryTime==0?-1:(DateTime.UtcNow.Ticks-Interlocked.Read(ref telemetryTime))/10000;
  if(client){string path=Path.Combine(Paths.Runtime,"receiver-stats.json");try{stats=File.ReadAllText(path);age=(long)(DateTime.UtcNow-File.GetLastWriteTimeUtc(path)).TotalMilliseconds;}catch{stats="{}";age=-1;}}
  return Json.Write(new{mode=client?"receive":"send",busy=busy,status=lastStatus,streaming=client?server!=null&&server.Streaming:link!=null&&link.Streaming,
   connected=client?server!=null&&server.ActiveHostId!="":link!=null&&link.Connected,backend=settings.Backend,
   senderPid=link==null?0:link.SenderPid,receiverPid=client?(server==null?0:server.ReceiverPid):(link==null?0:link.ReceiverPid),
   width=settings.Width,height=settings.Height,fps=settings.Fps,telemetryAgeMs=age,stats=Json.Read(stats)});
 }
 void Maintenance(string page){
  MaintenanceWindow.Show(window,page,MaintenanceState,Setup);
 }
 void Setup(string option,Action<int,string> completed){
  var button=C<Button>(option=="--setup-firewall"?"Firewall":"Driver");button.IsEnabled=false;
  string result=Path.Combine(Paths.Runtime,"setup-"+Guid.NewGuid().ToString("N")+".json");
  try{SetStatus("正在打开配置，请完成 Windows 权限提示…");var p=Process.Start(new ProcessStartInfo(Process.GetCurrentProcess().MainModule.FileName,option+" --setup-result="+Engine.Q(result)+" --setup-user="+WindowsIdentity.GetCurrent().User.Value){UseShellExecute=true,Verb="runas"});
   new Thread(()=>{int code=1;string message="";try{p.WaitForExit();code=p.ExitCode;message=File.Exists(result)?Json.S(Json.Read(File.ReadAllText(result)),"message"):Language.Tr("配置未完成，请查看诊断。");}catch(Exception e){message=e.Message;}finally{p.Dispose();try{File.Delete(result);}catch{}}
    UI(()=>{button.IsEnabled=true;SetStatus(code==0?"配置完成。":message);completed(code,message);});}){IsBackground=true}.Start();
  }catch(System.ComponentModel.Win32Exception e){button.IsEnabled=true;string message=e.NativeErrorCode==1223?Language.Tr("已取消 Windows 权限请求。"):e.Message;SetStatus(message);completed(1,message);}
  catch(Exception e){button.IsEnabled=true;SetStatus("配置未启动："+e.Message);completed(1,e.Message);}
 }
 static bool Admin(){return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);}
 void SwitchMode(bool receive){
  if(exiting)return;if(receive==client){Show();return;}
  C<Button>("SendMode").IsEnabled=false;C<Button>("ReceiveMode").IsEnabled=false;
  SetStatus(receive?"正在切换到接收模式…":"正在切换到发送模式…");
  Exit(()=>{try{new App(receive);}catch(Exception e){Paths.Log("app",e.ToString());MessageBox.Show(Language.Tr(e.Message),Language.Tr("切换模式失败"));try{new App(client,true);}catch{Application.Current.Shutdown();}}});
 }
 void Exit(Action after=null){
  if(exiting)return;exiting=true;manualStop=true;tray.Visible=false;tray.Dispose();
  if(networkChanged!=null){NetworkChange.NetworkAddressChanged-=networkChanged;networkChanged=null;}
  Queue(()=>{try{if(link!=null)link.Dispose();if(server!=null)server.Dispose();if(discovery!=null)discovery.Dispose();}
   finally{stop=true;if(activation!=null)activation.Dispose();window.Dispatcher.BeginInvoke(new Action(()=>{window.Close();if(after!=null)after();else Application.Current.Shutdown();}));}});
 }
 static bool SavedReceive(){try{return Json.S(Json.Read(File.ReadAllText(Path.Combine(Paths.Root,"app.json"))),"mode","receive")!="send";}catch{return true;}}
 [STAThread] public static int Main(string[] args){
  if(args.Contains("--display-latency-broker"))try{return FullChainLatency.Broker();}catch(Exception e){Paths.Log("display-latency",e.ToString());return 1;}
  if(args.Contains("--setup-display-latency"))try{return FullChainLatency.Install();}catch(Exception e){Paths.Log("display-latency",e.ToString());return 1;}
  if(LocalControl.IsRequest(args))try{return LocalControl.Run(args);}catch(Exception e){Paths.Log("control",e.Message);return 1;}
  try{
   Paths.Init();Language.Load();
   if(args.Contains("--udp-segmented"))segmentationOverride=true;
   if(args.Contains("--udp-iocp"))segmentationOverride=false;
   string backendArg=args.FirstOrDefault(v=>v.StartsWith("--display-backend="));
   if(backendArg!=null){backendOverride=backendArg.Substring(18);if(backendOverride!="vdd"&&backendOverride!="lanscreen")throw new ArgumentException("不支持的虚拟屏驱动。");}
   bool client=args.Contains("--receive")||(!args.Contains("--send")&&SavedReceive());
   string resultArg=args.FirstOrDefault(v=>v.StartsWith("--setup-result="));
   string userArg=args.FirstOrDefault(v=>v.StartsWith("--setup-user="));
   if(args.Contains("--setup-firewall"))return SetupActions.Firewall(resultArg==null?null:resultArg.Substring(15),userArg==null?null:userArg.Substring(13));
   if(args.Contains("--setup-driver"))return SetupActions.Driver(resultArg==null?null:resultArg.Substring(15));
   bool created;using(var mutex=new Mutex(true,@"Local\PyroScreen-"+WindowsIdentity.GetCurrent().User.Value,out created)){
    if(!created){
     try{using(var pipe=new NamedPipeClientStream(".",PipeName(),PipeDirection.Out)){pipe.Connect(2000);string command=args.Contains("--send")?"SEND":args.Contains("--receive")?"RECV":"SHOW";byte[] bytes=System.Text.Encoding.ASCII.GetBytes(command);pipe.Write(bytes,0,4);}return 0;}
     catch(Exception e){Paths.Log("startup",e.ToString());MessageBox.Show(Language.Tr("SideWave 已在运行："+e.Message));return 1;}
    }
    try{
      var app=new Application{ShutdownMode=ShutdownMode.OnExplicitShutdown};
      app.DispatcherUnhandledException+=(s,e)=>{Paths.Log("app",e.Exception.ToString());MessageBox.Show(Language.Tr(e.Exception.Message),"SideWave");e.Handled=true;};
      var ui=new App(client,args.Contains("--no-auto"));if(args.Contains("--tray"))ui.window.Hide();app.Run();Paths.CleanupSession();
    }finally{mutex.ReleaseMutex();}
   }
   return 0;
  }catch(Exception e){Paths.Log("startup",e.ToString());MessageBox.Show(Language.Tr(e.Message),Language.Tr("SideWave 启动失败"));return 1;}
 }
}
}
