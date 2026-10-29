using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Automation;
namespace PyroScreenTests {
class GuiActions{
 static void Record(string suffix,string value){try{File.WriteAllText(Path.Combine(Path.GetTempPath(),"pyroscreen-gui-action"+suffix),value);}catch{}}
 [STAThread]public static int Main(string[] args){
  if(args.Length!=3)return 1;
  string stage="find window";
  try{
   Process p=Process.GetProcessById(int.Parse(args[0]));p.Refresh();
   var root=AutomationElement.RootElement.FindFirst(TreeScope.Children,new AndCondition(new PropertyCondition(AutomationElement.ProcessIdProperty,p.Id),new PropertyCondition(AutomationElement.ControlTypeProperty,ControlType.Window)));
   if(root==null)root=AutomationElement.FromHandle(p.MainWindowHandle);
   var controlType=args[2].StartsWith("choose=")?ControlType.ComboBox:args[2]=="invoke"?ControlType.Button:args[2].StartsWith("toggle")?ControlType.CheckBox:ControlType.Edit;
   var identity=new PropertyCondition(args[1].StartsWith("#")?AutomationElement.AutomationIdProperty:AutomationElement.NameProperty,args[1].StartsWith("#")?args[1].Substring(1):args[1]);
   var element=root.FindFirst(TreeScope.Descendants,new AndCondition(identity,new PropertyCondition(AutomationElement.ControlTypeProperty,controlType)));
   if(element==null)throw new Exception("Requested control not found.");
   if(args[2].StartsWith("choose=")){
    stage="expand "+element.Current.ControlType.ProgrammaticName+" "+element.Current.Name;
    ((ExpandCollapsePattern)element.GetCurrentPattern(ExpandCollapsePattern.Pattern)).Expand();
    System.Threading.Thread.Sleep(150);
    var match=new AndCondition(new PropertyCondition(AutomationElement.NameProperty,args[2].Substring(7)),new PropertyCondition(AutomationElement.ControlTypeProperty,ControlType.ListItem));
    var item=element.FindFirst(TreeScope.Descendants,match);
    if(item==null)item=AutomationElement.RootElement.FindFirst(TreeScope.Descendants,new AndCondition(match,new PropertyCondition(AutomationElement.ProcessIdProperty,p.Id)));
    if(item==null)throw new Exception("Dropdown option unavailable.");
    stage="select "+item.Current.ControlType.ProgrammaticName+" "+item.Current.Name+" patterns="+String.Join(",",Array.ConvertAll(item.GetSupportedPatterns(),v=>v.ProgrammaticName));
    ((SelectionItemPattern)item.GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
    ((ExpandCollapsePattern)element.GetCurrentPattern(ExpandCollapsePattern.Pattern)).Collapse();
    System.Threading.Thread.Sleep(100);
    var selected=((SelectionPattern)element.GetCurrentPattern(SelectionPattern.Pattern)).Current.GetSelection();
    if(selected.Length!=1||selected[0].Current.Name!=args[2].Substring(7))throw new Exception("Dropdown selection readback did not match the requested option.");
   }
   else if(args[2]=="invoke")((InvokePattern)element.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
   else if(args[2].StartsWith("toggle")){
    var toggle=(TogglePattern)element.GetCurrentPattern(TogglePattern.Pattern);
    if(args[2]=="toggle")toggle.Toggle();else{
     var target=args[2]=="toggle-off"?ToggleState.Off:ToggleState.On;
     if(toggle.Current.ToggleState!=target)toggle.Toggle();
     if(toggle.Current.ToggleState!=target)throw new Exception("Checkbox state did not match the requested value.");
    }
   }
   else if(args[2]=="select")((SelectionItemPattern)element.GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
   else ((ValuePattern)element.GetCurrentPattern(ValuePattern.Pattern)).SetValue(args[2].StartsWith("setfile=")?File.ReadAllText(args[2].Substring(8)).Trim():args[2]);
   Record(".last",args[1]+" passed");return 0;
  }catch(Exception e){Record(".error",stage+"\n"+e.ToString());return 1;}
 }
}
}
