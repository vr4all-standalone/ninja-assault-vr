#include <algorithm>
#include <atomic>
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
namespace vr { namespace survivor { void neutral(){} bool game(){return false;} } }
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
#include "../../core/rend/vr/service_hold.h"
vr::xr::StartReleaseGate startRelease;vr::xr::ServiceHold serviceHold;bool serviceRequest=false;void lightgunService(bool value){serviceRequest=value;}
#include "under_test.inc"
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",what);}}
void layout(bool left){config::VrDualP1Left.value=config::VrSwapP1P2.value=left;config::VrTwoPlayerP1Hand.value=left?"left":"right";}
void reset(bool left=true){p2JoinPending=false;players=1;autoAccept=true;menuOpen=false;guns[0]=Gun{0,Right};guns[1]=Gun{1,Left};dualWield=false;dualSince=0;p2StartWas=p2JoinHeld=startSwallow=skipSwallow=false;known=true;prompt=false;config::VrDualWield.value=true;config::GGPOEnable.value=false;feedback=-1;lastP1=lastP2=0;for(int h:{Left,Right}){starts[h]=skips[h]=triggerReleaseWait[h]=startHeld[h]=false;startOwner[h]=-1;startPlayer[h]=1-h;}layout(left);}
void frame(XrTime t=1'000'000'000LL,float left=0,float right=0,bool tl=true,bool tr=true){bool tracked[2]{tl,tr};float pulls[2]{left,right};frameCode(t,tracked,pulls,starts[Left],skips[Left]||skips[Right]);if(autoAccept && players==1 && (lastP2&4)){players=3;frameCode(t+1,tracked,pulls,starts[Left],skips[Left]||skips[Right]);}}
int main(){
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
  check(!gate.sample(Right,true,true),"A held across native readiness needs release");
  gate.sample(Right,false,true);check(gate.sample(Right,true,true),"fresh A after ready remains normal Start");
  check(!gate.sample(Left,true,false)&&!gate.sample(Left,true,true),"loading X also requires release at readiness");
  gate.sample(Left,false,true);check(gate.sample(Left,true,true),"fresh X passes after readiness");
  gate.reset();check(!gate.sample(Right,true,true),"reset never inherits a held Start");
  vr::xr::ServiceHold hold;
  check(!hold.update(0,true,true),"A cannot open Service immediately");
  check(!hold.update(1'500'000'000LL,true,true)&&hold.visible()&&hold.progress()>.49f&&hold.progress()<.51f,"held A shows halfway progress");
  check(!hold.update(2'999'999'999LL,true,true),"Service remains closed before three seconds");
  check(hold.update(3'000'000'000LL,true,true),"three-second A hold sends explicit Test pulse");
  check(!hold.update(3'300'000'000LL,true,true),"Test pulse releases while A remains held");
  check(!hold.update(9'000'000'000LL,true,true),"held A cannot reopen Service repeatedly");
  hold.update(9'100'000'000LL,false,true);hold.update(10'000'000'000LL,true,true);
  check(hold.update(13'000'000'000LL,true,true),"release permits a new deliberate hold");
  hold.reset();hold.update(0,true,true);hold.update(2'000'000'000LL,false,true);check(!hold.visible()&&!hold.update(4'000'000'000LL,false,true),"early release cancels Service without delayed opening");
  hold.update(5'000'000'000LL,true,true);check(!hold.update(9'000'000'000LL,true,false)&&!hold.visible(),"focus loss or settings/preparation cancels hold and pulse");
  check(vr::xr::serviceButtons(0x71,true,false,0x40)==0x71,"unrelated cabinet Service/Test stays available");
  check(vr::xr::serviceButtons(0x11,true,true,0x40)==0x51,"only deliberate hold adds native Test");
  check(vr::xr::serviceButtons(0x71,false,false,0x40)==0x71,"ordinary non-VR cabinet input stays unchanged");
  vr::xr::ServiceHold sparse; sparse.update(0,true,true);
  check(sparse.update(3'100'000'000LL,false,true)&&sparse.startPlayer()<0,"release after threshold cannot become a short Start when a frame was skipped");
 }

 {
  bool p1=false,p2=false;
  reset();startRelease.reset();serviceHold.reset();
  sampleStartButtons(0,false,true,false,true,true,p1,p2);
  sampleStartButtons(1'000'000'000LL,false,true,true,true,true,p1,p2);
  sampleStartButtons(1'100'000'000LL,false,false,true,true,true,p1,p2);
  check(!p1&&!p2&&!serviceRequest,"raw boot-held A cannot become a delayed Start tap at readiness");
  sampleStartButtons(2'000'000'000LL,false,true,true,true,true,p1,p2);
  check(!p1&&!p2,"short A waits for release before Start");
  sampleStartButtons(2'100'000'000LL,false,false,true,true,true,p1,p2);
  check(p1&&!p2&&!serviceRequest,"fresh released A starts P1 at the loaded title screen");
  sampleStartButtons(2'300'000'000LL,false,false,true,true,true,p1,p2);
  check(!p1&&!p2,"short Start pulse releases after bounded delivery");
  sampleStartButtons(3'000'000'000LL,false,true,true,true,true,p1,p2);
  dualWield=true;players=3;layout(true);
  sampleStartButtons(3'100'000'000LL,false,false,true,true,true,p1,p2);
  check(p1&&!p2,"short A keeps its original player through a dual-layout change");
  reset();startRelease.reset();serviceHold.reset();sampleStartButtons(0,false,false,true,true,true,p1,p2);
  sampleStartButtons(1,false,true,true,true,true,p1,p2);
  sampleStartButtons(3'000'000'001LL,false,true,true,true,true,p1,p2);
  check(serviceRequest&&!p1&&!p2,"successful A service hold sends Test without normal Start");
  sampleStartButtons(3'010'000'001LL,false,false,true,true,true,p1,p2);
  check(!p1&&!p2,"release after service hold cannot emit a late Start");
  sampleStartButtons(3'300'000'001LL,false,false,true,true,true,p1,p2);
  check(!serviceRequest,"native Test pulse releases after its delivery interval");
  sampleStartButtons(4'000'000'000LL,false,true,true,true,true,p1,p2);
  sampleStartButtons(6'000'000'000LL,false,true,true,true,false,p1,p2);
  sampleStartButtons(8'000'000'000LL,false,true,true,true,true,p1,p2);
  check(!serviceRequest&&!p1&&!p2&&!serviceHold.visible(),"focus/menu cancellation requires A release before another gesture");
  sampleStartButtons(9'000'000'000LL,true,false,true,true,false,p1,p2);
  sampleStartButtons(10'000'000'000LL,true,false,true,true,true,p1,p2);
  check(!p1&&!p2,"X held while input is disabled cannot leak on resume");
  reset();config::GGPOEnable.value=true;startRelease.reset();serviceHold.reset();sampleStartButtons(0,false,false,true,true,true,p1,p2);
  sampleStartButtons(1,false,true,true,true,true,p1,p2);
  check(p1&&!p2&&!serviceRequest,"online A keeps ordinary synchronized Start instead of local service gesture");
  sampleStartButtons(4'000'000'001LL,false,true,true,true,true,p1,p2);
  check(!serviceRequest&&!serviceHold.visible(),"local service hold stays disabled online");
 }
 std::printf("%s %d production assignment checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
