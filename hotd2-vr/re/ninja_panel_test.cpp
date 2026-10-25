#include "rend/vr/panel_units.h"
#include <cmath>
#include <cstdio>
int main(){int failures=0,checks=0;auto check=[&](bool ok,const char* name){++checks;printf("%s %s\n",ok?"PASS":"FAIL",name);failures+=!ok;};
 for(float d:{15.f,40.f,120.f}){
  const float game=vr::panelDepthInGameUnits(d,.01f),meters=game*.11f*.01f;
  check(std::abs(meters-d*.11f)<.0001f,"panel distance has reference physical size at every menu limit");
  check(meters>.05f,"HUD remains beyond OpenXR near plane");
  const float originZ=-.45f/(.11f*.01f);const float hit=(-game-originZ)/-1.f;
  check(hit>0.f,"forward extended gun intersects HUD in front of its muzzle");
 }
 check(vr::panelDepthInGameUnits(40.f,1.f)==40.f,"reference games retain exact panel distance");
 const float hud=1.f/1024.f,low=.0009f,high=.01f;
 float was=0;bool positive=true,ordered=true,inBand=true;
 for(int n=0;n<=1000;n++){float w=low+(high-low)*n/1000;float snapped=vr::snappedOverlayDepth(w,hud,low,high);positive&=std::isfinite(snapped)&&snapped>0;ordered&=snapped>=was;inBand&=std::abs(snapped-hud)<.002f;was=snapped;}
 check(positive,"all supported Ninja HUD layers retain positive finite depth");check(ordered,"UI layer order stays monotonic");check(inBand,"snapped layers remain inside overlay shader band");
 check(vr::snappedOverlayDepth(1.f,1.f,.75f,1.05f)==1.f,"reference HUD center remains exact");
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
