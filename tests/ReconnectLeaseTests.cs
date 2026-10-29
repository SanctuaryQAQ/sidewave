using System;
using System.IO;
using System.Net.Sockets;
namespace PyroScreen {
class ReconnectLeaseTests {
 static string stage="initial",deviceId="";
 static Wire Connect(Settings settings,out string connection){
  var tcp=new TcpClient();tcp.Connect(settings.LastIp,55166);var wire=new Wire(tcp,false,null,settings.LastPin);
  try{
   string hostNonce=Convert.ToBase64String(Secrets.Random(32));wire.Send(new{type="hello",protocol=1,id=settings.DeviceId,nonce=hostNonce,pair=false});
   var challenge=wire.Read();if(Json.S(challenge,"type")!="challenge"||Json.S(challenge,"paired")!="True"||Json.S(challenge,"id")!=settings.LastId)throw new IOException("Expected saved, already paired receiver.");
   var key=settings.Token(settings.LastId);if(key==null)throw new IOException("Saved device token unavailable.");
   string transcript="PyroScreen|1|"+settings.DeviceId+"|"+settings.LastId+"|"+hostNonce+"|"+Json.S(challenge,"nonce")+"|"+wire.Pin;
   if(!Secrets.Equal(Json.S(challenge,"proof"),Secrets.Mac(key,"server|"+transcript)))throw new IOException("Receiver proof mismatch.");
   wire.Send(new{type="proof",proof=Secrets.Mac(key,"host|"+transcript)});var ready=wire.Read();if(Json.S(ready,"type")!="ready")throw new IOException(Json.S(ready,"message","Lease rejected."));
   connection=Json.S(ready,"connection");wire.SetReceiveTimeout(3000);return wire;
  }catch{wire.Dispose();throw;}
 }
 public static int Main(string[] args){
  Paths.Init();try{
   var settings=Settings.Load("host");deviceId=settings.DeviceId;string a,b;stage="first";
   using(var first=Connect(settings,out a)){
   stage="second";using(var second=Connect(settings,out b)){
    if(a==b)throw new IOException("Lease identity was reused.");
    bool closed=false;try{first.Call(new{type="clock",t1=Native.Now()});}catch{closed=true;}if(!closed)throw new IOException("Old owner socket still usable.");
    if(Json.S(second.Call(new{type="clock",t1=Native.Now()}),"type")!="clock")throw new IOException("New owner cannot use control.");
    File.WriteAllText(args[0],Json.Write(new{passed=true,savedCredentialUsed=true,oldAuthenticatedLeaseClosed=true,newLeaseUsable=true,newConnectionId=true}));
   }}return 0;
  }catch(Exception e){File.WriteAllText(args[0],Json.Write(new{passed=false,error=e.Message,stage=stage,deviceId=deviceId}));return 1;}
 }
}
}
