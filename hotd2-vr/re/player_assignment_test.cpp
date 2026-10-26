#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
using XrTime=long long;using u32=unsigned;
constexpr int Left=0,Right=1,OffScreen=-10000,startAction=1,skipAction=2;
namespace config {
struct Bool {bool value=true;operator bool()const{return value;}void set(bool v){value=v;}};
struct Str {std::string value="left";std::string get()const{return value;}void set(const char* v){value=v;}};
Bool VrDualP1Left,VrSwapP1P2,VrDualWield,GGPOEnable{false};Str VrTwoPlayerP1Hand;
std::string savedKey,savedValue;
void saveBool(const char*,const char* key,bool value){savedKey=key;savedValue=value?"true":"false";}
void saveStr(const char*,const char* key,const char* value){savedKey=key;savedValue=value;}
}
struct Gun {int player=0,hand=Right;bool triggerWas=false,triggerWait=false,pointedDown=false,offArmed=false,reloadButtonWas=false,freshPull=false;XrTime pullAt=0,heldAt=0,reloadAt=0;int shotsDue=0,autosDue=0,clicksDue=0;unsigned shotCount=0;} guns[2]{{0,Right},{1,Left}};
bool menuOpen=false;void updateMenu(XrTime,bool,bool,bool){}
bool dualWield=false,p2StartWas=false,p2JoinHeld=false,startSwallow=false,skipSwallow=false;
bool triggerReleaseWait[2]{},startHeld[2]{};int startPlayer[2]{1,0},startOwner[2]{-1,-1};
XrTime dualSince=0;struct {bool held=false;}slide;
bool starts[2]{},skips[2]{},known=true,prompt=false;u32 lastP1=0,lastP2=0;int feedback=-1;
bool boolAction(int action,int hand){return action==startAction?starts[hand]:skips[hand];}
bool player2PromptKnown(){return known;}bool player2Prompting(){return prompt;}
void lightgunSet(int,int,int,u32){}void noteHolds(const char*){}void haptic(int hand,float,float){feedback=hand;}
#define NOTICE_LOG(...)
#include "under_test.inc"
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",what);}}
void layout(bool left){config::VrDualP1Left.value=config::VrSwapP1P2.value=left;config::VrTwoPlayerP1Hand.value=left?"left":"right";}
void reset(bool left=true){menuOpen=false;guns[0]=Gun{0,Right};guns[1]=Gun{1,Left};dualWield=false;dualSince=0;p2StartWas=p2JoinHeld=startSwallow=skipSwallow=false;known=true;prompt=false;config::VrDualWield.value=true;config::GGPOEnable.value=false;feedback=-1;lastP1=lastP2=0;for(int h:{Left,Right}){starts[h]=skips[h]=triggerReleaseWait[h]=startHeld[h]=false;startOwner[h]=-1;startPlayer[h]=1-h;}layout(left);}
void frame(XrTime t=1'000'000'000LL,float left=0,float right=0,bool tl=true,bool tr=true){bool tracked[2]{tl,tr};float pulls[2]{left,right};frameCode(t,tracked,pulls,starts[Left],skips[Left]||skips[Right]);}
int main(){
 for(bool left:{false,true}){
  reset(left);int hp=left?Left:Right;
  check(p1Hand()==Right,"solo P1 Start role is right regardless of dual layout");
  check(menuValue()==(left?"P1 left / P2 right":"P1 right / P2 left"),"menu shows saved two-player layout during solo play");
  toggleMenu();check(p1Hand()==Right,"menu preference never changes solo roles");check(menuValue()==(left?"P1 right / P2 left":"P1 left / P2 right"),"menu toggles dual layout");toggleMenu();
  starts[Right]=true;frame();check(!dualWield&&lastP1==4&&lastP2==0,"solo A starts P1 without joining P2");
  reset(left);starts[Left]=true;frame();check(dualWield&&guns[0].hand==hp&&guns[1].hand==1-hp,"solo X joins P2 into saved layout immediately");check(lastP1==0&&lastP2==4,"P2-first joining press never starts P1");check(feedback==1-hp,"join feedback goes to assigned P2 hand");
  frame(1'100'000'000LL);check(lastP1==0&&lastP2==4,"held X remains P2 after joining");
  starts[Left]=false;frame(1'200'000'000LL);starts[Left]=true;frame(1'300'000'000LL);check((hp==Left?lastP1:lastP2)==4&&(hp==Left?lastP2:lastP1)==0,"fresh X follows dual layout after release");
  starts[Left]=false;frame(1'400'000'000LL);starts[hp]=true;frame(1'500'000'000LL);layout(!left);frame(1'600'000'000LL);check(lastP1==4&&lastP2==0,"held P1 Start stays P1 through live layout swap");check(guns[0].hand==1-hp&&guns[1].hand==hp,"live swap moves both gun assignments");
  starts[hp]=false;frame(1'700'000'000LL);starts[hp]=true;frame(1'800'000'000LL);check(lastP1==0&&lastP2==4,"released Start adopts new hand assignment");
  reset(left);starts[Right]=true;frame();starts[Left]=true;frame(1'100'000'000LL);check(dualWield&&lastP1==4&&lastP2==4,"simultaneous/overlapping A and X reach both players on join");frame(1'200'000'000LL);check(lastP1==4&&lastP2==4,"held A remains P1 while held joining X remains P2");
  reset(left);starts[Left]=true;frame();starts[Left]=false;frame(1'200'000'000LL);starts[hp]=true;frame(2'000'000'000LL);prompt=true;frame(6'000'000'000LL);check(!dualWield&&p1Hand()==Right,"P2 leaving restores solo role");check(lastP1==4&&lastP2==0,"held P1 Start does not become another player's Start on leave");
  reset(left);starts[Left]=true;frame();starts[Left]=false;frame(1'200'000'000LL);starts[1-hp]=true;frame(2'000'000'000LL);prompt=true;frame(6'000'000'000LL);check(!dualWield&&lastP1==0,"held P2 Start cannot become P1 Start on leave");starts[0]=starts[1]=false;prompt=false;frame(6'100'000'000LL);starts[Left]=true;frame(6'200'000'000LL);check(dualWield&&lastP1==0&&lastP2==4,"P2 can rejoin with fresh solo X");
  for(int gate=0;gate<5;++gate){reset(left);starts[Left]=true;if(gate==0)config::VrDualWield.value=false;if(gate==1)config::GGPOEnable.value=true;if(gate==2)known=false;frame(1'000'000'000LL,0,0,gate!=3,gate!=4);check(!dualWield&&lastP1==0&&lastP2==0,"blocked X join cannot start P1");}
  reset(left);dualWield=true;guns[0].hand=hp;guns[1].hand=1-hp;starts[hp]=true;frame();menuOpen=true;menuFrame(starts[Left],starts[Right]);starts[hp]=false;menuFrame(false,false);layout(!left);starts[hp]=true;menuFrame(starts[Left],starts[Right]);menuOpen=false;frame(2'000'000'000LL);check(lastP1==0&&lastP2==4,"Start released/repressed inside menu uses new player assignment");
  reset(left);dualWield=true;guns[0].hand=hp;guns[1].hand=1-hp;starts[hp]=true;frame();menuOpen=true;layout(!left);menuFrame(starts[Left],starts[Right]);menuOpen=false;frame(2'000'000'000LL);check(lastP1==4&&lastP2==0,"Start genuinely held through menu swap keeps its original player");
  reset(left);frame(1,1,0);check(guns[0].hand==Left&&!dualWield,"solo gun follows left trigger");guns[0].triggerWas=false;frame(2,0,1);check(guns[0].hand==Right&&!dualWield,"solo gun follows right trigger");guns[0].hand=Left;toggleMenu();frame(3);check(guns[0].hand==Left,"solo gun stays in current hand when dual preference changes");
 }
 std::printf("%s %d production assignment checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
