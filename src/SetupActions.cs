using System;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Windows;
namespace PyroScreen {
static class SetupActions {
 static int Finish(string result,int code,string message){
  Paths.Log("setup",message);
  if(result!=null)Paths.Atomic(result,Json.Write(new{code=code,message=message}));
  else MessageBox.Show(Language.Tr(message),"PyroScreen");
  return code;
 }
 public static int Firewall(string result=null,string callerSid=null){
  try{
   SystemInspection.Script("Install-Network.ps1",Process.GetCurrentProcess().MainModule.FileName,90);
   string message="已验证本软件的三条专用网络防火墙规则。公共网络仍需在 Windows 设置中改为专用网络。";
   try{if(callerSid!=null&&callerSid!=WindowsIdentity.GetCurrent().User.Value)throw new IOException(Language.Tr("请使用当前登录用户的管理员权限安装显示采样；其他账户不能为当前用户配置采样。"));FullChainLatency.Install();message+="\n"+Language.Tr("显示延迟采样已安装。");}
   catch(Exception e){return Finish(result,2,message+"\n"+Language.Tr("防火墙已配置；显示延迟采样安装失败：")+e.Message);}
   return Finish(result,0,message);
  }catch(Exception e){return Finish(result,1,Language.Tr("网络配置失败")+": "+e.Message);}
 }
 public static int Driver(string result=null){
  try{
   string output=SystemInspection.Script("Install-Driver.ps1",null,120);
   string message="已验证 Windows 驱动库中的虚拟显示驱动。连接副机时创建副屏，断开后释放。";
   if(output.Contains("PYROSCREEN_REBOOT_REQUIRED"))return Finish(result,2,message+"\n"+Language.Tr("Windows 要求重新启动后才能使用驱动。"));
   return Finish(result,0,message);
  }catch(Exception e){return Finish(result,1,Language.Tr("驱动安装失败")+": "+e.Message);}
 }
}
}
