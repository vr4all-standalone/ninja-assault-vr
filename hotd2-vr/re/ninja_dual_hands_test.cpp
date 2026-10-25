#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
namespace config {
struct BoolSetting { bool value=true; operator bool() const{return value;} void set(bool v){value=v;} };
struct StringSetting { std::string value; std::string get() const{return value;} void set(const char* v){value=v;} };
BoolSetting VrDualP1Left, VrDualWield, GGPOEnable{false};
StringSetting VrP1Hand;
std::string savedKey; bool savedValue=false;
void saveBool(const char*,const char* key,bool value){savedKey=key;savedValue=value;}
void saveStr(const char*,const char* key,const char*){savedKey=key;}
}
constexpr int Left=0,Right=1,OffScreen=-10000,startAction=1,skipAction=2;
using XrTime=long long;using u32=unsigned;
struct Gun {
 int player=0,hand=Right;
 bool triggerWas=false,triggerWait=false,pointedDown=false,offArmed=false,reloadButtonWas=false;
 long long pullAt=0,reloadAt=0,heldAt=0;int shotsDue=0,autosDue=0,clicksDue=0;
} guns[2]{{0,Right},{1,Left}};
bool dualWield=false,p2StartWas=false,p2JoinHeld=false,startSwallow=false,skipSwallow=false;
XrTime dualSince=0;
struct {bool held=false;} slide;
bool starts[2]{},skips[2]{},promptKnown=true,prompting=false;
u32 lastP1=0,lastP2=0;
int hapticHand=-1;
bool boolAction(int action,int hand){return action==startAction?starts[hand]:skips[hand];}
bool player2PromptKnown(){return promptKnown;}
bool player2Prompting(){return prompting;}
void lightgunSet(int,int,int,u32){}
void noteHolds(const char*){}
void haptic(int hand,float,float){hapticHand=hand;}
#define NOTICE_LOG(...)
namespace controllers {enum Button{A,B,X,Y,Menu,Stick,Trigger,Grip,Buttons};}
using controllers::Button;
#include "dual_hands_under_test.inc"
int checks=0,failures=0;
void check(bool okay,const char* message){++checks;if(!okay){++failures;std::fprintf(stderr,"FAIL %s\n",message);}}
void reset(){
 guns[0]=Gun{0,Right};guns[1]=Gun{1,Left};dualWield=false;dualSince=0;
 starts[0]=starts[1]=skips[0]=skips[1]=false;
 p2StartWas=p2JoinHeld=startSwallow=skipSwallow=false;promptKnown=true;prompting=false;
 config::VrDualP1Left.value=true;config::VrDualWield.value=true;config::GGPOEnable.value=false;
 config::VrP1Hand.value="left";hapticHand=-1;lastP1=lastP2=0;
}
void frame(long long t=1'000'000'000LL,float left=0,float right=0,bool trackLeft=true,bool trackRight=true){
 const bool tracked[2]{trackLeft,trackRight};const float pulls[2]{left,right};
 updateHands(t,tracked,pulls,starts[Left],skips[Left]||skips[Right]);
}
const char* label(const Words& words,int hand,Button button){for(const Label& l:words.hand[hand])if(l.button==button)return l.text;return "";}
int main(){
 check(defaultEnabled,"dual P1-left preference is enabled by default in persisted config");
 check(!oldSettingUsed,"global P1 hand preference no longer controls single play");
 check(menuNamed,"menu names the dual-only preference Two players");
 for(bool preference:{false,true}){
  reset();config::VrDualP1Left.value=preference;
  check(p1Hand()==Right,"single startup role stays P1 right for either preference");
  frame(1,1,0);check(guns[0].hand==Left&&!dualWield,"single gun transfers to left trigger");
  guns[0].triggerWas=false;frame(2,0,1);check(guns[0].hand==Right&&!dualWield,"single gun transfers to right trigger");
  guns[0].hand=Left;config::VrDualP1Left.value=!preference;frame(3);
  check(guns[0].hand==Left,"changing dual preference alone leaves single gun where it is");
  starts[Right]=true;frame(4);check(lastP1==4&&lastP2==0&&!dualWield,"single A remains P1 Start");
  starts[Right]=false;skips[Left]=true;frame(5);check(lastP1==128&&lastP2==0,"single Y reloads P1");
  skips[Left]=false;skips[Right]=true;frame(6);check(lastP1==128&&lastP2==0,"single B reloads P1");
 }
 for(bool preference:{false,true}){
  reset();config::VrDualP1Left.value=preference;starts[Left]=true;frame();
  const int hand1=preference?Left:Right,hand2=1-hand1;
  check(dualWield&&guns[0].hand==hand1&&guns[1].hand==hand2,"P2 join assigns preferred physical hands immediately");
  check(lastP2&4,"single X joining press reaches P2 even when P2 moves right");
  check(hapticHand==hand2,"join feedback goes to newly assigned P2 hand");
  check(guns[0].triggerWait&&guns[1].triggerWait,"join waits for both reassigned triggers to release");
  frame(1'100'000'000LL);
  check(lastP1==0&&lastP2==4,"joining X stays P2 Start until release and never becomes P1 Start");
  starts[Left]=starts[Right]=false;frame(2'000'000'000LL);
  starts[hand1]=true;skips[hand1]=true;frame(3'000'000'000LL);
  check(lastP1==(4|128)&&lastP2==0,"dual P1 buttons follow actual P1 hand");
  starts[hand1]=skips[hand1]=false;starts[hand2]=skips[hand2]=true;frame(3'100'000'000LL);
  check(lastP1==0&&lastP2==(4|128),"dual P2 buttons follow actual P2 hand");
 }
 for(bool preference:{false,true})for(int oldHand:{Left,Right}){
  reset();config::VrDualP1Left.value=preference;guns[0].hand=oldHand;
  guns[0].pointedDown=guns[0].reloadButtonWas=true;starts[Left]=true;frame();
  for(const Gun& g:guns){
   check(g.pointedDown==(g.hand==oldHand),"joining preserves the known physical down-reload latch");
   check(g.reloadButtonWas==(g.hand==oldHand),"joining preserves the known physical reload-button latch");
  }
 }
 for(int gate=0;gate<5;++gate){
  reset();starts[Left]=true;
  if(gate==0)config::VrDualWield.value=false;
  if(gate==1)config::GGPOEnable.value=true;
  if(gate==2)promptKnown=false;
  frame(1'000'000'000LL,0,0,gate!=3,gate!=4);
  check(!dualWield&&lastP2==0,"dual join respects enabled/netplay/profile/tracking gates");
 }
 reset();dualWield=true;dualSince=1;guns[0].hand=Left;guns[1].hand=Right;
 prompting=true;starts[Right]=true;frame(5'000'000'000LL);
 check(!dualWield&&p1Hand()==Right&&lastP1==4&&lastP2==0,"P2 leave restores fixed single A/X controls in the same frame");
 check(guns[0].hand==Left,"P2 leave keeps P1's current gun hand");
 starts[Right]=false;prompting=false;frame(5'100'000'000LL,0,1);
 check(guns[0].hand==Right,"either-trigger single transfer resumes after P2 leaves");
 for(bool preference:{false,true})for(int downMask=0;downMask<4;++downMask){
  reset();dualWield=true;dualSince=1;config::VrDualP1Left.value=preference;
  const int wanted=preference?Left:Right;
  guns[0].hand=1-wanted;guns[1].hand=wanted;
  bool physicalDown[2]{bool(downMask&1),bool(downMask&2)};
  for(Gun& g:guns){g.pointedDown=physicalDown[g.hand];g.reloadButtonWas=g.hand==Left;g.pullAt=g.reloadAt=g.heldAt=123;g.offArmed=true;g.shotsDue=g.autosDue=g.clicksDue=1;}
  frame(2'000'000'000LL,1,1);
  check(guns[0].hand==wanted&&guns[1].hand==1-wanted,"live dual preference flip assigns both players");
  for(const Gun& g:guns){
   check(g.pointedDown==physicalDown[g.hand],"down-reload latch follows physical hand");
   check(g.reloadButtonWas==(g.hand==Left),"held reload button latch follows physical hand");
   check(g.triggerWas&&g.triggerWait,"live flip requires trigger release");
   check(!g.pullAt&&!g.reloadAt&&!g.heldAt&&!g.offArmed&&!g.shotsDue&&!g.autosDue&&!g.clicksDue,"live flip clears pending input and feedback");
  }
  for(Gun& g:guns){
   check(releaseGate(g,triggerHeld(g,1.f),3)==0&&g.triggerWait,"held trigger cannot shoot or reload after a flip");
   check(releaseGate(g,triggerHeld(g,.5f),3)==0&&g.triggerWait,"half release cannot bypass flip release gate");
   check(releaseGate(g,triggerHeld(g,.3f),0)==0&&!g.triggerWait,"full trigger release rearms after flip");
   g.triggerWas=false;check(releaseGate(g,triggerHeld(g,1.f),1)==1,"fresh pull can shoot after full release");
  }
 }
 reset();check(handsMenuValue()=="P1 left / P2 right","menu displays default dual layout even in single mode");
 toggleHandsMenu();check(!config::VrDualP1Left.value&&config::savedKey=="vr.DualP1Left"&&!config::savedValue,"menu toggles and saves boolean dual preference");
 check(handsMenuValue()=="P1 right / P2 left","menu displays reverse layout");
 toggleHandsMenu();check(config::VrDualP1Left.value&&config::savedValue,"menu restores enabled dual preference");
 const char* profiles[]{"/interaction_profiles/oculus/touch_controller","/interaction_profiles/valve/index_controller","/interaction_profiles/htc/vive_controller","/interaction_profiles/microsoft/motion_controller","unknown",nullptr};
 for(const char* profile:profiles){
  reset();const Words& single=wordsFor(profile);
  check(std::strstr(label(single,Left,controllers::Trigger),"P2")==nullptr&&std::strstr(label(single,Left,controllers::Trigger),"Shoot"),"single left trigger label describes P1 shot/transfer");
  check(std::strstr(label(single,Right,controllers::Trigger),"Shoot")!=nullptr,"single right trigger label describes shot/transfer");
  check(std::strstr(label(single,Left,controllers::Y),"P2")==nullptr,"single left reload label matches P1");
  check(std::strstr(label(single,Right,controllers::A),"P1")&&std::strstr(label(single,Left,controllers::X),"P2"),"single startup labels preserve P1 Start and P2 Join");
  dualWield=true;
  for(bool preference:{false,true}){
   config::VrDualP1Left.value=preference;const Words& dual=wordsFor(profile);const int hand1=preference?Left:Right;
   check(std::strstr(label(dual,hand1,controllers::Trigger),"P1")&&std::strstr(label(dual,1-hand1,controllers::Trigger),"P2"),"dual trigger labels follow assigned players");
   check(std::strstr(label(dual,hand1,hand1==Left?controllers::X:controllers::A),"P1")&&std::strstr(label(dual,1-hand1,hand1==Left?controllers::A:controllers::X),"P2"),"dual start labels follow assigned players");
  }
 }
 std::printf("%s %d production dual-hand checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
