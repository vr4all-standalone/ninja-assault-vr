#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <cassert>
#include <iterator>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
using XrTime=long long;using u32=unsigned;
constexpr int Left=0,Right=1,OffScreen=-10000,startAction=1,skipAction=2;
namespace config {
struct Bool {bool value=true;operator bool()const{return value;}void set(bool v){value=v;}};
struct Str {std::string value="left";std::string get()const{return value;}void set(const char* v){value=v;}};
Bool VrXrGun, VrDualP1Left,VrSwapP1P2,VrDualWield,GGPOEnable{false};Str VrTwoPlayerP1Hand;
std::string savedKey,savedValue;
void saveBool(const char*,const char* key,bool value){savedKey=key;savedValue=value?"true":"false";}
void saveStr(const char*,const char* key,const char* value){savedKey=key;savedValue=value;}
}
namespace vr { namespace survivor { void neutral(){} bool game(){return false;} bool ready(){return false;} } }
namespace survivor=vr::survivor;
struct Gun {int player=0,hand=Right;bool triggerWas=false,triggerWait=false,pointedDown=false,offArmed=false,reloadButtonWas=false,freshPull=false;XrTime pullAt=0,heldAt=0,reloadAt=0;int shotsDue=0,autosDue=0,clicksDue=0;unsigned shotCount=0;} guns[2]{{0,Right},{1,Left}};
bool menuOpen=false;void updateMenu(XrTime,bool,bool,bool){}
bool p2JoinPending=false;unsigned players=1;bool autoAccept=true;
bool dualWield=false,p2StartWas=false,p2JoinHeld=false,startSwallow=false,skipSwallow=false;
bool triggerReleaseWait[2]{},startHeld[2]{};int startPlayer[2]{1,0},startOwner[2]{-1,-1};
XrTime dualSince=0;struct {bool held=false;}slide;
bool starts[2]{},skips[2]{},known=true,prompt=false;u32 lastP1=0,lastP2=0;int feedback=-1;
bool boolAction(int action,int hand){return action==startAction?starts[hand]:skips[hand];}
bool playerInGame(int player){return player>=0 && player<2 && ((players>>player)&1)!=0;}
bool player2PromptKnown(){return known;}bool player2Prompting(){return prompt;}
void lightgunSet(int player,int,int,u32 buttons){if(player==1)lastP2=buttons;}void noteHolds(const char*){}void haptic(int hand,float,float){feedback=hand;}
#define NOTICE_LOG(...)
std::atomic<bool> newGame{false};bool p1Armed=false;void setHowToPlay(bool,const char*){}
#include "../../core/rend/vr/service_controls.h"
#include "../../core/rend/vr/service_boot.h"
using vr::xr::ServiceStyle;using vr::xr::serviceStyle;
struct {struct {std::string gameId;}content;}settings;
vr::xr::StartReleaseGate startRelease;vr::xr::ServiceControls serviceControls;
std::atomic<uint64_t> serviceGameBootGeneration{0};
std::atomic<bool> startInputReady{true};auto& gameReady=startInputReady;bool emuRunning=true;struct {bool running(){return emuRunning;}} emu;
bool menuCloseRequested=false;void close(){menuCloseRequested=true;}bool serviceRequest=false,serviceNextRequest=false,serviceEntryRequest=false,serviceSelectRequest=false;void lightgunService(bool value,bool next=false,bool select=false){serviceRequest=value||select;serviceEntryRequest=value;serviceNextRequest=next;serviceSelectRequest=select;}
#include "under_test.inc"
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",what);}}
void layout(bool left){config::VrDualP1Left.value=config::VrSwapP1P2.value=left;config::VrTwoPlayerP1Hand.value=left?"left":"right";}
void reset(bool left=true){settings.content.gameId="";serviceControls.reset();startRelease.reset();serviceRequest=serviceNextRequest=false;startSwallow=menuCloseRequested=false;p2JoinPending=false;players=1;autoAccept=true;menuOpen=false;guns[0]=Gun{0,Right};guns[1]=Gun{1,Left};dualWield=false;dualSince=0;p2StartWas=p2JoinHeld=startSwallow=skipSwallow=false;known=true;prompt=false;config::VrDualWield.value=true;config::GGPOEnable.value=false;feedback=-1;lastP1=lastP2=0;for(int h:{Left,Right}){starts[h]=skips[h]=triggerReleaseWait[h]=startHeld[h]=false;startOwner[h]=-1;startPlayer[h]=1-h;}layout(left);}
void frame(XrTime t=1'000'000'000LL,float left=0,float right=0,bool tl=true,bool tr=true){bool tracked[2]{tl,tr};float pulls[2]{left,right};frameCode(t,tracked,pulls,starts[Left],skips[Left]||skips[Right]);if(autoAccept && players==1 && (lastP2&4)){players=3;frameCode(t+1,tracked,pulls,starts[Left],skips[Left]||skips[Right]);}}
int main(){
 gameReady=false;check(!serviceMenuAvailable(),"Service row waits for initial native game loading");gameReady=true;
 emuRunning=false;check(!gameStartReady()&&serviceMenuAvailable(),"paused VR menu can open Service from loaded snapshot");emuRunning=true;
 using vr::xr::nativeMainBoot;
 check(nativeMainBoot(0x0c020800,0x0c01f820,0x0c020800,0x0c020000),"standard BIOS main handoff restores controls");
 check(nativeMainBoot(0x8c021000,0x0c01f820,0x8c021000,0x8c021000),"equal main/test PC still recognizes main selector");
 check(!nativeMainBoot(0x8c021000,0x0c01f824,0x8c021000,0x8c021000),"equal main/test PC rejects Game Test selector");
 check(!nativeMainBoot(0x0c020000,0x0c01f824,0x0c020800,0x0c020000),"Game Test boot preserves navigation");
 check(nativeMainBoot(0xac020800,0x8c01f820,0x0c020800,0x0c020000),"cached and uncached RAM aliases agree");
 check(!nativeMainBoot(0x0c020802,0x0c01f820,0x0c020800,0x0c020000),"only exact native entry is accepted");
 check(!nativeMainBoot(0x0c020800,0x0c01f824,0x0c020800,0x0c020800),"test boot cannot mimic equal main entry");
 check(!nativeMainBoot(0x0c020800,0,0x0c020800,0x0c020000),"ordinary recompiled code is not a BIOS handoff");
 check(!nativeMainBoot(0x0c020800,0x0c01f820,0x0c020800,0),"uninitialized test header is rejected");
 check(!nativeMainBoot(0x0c01f820,0x0c01f820,0x0c01f820,0x0c020000),"BIOS metadata is not executable game entry");
 check(!nativeMainBoot(0x0c020801,0x0c01f820,0x0c020801,0x0c020000),"unaligned header is rejected");
 check(!nativeMainBoot(0x0d020800,0x0c01f820,0x0d020800,0x0c020000),"entry outside NAOMI RAM is rejected");

 for(bool left:{false,true}){
  reset(left);int hp=left?Left:Right;
  check(p1Hand()==Right,"solo P1 Start role is right regardless of dual layout");
  check(menuValue()==(left?"P1 left / P2 right":"P1 right / P2 left"),"menu shows saved two-player layout during solo play");
  toggleMenu();check(p1Hand()==Right,"menu preference never changes solo roles");check(menuValue()==(left?"P1 right / P2 left":"P1 left / P2 right"),"menu toggles dual layout");toggleMenu();
  starts[Right]=true;frame();check(!dualWield&&lastP1==4&&lastP2==0,"solo A starts P1 without joining P2");
  reset(left);starts[Left]=true;frame();check(dualWield&&guns[0].hand==hp&&guns[1].hand==1-hp,"solo X applies saved layout after native acceptance");check(lastP1==0&&lastP2==4,"joining press never starts P1");check(feedback==1-hp,"join feedback goes to assigned P2 hand");
  frame(1'100'000'000LL);check(lastP1==0&&lastP2==4,"held X remains P2 after joining");
  starts[Left]=false;frame(1'200'000'000LL);starts[Left]=true;frame(1'300'000'000LL);check((hp==Left?lastP1:lastP2)==4&&(hp==Left?lastP2:lastP1)==0,"fresh X follows dual layout after release");
  starts[Left]=false;frame(1'400'000'000LL);starts[hp]=true;frame(1'500'000'000LL);layout(!left);frame(1'600'000'000LL);check(lastP1==4&&lastP2==0,"held P1 Start stays P1 through live layout swap");check(guns[0].hand==1-hp&&guns[1].hand==hp,"live swap moves both gun assignments");
  starts[hp]=false;frame(1'700'000'000LL);starts[hp]=true;frame(1'800'000'000LL);check(lastP1==0&&lastP2==4,"released Start adopts new hand assignment");
  reset(left);starts[Right]=true;frame();starts[Left]=true;frame(1'100'000'000LL);check(dualWield&&lastP1==4&&lastP2==4,"simultaneous/overlapping A and X reach both players on join");frame(1'200'000'000LL);check(lastP1==4&&lastP2==4,"held A remains P1 while held joining X remains P2");
  reset(left);starts[Left]=true;frame();starts[Left]=false;frame(1'200'000'000LL);starts[hp]=true;frame(2'000'000'000LL);prompt=true;players=1;frame(6'000'000'000LL);check(!dualWield&&p1Hand()==Right,"P2 leaving restores solo role");check(lastP1==4&&lastP2==0,"held P1 Start does not become another player's Start on leave");
  reset(left);starts[Left]=true;frame();starts[Left]=false;frame(1'200'000'000LL);starts[1-hp]=true;frame(2'000'000'000LL);prompt=true;players=1;frame(6'000'000'000LL);check(!dualWield&&lastP1==0,"held P2 Start cannot become P1 Start on leave");starts[0]=starts[1]=false;prompt=false;frame(6'100'000'000LL);starts[Left]=true;frame(6'200'000'000LL);check(dualWield&&lastP1==0&&lastP2==4,"P2 can rejoin with fresh solo X");
  for(int gate=0;gate<5;++gate){reset(left);starts[Left]=true;if(gate==0)config::VrDualWield.value=false;if(gate==1)config::GGPOEnable.value=true;if(gate==2)known=false;frame(1'000'000'000LL,0,0,gate!=3,gate!=4);check(!dualWield&&lastP1==0&&lastP2==0,"blocked X join cannot start P1");}
  reset(left);dualWield=true;players=3;guns[0].hand=hp;guns[1].hand=1-hp;starts[hp]=true;frame();menuOpen=true;menuFrame(starts[Left],starts[Right]);starts[hp]=false;menuFrame(false,false);layout(!left);starts[hp]=true;menuFrame(starts[Left],starts[Right]);menuOpen=false;frame(2'000'000'000LL);check(lastP1==0&&lastP2==4,"Start released/repressed inside menu uses new player assignment");
  reset(left);dualWield=true;players=3;guns[0].hand=hp;guns[1].hand=1-hp;starts[hp]=true;frame();menuOpen=true;layout(!left);menuFrame(starts[Left],starts[Right]);menuOpen=false;frame(2'000'000'000LL);check(lastP1==4&&lastP2==0,"Start genuinely held through menu swap keeps its original player");
  reset(left);autoAccept=false;players=0;starts[Left]=true;frame();check(!dualWield&&lastP2==0,"loading X cannot spawn or start P2 before P1 is in game");
  players=1;frame(1'100'000'000LL);check(!dualWield&&lastP2==0,"X held from loading must be released before joining");
  starts[Left]=false;frame(1'200'000'000LL);starts[Left]=true;frame(1'300'000'000LL);check(p2JoinPending&&!dualWield&&lastP2==4,"fresh X requests native join without creating a speculative P2 gun");
  frame(1'400'000'000LL);check(!dualWield&&lastP2==4,"ignored Start does not create P2's gun");
  players=3;frame(1'500'000'000LL);check(dualWield&&!p2JoinPending&&guns[0].hand==hp&&lastP2==4,"native P2 acknowledgment applies saved layout and preserves joining Start ownership");
  players=1;frame(1'600'000'000LL);check(!dualWield&&lastP1==0,"native P2 exit removes gun without turning held X into P1 Start");
  reset(left);autoAccept=false;starts[Left]=true;frame();starts[Left]=false;frame(1'100'000'000LL);frame(6'000'000'000LL);check(!dualWield&&!p2JoinPending&&lastP2==0,"unaccepted join expires with P2 buttons released");
  reset(left);autoAccept=false;starts[Left]=true;frame();config::VrDualWield.value=false;frame(1'100'000'000LL);players=3;frame(1'200'000'000LL);check(!dualWield&&!p2JoinPending&&lastP2==0,"disabling local joining cancels an outstanding request");
  reset(left);autoAccept=false;starts[Left]=true;frame();players=3;frame(1'100'000'000LL,0,0,false,true);check(dualWield&&!p2JoinPending,"tracking loss cannot discard native acknowledgment of a sent join");
  reset(left);autoAccept=false;starts[Left]=true;frame();players=2;frame(1'100'000'000LL);check(dualWield&&!p2JoinPending,"P1 exit while a requested join lands does not strand active P2");
  reset(left);autoAccept=false;p2JoinPending=true;lastP2=4;newGame=true;resetForNewGame();check(!dualWield&&!p2JoinPending&&lastP2==0&&!newGame.load(),"game restart clears pending native join and P2 input");
  players=3;starts[Left]=true;frame(2'000'000'000LL);check(!dualWield&&!p2JoinPending,"X held during restart cannot inherit the previous run's join");
  reset(left);autoAccept=false;starts[Left]=true;frame();players=3;frame(6'000'000'000LL);check(dualWield&&!p2JoinPending,"native join acknowledgment survives a host pause longer than request timeout");
  reset(left);frame(1,1,0);check(guns[0].hand==Left&&!dualWield,"solo gun follows left trigger");guns[0].triggerWas=false;frame(2,0,1);check(guns[0].hand==Right&&!dualWield,"solo gun follows right trigger");guns[0].hand=Left;toggleMenu();frame(3);check(guns[0].hand==Left,"solo gun stays in current hand when dual preference changes");
 }

 {
  vr::xr::StartReleaseGate gate;
  check(!gate.sample(Right,true,false),"boot A does not reach native Start");
  check(!gate.sample(Right,true,true),"A held across native readiness requires release");
  gate.sample(Right,false,true);check(gate.sample(Right,true,true),"fresh A after loading remains normal Start");
  check(!gate.sample(Left,true,false)&&!gate.sample(Left,true,true),"boot X also requires release");
  gate.sample(Left,false,true);check(gate.sample(Left,true,true),"fresh X passes after readiness");
  gate.reset();check(!gate.sample(Right,true,true),"reset cannot inherit held Start");
  check(vr::xr::serviceButtons(0x71,true,false,0x40)==0x71,"existing keyboard/gamepad cabinet controls remain available");
  check(vr::xr::serviceButtons(0x01,true,true,0x40,false,0x20)==0x41,"explicit Select adds only Test");
  check(vr::xr::serviceButtons(0x01,true,false,0x40,true,0x20)==0x21,"explicit Next adds only Service");
  check(vr::xr::serviceButtons(0x01,true,true,0x40,true,0x20)==0x61,"Next and Select remain independent native inputs");
  check(vr::xr::serviceButtons(0x71,false,true,0x40,true,0x20)==0x71,"disabled XR injection does not alter cabinet input");
 }
 {
  bool p1=false,p2=false;
  reset();sampleStartButtons(0,false,true,false,true,p1,p2);
  sampleStartButtons(1'000'000'000LL,false,true,true,true,p1,p2);
  check(!p1&&!p2&&!serviceRequest&&!serviceControls.active,"A held from loading neither starts nor opens Service");
  sampleStartButtons(1'100'000'000LL,false,false,true,true,p1,p2);
  sampleStartButtons(2'000'000'000LL,false,true,true,true,p1,p2);
  check(p1&&!p2&&!serviceRequest,"fresh A starts P1 immediately without a Service gesture");
  sampleStartButtons(10'000'000'000LL,false,true,true,true,p1,p2);
  check(p1&&!serviceRequest&&!serviceNextRequest&&!serviceControls.active,"holding A for any duration cannot open Service");
  sampleStartButtons(11'000'000'000LL,false,false,true,true,p1,p2);
  check(!p1&&!p2&&!serviceRequest,"released A releases Start without a late Test pulse");
  reset();config::GGPOEnable.value=true;sampleStartButtons(0,false,false,true,true,p1,p2);
  sampleStartButtons(1,false,true,true,true,p1,p2);
  check(p1&&!serviceRequest&&!serviceControls.active,"online A remains ordinary synchronized Start");
  check(!openServiceMenu()&&!serviceControls.active,"online play cannot request local Service");
  config::GGPOEnable.value=false;config::VrXrGun.value=false;
  check(!openServiceMenu()&&!serviceControls.active,"disabled VR guns cannot request Service");
  config::VrXrGun.value=true;
 }
 {
  bool p1=false,p2=false;
  reset();serviceGameBootGeneration=4;chooseServiceRow();
  check(menuCloseRequested&&serviceControls.active&&serviceControls.pending&&!serviceRequest,"the real Service row queues entry and resumes the paused game without immediate Test");
  sampleStartButtons(0,false,true,false,false,p1,p2);
  check(serviceControls.pending&&!serviceRequest&&!serviceNextRequest&&!p1&&!p2,"paused/menu-held A cannot dispatch Service before unpause");
  sampleStartButtons(1'000'000'000LL,true,true,false,true,p1,p2);
  check(serviceRequest&&!serviceNextRequest&&!serviceControls.pending&&!p1&&!p2,"explicit menu entry sends exactly one Test pulse without either Start");
  sampleStartButtons(1'400'000'000LL,true,true,false,true,p1,p2);
  check(!serviceRequest&&!serviceNextRequest&&!p1&&!p2,"buttons held through menu entry cannot select again or start P2");
  sampleStartButtons(1'500'000'000LL,false,false,false,true,p1,p2);
  sampleStartButtons(2'000'000'000LL,true,false,false,true,p1,p2);
  check(serviceNextRequest&&!serviceRequest&&!p1&&!p2,"Service X navigates native options while game readiness is false");
  sampleStartButtons(2'090'000'000LL,false,false,false,true,p1,p2);
  check(!serviceNextRequest,"rapid Next taps have a native release edge");
  sampleStartButtons(2'120'000'000LL,true,false,false,true,p1,p2);
  check(serviceNextRequest,"a second rapid Next tap stays separate");
  sampleStartButtons(2'300'000'000LL,false,false,false,true,p1,p2);
  sampleStartButtons(3'000'000'000LL,false,true,false,true,p1,p2);
  check(serviceRequest&&!serviceNextRequest&&!p1&&!p2,"short A selects immediately inside Service without game Start");
  sampleStartButtons(3'090'000'000LL,false,false,false,true,p1,p2);
  check(!serviceRequest,"rapid Select taps have a native release edge");
  sampleStartButtons(3'120'000'000LL,false,true,false,true,p1,p2);
  check(serviceRequest,"second rapid Select tap stays separate");
  resetStartGestures();sampleStartButtons(4'000'000'000LL,true,true,false,true,p1,p2);
  check(serviceControls.active&&!serviceRequest&&!serviceNextRequest&&!p1&&!p2,"focus or sync recovery preserves mode but consumes held A/X");
  sampleStartButtons(4'100'000'000LL,false,false,false,true,p1,p2);
  sampleStartButtons(5'000'000'000LL,true,false,false,true,p1,p2);
  check(serviceNextRequest,"fresh native Next works after focus recovery");
  serviceControls.cancel();openServiceMenu();sampleStartButtons(6'000'000'000LL,false,false,false,true,p1,p2);
  check(serviceControls.active&&!serviceRequest&&!serviceNextRequest,"reopening an already active Service row only resumes it without another Test");
  sampleStartButtons(7'000'000'000LL,false,true,true,true,p1,p2);
  check(!p1&&!p2,"returning native readiness alone cannot close Service or start a player");
  notifyServiceGameBoot();sampleStartButtons(8'000'000'000LL,true,true,true,true,p1,p2);
  check(!serviceControls.active&&!serviceRequest&&!serviceNextRequest&&!p1&&!p2,"native main boot automatically clears Service without B and consumes the exit press");
  sampleStartButtons(8'100'000'000LL,false,false,true,true,p1,p2);
  sampleStartButtons(9'000'000'000LL,false,true,true,true,p1,p2);
  check(p1&&!p2&&!serviceRequest,"a fresh A starts P1 normally after native EXIT without a B step");
  serviceControls.request(serviceGameBootGeneration.load());config::GGPOEnable.value=true;
  sampleStartButtons(10'000'000'000LL,false,false,true,true,p1,p2);
  check(!serviceControls.active&&!serviceRequest&&!serviceNextRequest,"online transition clears Service mode and native pulses");
  config::GGPOEnable.value=false;serviceControls.request(serviceGameBootGeneration.load());newGame=true;resetForNewGame();
  check(!serviceControls.active&&!serviceRequest&&!serviceNextRequest,"new game clears Service mode and pending input");
  reset();autoAccept=false;serviceControls.request(serviceGameBootGeneration.load());p2JoinPending=true;starts[Left]=true;frame();
  check(!p2JoinPending&&!dualWield&&lastP2==0,"Service mode cancels speculative P2 requests");
  serviceControls.reset();
 }
 for(const char* id:{"ninjaslt","gunsur2","NINJA ASSAULT"," BIOHAZARD  GUN SURVIVOR2"}) {
  bool p1=false,p2=false;reset();settings.content.gameId=id;
  check(!serviceMenuExitAvailable(),"ordinary Namco gameplay shows only explicit Service entry");
  chooseServiceRow();chooseServiceRow();check(serviceControls.pending&&!serviceControls.exiting,"two Service selects in one frame queue entry once");sampleStartButtons(1'000'000'000LL,false,false,true,true,p1,p2);
  check(serviceEntryRequest&&!serviceSelectRequest&&serviceMenuExitAvailable(),"Namco menu entry uses native Test only");
  sampleStartButtons(1'300'000'000LL,false,false,true,true,p1,p2);
  sampleStartButtons(2'000'000'000LL,true,false,true,true,p1,p2);
  auto bits=vr::xr::serviceButtons(0,true,serviceEntryRequest,1,serviceNextRequest,4,serviceSelectRequest,16);
  check(bits==4&&!p1&&!p2,"Namco X injects only native menu Down");
  sampleStartButtons(2'200'000'000LL,false,false,true,true,p1,p2);
  sampleStartButtons(3'000'000'000LL,false,true,true,true,p1,p2);
  const unsigned enter=serviceStyle(id)==ServiceStyle::Ninja?16:32;
  bits=vr::xr::serviceButtons(0,true,serviceEntryRequest,1,serviceNextRequest,4,serviceSelectRequest,enter);
  check(bits==enter&&!serviceEntryRequest&&!p1&&!p2,"Namco A injects the title's Enter without toggling Test or starting players");
  chooseServiceRow();chooseServiceRow();sampleStartButtons(4'000'000'000LL,true,true,true,false,p1,p2);
  check(serviceControls.pending&&serviceControls.exiting&&!serviceRequest,"Namco Exit Service Menu waits for resume");
  sampleStartButtons(5'000'000'000LL,true,true,true,true,p1,p2);
  check(serviceEntryRequest&&!serviceSelectRequest&&!serviceNextRequest&&!p1&&!p2,"only the explicit Namco Exit row sends the return Test toggle");
  notifyServiceGameBoot();sampleStartButtons(5'100'000'000LL,true,true,true,true,p1,p2);
  check(!serviceControls.active&&!serviceMenuExitAvailable()&&!serviceRequest&&!p1&&!p2,"native Namco return automatically clears mapping and consumes held inputs");
 }
 {
  NativeJvsMapper mapper;mapper.init_mappings();u32 buttons[4]{},native[4]{};
  buttons[0]=vr::xr::serviceButtons(0,true,false,NAOMI_TEST_KEY,false,NAOMI_DOWN_KEY,true,serviceEnterMask(ServiceStyle::Ninja));
  mapper.read_digital_in(buttons,native);
  check(native[0]==NAOMI_BTN0_KEY&&native[1]==0,"actual Ninja JVS descriptor maps Service A to only cabinet Enter");
  buttons[0]=NAOMI_DOWN_KEY;mapper.read_digital_in(buttons,native);
  check(native[0]==NAOMI_DOWN_KEY&&native[1]==0,"actual Ninja JVS descriptor preserves Service X Down");
 }
 std::printf("%s %d production assignment checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
