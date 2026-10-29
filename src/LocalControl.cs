using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Threading;

namespace PyroScreen {
// Explicit local application API. It shares the same current-user pipe ACL as
// INFO and never operates on windows, input devices or accessibility controls.
static class LocalControl {
 public static bool IsRequest(string[] args){foreach(string a in args)if(a=="--apply-current"||a=="--stop-video"||a=="--disconnect"||a=="--quit"||a=="--control"||a=="--status")return true;return false;}
 public static byte[] Read(Stream stream,int length){
  byte[] bytes=new byte[length];int offset=0;
  while(offset<length){var read=stream.BeginRead(bytes,offset,length-offset,null,null);try{
   if(!read.AsyncWaitHandle.WaitOne(5000))throw new IOException("Local control read timed out.");
   int got=stream.EndRead(read);if(got==0)throw new IOException("Local control reply incomplete.");offset+=got;
  }finally{read.AsyncWaitHandle.Close();}}
  return bytes;
 }
 public static void Reply(Stream stream,object value){byte[] bytes=Encoding.UTF8.GetBytes(Json.Write(value));stream.Write(BitConverter.GetBytes(bytes.Length),0,4);stream.Write(bytes,0,bytes.Length);}
 static Dictionary<string,object> Call(Dictionary<string,object> request){
  using(var pipe=new NamedPipeClientStream(".","PyroScreen-"+WindowsIdentity.GetCurrent().User.Value,PipeDirection.InOut,PipeOptions.Asynchronous)){
   pipe.Connect(3000);byte[] tag=Encoding.ASCII.GetBytes(request==null?"INFO":"CTRL");pipe.Write(tag,0,4);
   if(request!=null){byte[] bytes=Encoding.UTF8.GetBytes(Json.Write(request));if(bytes.Length>16384)throw new IOException("Local command is too large.");pipe.Write(BitConverter.GetBytes(bytes.Length),0,4);pipe.Write(bytes,0,bytes.Length);}
   int length=BitConverter.ToInt32(Read(pipe,4),0);if(length<2||length>32768)throw new IOException("Local reply size invalid.");
   return Json.Read(Encoding.UTF8.GetString(Read(pipe,length)));
  }
 }
 public static int Run(string[] args){
  string resultPath="";Dictionary<string,object> request=null;bool status=false;
  for(int i=0;i<args.Length;++i){
   string arg=args[i];
   if(arg=="--result"&&i+1<args.Length)resultPath=args[++i];
   else if(arg=="--status")status=true;
   else if(arg=="--control"&&i+1<args.Length)request=Json.Read(File.ReadAllText(args[++i]));
   else if(arg=="--apply-current")request=new Dictionary<string,object>{{"operation","apply"}};
   else if(arg=="--stop-video")request=new Dictionary<string,object>{{"operation","stop-video"}};
   else if(arg=="--disconnect")request=new Dictionary<string,object>{{"operation","disconnect"}};
   else if(arg=="--quit")request=new Dictionary<string,object>{{"operation","quit"}};
   else throw new IOException("Unknown local control argument.");
  }
  object report;int exit;
  try{
   if(status){if(request!=null)throw new IOException("Use status or a command, not both.");report=Call(null);exit=0;}
   else{
    if(request==null)throw new IOException("No local operation.");
    var ack=Call(request);if(Json.S(ack,"accepted")!="True")throw new IOException(Json.S(ack,"error","Command rejected."));
    string id=Json.S(ack,"id");
    if(Json.S(request,"operation")=="quit"){report=ack;exit=0;}
    else{
     DateTime limit=DateTime.UtcNow.AddSeconds(90);Dictionary<string,object> state=null;
     while(DateTime.UtcNow<limit){Thread.Sleep(100);state=Call(null);if(Json.S(state,"controlId")==id&&(Json.S(state,"controlState")=="ok"||Json.S(state,"controlState")=="error"))break;}
     if(state==null||Json.S(state,"controlId")!=id||Json.S(state,"controlState")!="ok")throw new IOException(state!=null&&Json.S(state,"controlId")==id&&Json.S(state,"controlState")=="error"?Json.S(state,"controlError","Local operation failed."):"Local command timed out.");
     report=new{passed=true,id=id,state=state};exit=0;
    }
   }
  }catch(Exception e){report=new{passed=false,error=e.Message};exit=1;}
  if(resultPath!="")File.WriteAllText(resultPath,Json.Write(report),new UTF8Encoding(false));
  return exit;
 }
}
}
