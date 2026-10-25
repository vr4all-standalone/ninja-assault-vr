// SPDX-License-Identifier: GPL-2.0-or-later
#include "standalone_game.h"
#include <cstdio>
#include <cstring>
int main() {
 int failures=0;
 auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name); failures+=!ok;};
 check(vr::standaloneGameError("NINJA ASSAULT")==nullptr,"exact Ninja loaded ID accepted");
 const char* rejected[]={"NINJA ASSAULT extra","ninja assault","ninjaslt.zip","", "LUPIN THE THIRD  -THE SHOOTING-"," CONFIDENTIAL MISSION ---------"};
 for(auto id:rejected) check(vr::standaloneGameError(id)!=nullptr,"other loaded media rejected");
 const char* message=vr::standaloneGameError("");
 check(message&&std::strstr(message,"Ninja Assault VR"),"actionable game rejection");
 return failures?1:0;
}
