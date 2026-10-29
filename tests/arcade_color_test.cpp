// Actual production functions are included verbatim by the runner. Only GL/runtime boundaries are fixtures.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <fstream>
#include "glb_mesh.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "variant.inc"
#include "xr_gun.h"
#include "gun_model.h"
#if __has_include("arcade_gun_color.h")
#include "arcade_gun_color.h"
#endif
using u8=uint8_t; using u32=uint32_t; using GLuint=unsigned; using GLint=int; using GLenum=unsigned; using GLsizei=int;
enum { GL_FALSE, GL_TRUE, GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_STATIC_DRAW, GL_FLOAT, GL_UNSIGNED_BYTE,
 GL_TEXTURE0, GL_TEXTURE_2D, GL_RGBA, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR,
 GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT, GL_REPEAT, GL_TEXTURE_WRAP_T,
 GL_TRIANGLES, GL_UNSIGNED_SHORT, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_BLEND, GL_DEPTH_TEST, GL_LESS, GL_DEPTH_BUFFER_BIT,
 VERTEX_POS_ARRAY, VERTEX_NORM_ARRAY, VERTEX_UV_ARRAY, VERTEX_COL_BASE_ARRAY };
static GLuint nextId=10, activeProgram;
static std::map<std::pair<GLuint,std::string>,int> locations;
static std::map<int,glm::vec4> values;
static std::map<int,int> writes;
static std::map<GLuint,std::string> shaderSources;
static int nextLocation=1;
struct Draw { bool baked; glm::vec4 base, overrideValue; glm::vec3 emissive, light; float spec,shine,cutoff; int overrideWrites; };
static std::vector<Draw> draws;
static int loc(const char* name){auto i=locations.find({activeProgram,name});return i==locations.end()?-1:i->second;}
static glm::vec4 val(const char* name){return values[loc(name)];}
GLint glGetUniformLocation(GLuint p,const char* name){auto key=std::make_pair(p,std::string(name));auto i=locations.find(key);if(i!=locations.end())return i->second;return locations[key]=nextLocation++;}
void glUniform3f(int l,float x,float y,float z){if(l>=0){values[l]={x,y,z,0};writes[l]++;}}
void glUniform4f(int l,float x,float y,float z,float w){if(l>=0){values[l]={x,y,z,w};writes[l]++;}}
void glUniform1f(int l,float x){if(l>=0){values[l]={x,0,0,0};writes[l]++;}}
void glUniform1i(int,int){}
void glUniformMatrix4fv(int,int,int,const float*){}
GLuint createVertexArray(){return nextId++;}
void bindVertexArray(GLuint){} void deleteVertexArray(GLuint){}
void glGenBuffers(int,GLuint* p){*p=nextId++;} void glGenTextures(int,GLuint* p){*p=nextId++;}
void glBindBuffer(int,GLuint){} void glBufferData(int,size_t,const void*,int){}
void glVertexAttribPointer(int,int,int,int,size_t,const void*){}
void glEnableVertexAttribArray(int){} void glActiveTexture(int){}
void glTexImage2D(int,int,int,int,int,int,int,int,const void*){} void glGenerateMipmap(int){}
void glDeleteBuffers(int,const GLuint*){} void glClearDepthf(float){} void glClear(int){} void glColorMask(int,int,int,int){}
struct Cache {void UseProgram(GLuint p){activeProgram=p;} void DeleteProgram(GLuint){} void BindTexture(int,GLuint){} void TexParameteri(int,int,int){} void DeleteTextures(int,const GLuint*){} void Disable(int){} void Enable(int){} void DepthMask(int){} void DepthFunc(int){} } glcache;
struct GlVertexArray{static void unbind(){}};
struct OpenGlSource {std::string s; OpenGlSource& addSource(const char* text){s+=text;return *this;} std::string generate(){return s;}};
const char* VertexCompatShader=""; const char* PixelCompatShader="";
GLuint gl_CompileAndLink(const char*,const char* fragment){GLuint p=nextId++;shaderSources[p]=fragment;return p;}
namespace config {bool VrStaffHand=false;}
#define NOTICE_LOG(...) ((void)0)
#define highp
#define mediump
#define lowp
using glm::vec3; using glm::vec4;
struct Discard{};
vec4 textureSample, vtx_color, arcadeBaseColor; float alphaTest; int tex; glm::vec2 vtx_uv;
vec4 texture(int,glm::vec2){return textureSample;}
#define discard throw Discard{}
#include "shader_base.inc"
#undef discard
static glm::vec4 sampleTex(1,1,1,1), sampleVertex(0,0.8f,1,1);
void glDrawArrays(int,int,int){glm::vec4 base;try{base=fragmentBase(sampleTex,sampleVertex,val("arcadeBaseColor"),val("alphaTest").x);}catch(Discard&){return;}draws.push_back({false,base,val("arcadeBaseColor"),{},glm::vec3(val("muzzleColor")),val("specular").x,val("shininess").x,val("alphaTest").x,writes[loc("arcadeBaseColor")]});}
void glDrawElements(int,int,int,const void*){draws.push_back({true,val("baseColor"),{},glm::vec3(val("emissive")),{},val("specular").x,val("shininess").x,0,0});}
namespace nowide {using std::fopen;}
namespace vr::xr {
#include "renderer_types.inc"
static const HandsModel* selected=nullptr;
const HandsModel* handModel(GunKind){return selected;}
void logGlErrors(){} void logGlErrors(const char*){}
#include "renderer_functions.inc"
}
using namespace vr::xr;
static int checks, failures;
void check(bool b,const std::string& message){checks++;if(!b){failures++;std::cerr<<"FAIL "<<message<<"\n";}}
bool near(glm::vec3 a,glm::vec3 b){return glm::length(a-b)<0.00001f;}
bool near(glm::vec4 a,glm::vec4 b){return glm::length(a-b)<0.00001f;}
void prepare(Model& m){m=Model();m.info.muzzle={0,.04f,-.14f};m.info.gunOnly=true;m.textures.push_back({1,1,16,{255,255,255,255}});for(auto& mesh:m.meshes)mesh.push_back({0,0,3});}
OwnGun& add(const char* stem,const char* label){auto p=std::make_unique<OwnGun>();p->file.stem=stem;p->name=label;p->loaded=true;prepare(p->model);ownGuns.push_back(std::move(p));return *ownGuns.back();}
void render(const HandsModel* m,int player,bool mirrored=false){selected=m;GunView g;g.player=player;g.pose=glm::scale(glm::mat4(1),glm::vec3(mirrored?-1.f:1.f,1,1));g.otherHand=true;g.sinceShot=.01f;draws.clear();drawGunModel(glm::mat4(1),{0,0,1},g,true,{.01f,.9f,.01f});}
void expectModel(const HandsModel* m,int p,bool enabled,const char* label,bool mirror=false){
 int before=0;if(program){activeProgram=program;before=writes[loc("arcadeBaseColor")];}
 render(m,p,mirror);check(!draws.empty(),std::string(label)+" submits model");
 for(size_t i=0;i<draws.size();i++){
  const auto& d=draws[i];const bool plastic=enabled&&(i==0||i==3);
  check(!d.baked,std::string(label)+" uses loaded route");
  check(d.overrideWrites>before,std::string(label)+" uploads override separately for this part");before=d.overrideWrites;
  check(d.overrideValue.a==(plastic?1.f:0.f),std::string(label)+" colors only named body/grip; preserves screws/lens");
  glm::vec4 expected=sampleTex*sampleVertex;if(plastic)expected=glm::vec4(p==1?glm::vec3(.04f,.25f,1.f):glm::vec3(.90f,.04f,.04f),expected.a);
  check(near(d.base,expected),std::string(label)+" absolute body/grip and original detail RGB/alpha");
 }
}
int main(int argc,char** argv){
 auto& arcade=add("NAMCO","unrelated label");auto& zapper=add("zapper","Zapper");auto& custom=add("custom","Arcade gun");
 std::string error;arcade.file.path=argv[1];arcade.file.grip={0,0,0};arcade.file.muzzle={1,0,0};arcade.file.length=.25f;
 check(loadOwnGun(arcade.model,arcade.file,error),"actual GLB reader and production model builder accept semantic fixture: "+error);
 check(arcade.model.meshes[MeshFrame].size()==4,"equal RGBA/texture plastic, metal and lens retain separate semantic parts");
 auto unrelatedFile=arcade.file;unrelatedFile.stem="custom";check(loadOwnGun(custom.model,unrelatedFile,error),"custom model with plastic mesh names loads" );
 unrelatedFile.stem="zapper";check(loadOwnGun(zapper.model,unrelatedFile,error),"Zapper with plastic mesh names loads" );
 slots[0].loaded=true;prepare(slots[0].model);
 // Independent physical poses and both hand layouts carry logical identities through real drawGunModel/drawHands.
 for(bool mirror:{false,true})for(int p:{0,1})expectModel(&arcade.model.info,p,true,mirror?"Arcade mirrored":"Arcade right",mirror);
 sampleTex={.002f,.003f,.001f,1};sampleVertex={.003f,.002f,.001f,1};
 for(int p:{0,1})expectModel(&arcade.model.info,p,true,"Arcade dark grip");
 sampleTex={.2f,.4f,.8f,1};sampleVertex={.6f,.3f,.1f,1};
 expectModel(&arcade.model.info,1,true,"P2 before reset");expectModel(&slots[0].model.info,0,false,"native reset");expectModel(&custom.model.info,1,false,"custom label reset");expectModel(&arcade.model.info,0,true,"P1 after reset");expectModel(&zapper.model.info,1,false,"Zapper reset");
 slots[0].model.staff=true;
#if TEST_MOK
 slots[0].model.info.staff=true;
#endif
 expectModel(&slots[0].model.info,1,false,"staff reset");
#if TEST_MOK
 check(draws.size()==2,"MOK staff has no fist/spare open hand when VrStaffHand off");for(auto& d:draws)check(near(d.light,{.12f,1.f,.5f}),"MOK emerald muzzle light preserved");
#endif
#if TEST_SURVIVOR
 survivorSlots[7].loaded=true;prepare(survivorSlots[7].model);expectModel(&survivorSlots[7].model.info,1,false,"Survivor extra bank reset");
#endif
 HandsModel stale;HandsView invalid;invalid.model=&stale;draws.clear();drawHands(glm::mat4(1),{0,0,1},invalid);check(draws.empty(),"unmatched no draw");invalid.model=nullptr;drawHands(glm::mat4(1),{0,0,1},invalid);check(draws.empty(),"null no draw");expectModel(&arcade.model.info,0,true,"valid after stale");
 // A grey material with the plastic mesh names must color; an identical metal/lens material must not.
 // Reuse the exact real asset through the production cgltf decoder, grouping and renderer when supplied.
 if(argc>2&&argv[2][0]){
  auto file=arcade.file;file.path=argv[2];file.grip={-.072f,.018f,0};file.muzzle={.1235f,.0391f,0};file.length=.2475f;
  check(loadOwnGun(arcade.model,file,error),"bundled Namco GLB loads: "+error);
  check(arcade.model.meshes[MeshFrame].size()==4,"bundled Namco has four semantic material groups");
  for(int p:{0,1})expectModel(&arcade.model.info,p,true,"bundled Namco grey screws / original lens / full plastic");
 }
 // Actual fragment entry prefix executes original alpha test and production resolver.
 for(float enabled:{0.f,1.f}){bool discarded=false;try{fragmentBase({.1f,.2f,.3f,.25f},{.4f,.5f,.6f,.4f},{.9f,.04f,.04f,enabled},.5f);}catch(Discard&){discarded=true;}check(discarded,"original cutout discards .10 alpha");auto b=fragmentBase({.1f,.2f,.3f,.25f},{.4f,.5f,.6f,.4f},{.9f,.04f,.04f,enabled},-1);check(std::abs(b.a-.1f)<.00001f,"texture times vertex alpha .10 retained");for(float a:{.5f,.6f}){bool threw=false;try{b=fragmentBase({.1f,.2f,.3f,a},{.4f,.5f,.6f,1},{.04f,.25f,1,enabled},.5f);}catch(Discard&){threw=true;}check(!threw&&std::abs(b.a-a)<.00001f,"strict alpha cutoff preserved");}}
 // Baked body/grip are full player RGB; screws/lens preserve their original materials and emission.
 const float specs[]={.7f,.35f,.8f,1.f},shines[]={60,28,80,140};
 for(int p:{0,1}){render(nullptr,p);check(draws.size()==4,"baked four parts");for(size_t i=0;i<draws.size();i++){const auto& d=draws[i];glm::vec3 expected=i==2?glm::vec3(.16f,.16f,.18f):i==3?glm::vec3(.06f,.01f,.01f):p?glm::vec3(.04f,.25f,1):glm::vec3(.9f,.04f,.04f);check(d.baked&&near(glm::vec3(d.base),expected),std::string("baked semantic part ")+gunmodel::Parts[i].name);check(std::abs(d.spec-specs[i])<.00001f&&std::abs(d.shine-shines[i])<.00001f,"baked original specular/shine");glm::vec3 emission=i==3?glm::vec3(.12f,0,0):glm::vec3(0);check(near(d.emissive,emission),"baked lens original emission hue and peak");}}
 const auto oldProgram=program;termGun();check(program==0&&modelProgram==0,"termGun resets both programs");
 // Every uniform location declared by actual init is reset by actual term.
 check(uMvp==-1&&uTex==-1&&mColor==-1,"existing locations reset");
#if __has_include("arcade_gun_color.h")
 check(uArcadeBaseColor==-1,"Arcade uniform location reset");
#endif
 check(!arcade.loaded,"own model unloaded on context loss");arcade.loaded=true;check(loadOwnGun(arcade.model,arcade.file,error),"semantic model reloads after GL recreation");sampleTex=glm::vec4(1);sampleVertex={0,.8f,1,1};expectModel(&arcade.model.info,1,true,"P2 after GL recreation");check(program!=oldProgram,"program recreated");expectModel(&slots[0].model.info,0,false,"native after recreation");render(nullptr,0);check(draws.size()==4&&near(glm::vec3(draws[1].base),{.9f,.04f,.04f}),"baked reinit full red grip");
 std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
