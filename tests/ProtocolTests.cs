using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
namespace PyroScreen {
class ProtocolTests {
 public static int Main(string[] args){
  Paths.Init();string result=Path.Combine(Paths.Bin,"protocol-test.json");
  string hostPath=Path.Combine(Paths.Root,"host.json");byte[] savedHost=File.Exists(hostPath)?File.ReadAllBytes(hostPath):null;
  try{
   string code=Encoding.UTF8.GetString(ProtectedData.Unprotect(File.ReadAllBytes(Path.Combine(Paths.Root,"pair-code.dpapi")),null,DataProtectionScope.CurrentUser));
   var config=Settings.Load("test-host");config.LastIp="";config.LastId="";config.LastPin="";config.Peers.Clear();
   using(var h=new HostLink(config)){
    bool wrongRejected=false;try{h.Connect("127.0.0.1","AAAAAAAAAAAAAAAA");}catch{wrongRejected=true;}
    Thread.Sleep(2200);
    h.Connect("127.0.0.1",code);string first=h.ConnectionId;h.Calibrate();var stats=h.Stats();
    if(Json.S(stats,"connection")!=first)throw new Exception("Control session changed during clock/stats.");
    h.Disconnect();Thread.Sleep(200);h.Connect("127.0.0.1");string second=h.ConnectionId;
    if(first==second)throw new Exception("Reconnect reused session ID.");
    h.Disconnect();Thread.Sleep(200);config.Peers.Clear();config.LastPin="";config.LastId="";
    h.Connect("127.0.0.1",code);
    File.WriteAllText(result,Json.Write(new{passed=true,wrongCodeRejected=wrongRejected,paired=true,rememberedReconnect=true,persistentControl=true,forgottenHostRepair=true}));
   }return 0;
  }catch(Exception e){File.WriteAllText(result,Json.Write(new{passed=false,error=e.ToString()}));return 1;}
  finally{if(savedHost!=null)File.WriteAllBytes(hostPath,savedHost);else if(File.Exists(hostPath))File.Delete(hostPath);}
 }
}
}
