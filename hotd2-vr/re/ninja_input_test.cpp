#include "rend/vr/gun_input.h"
#include <cstdio>
int main() {
 int failed=0, checks=0;
 auto check=[&](bool ok,const char* name){++checks;printf("%s %s\n",ok?"PASS":"FAIL",name);failed+=!ok;};
 vr::GunInput p1,p2; int x=5000,y=5000; uint32_t b=4;
 vr::adaptNinjaInput(p1,false,x,y,b);check(b==4&&x==5000,"other game input stays unchanged");
 vr::adaptNinjaInput(p1,true,x,y,b);check(b==4,"Ninja Start stays on its native JYU path");
 b=128|4;vr::adaptNinjaInput(p1,true,x,y,b);check(b==6&&x==-10000&&y==-10000,"manual reload sends JYU sentinel while preserving Start");
 b=0;vr::adaptNinjaInput(p1,true,x,y,b);
 x=3000;y=4000;b=1;vr::adaptNinjaInput(p1,true,x,y,b);check(b==1&&x==3000&&y==4000,"a normal on-screen pull reaches the game unchanged");
 x=y=-10000;b=2;vr::adaptNinjaInput(p1,true,x,y,b);check(b==1&&x==3000&&y==4000,"held on-screen shot swept off-screen never becomes a reload");
 x=y=-20000;b=2|8;vr::adaptNinjaInput(p1,true,x,y,b);check(b==9&&x==3000&&y==4000,"held shot preserves the last valid aim and unrelated D-pad bits");
 b=0;x=y=-10000;vr::adaptNinjaInput(p1,true,x,y,b);check(b==0&&x==-10000&&y==-10000,"idle off-screen coordinates remain native after release");
 b=2;vr::adaptNinjaInput(p1,true,x,y,b);check(b==2&&x==-10000,"a fresh off-screen pull starts a reload");
 x=6000;y=7000;b=1|256;vr::adaptNinjaInput(p1,true,x,y,b);check(b==258&&x==-10000&&y==-10000,"held reload swept on-screen cannot become a shot or erase plane metadata");
 b=0;x=6000;y=7000;vr::adaptNinjaInput(p1,true,x,y,b);
 b=1;vr::adaptNinjaInput(p1,true,x,y,b);check(b==1&&x==6000&&y==7000,"release resets the pull category before a new on-screen shot");
 x=2500;y=3500;b=1;vr::adaptNinjaInput(p2,true,x,y,b);
 x=y=-10000;b=2;vr::adaptNinjaInput(p2,true,x,y,b);check(b==1&&x==2500&&y==3500,"each player's held aim is independent");
 x=6000;y=7000;b=128|1;vr::adaptNinjaInput(p1,true,x,y,b);check(b==2&&x==-10000&&y==-10000,"a fresh manual B can replace a held shot with a reload request");
 b=0;x=2000;y=3000;vr::adaptNinjaInput(p1,true,x,y,b);
 b=0;x=y=-10000;vr::adaptNinjaInput(p1,true,x,y,b);check(b==0&&x==-10000&&y==-10000,"merely pointing away never fabricates a trigger");
 b=1;vr::adaptNinjaInput(p1,true,x,y,b);check(b==2&&x==-10000,"off-screen trigger uses explicit native reload even without caller bit 2");
 b=0;x=10000;y=5000;vr::adaptNinjaInput(p1,true,x,y,b);
 b=1;vr::adaptNinjaInput(p1,true,x,y,b);check(b==2&&x==-10000,"the 640 pixel endpoint is outside the normalized aim range");
 std::printf("%d checks, %d failures\n",checks,failed);return failed?1:0;
}
