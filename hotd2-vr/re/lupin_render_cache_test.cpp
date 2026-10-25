/* Standalone regression for the actual xr_hands.cpp gunModel() cache-entry path.
   The runner extracts that function verbatim. Only its surrounding renderer/platform
   dependencies are replaced; Source, GunKind and cache validation are production code.
   All HND1 bytes below are synthetic. No emulator, GL context, or game cache is used. */
#include "hands_rip.h"
#include "xr_hands.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace vr::xr
{
namespace
{
struct Model { HandsModel info {}; };
struct Slot { Model model; bool tried = false, loaded = false, missing = false; };
struct OwnGun { Model model; bool loaded = false; };
Slot slots[4];
std::vector<std::unique_ptr<OwnGun>> ownGuns;
hands::Source source;
std::string dataRoot;
int genericLoads = 0;
void refresh() {}
void refreshOwn() {}
bool selectableGuns() { return true; }
int ownIndex(GunKind) { return -1; }
const hands::Source *gameModelSource() { return &source; }
std::string get_writable_data_path(const char *file) { return dataRoot + "/" + file; }
bool file_exists(const std::string& path) { return std::filesystem::exists(std::filesystem::u8path(path)); }
bool load(Model&, std::string path, const std::string&, bool& missing)
{
    ++genericLoads;
    missing = !file_exists(path);
    // This seam records entry into the legacy parser. Structurally valid flags1 and
    // NaN-header payloads reach that parser in the regression; the guard must stop them.
    return !missing;
}
}
#include "lupin_gun_model_under_test.inc"
}

namespace
{
void word(std::vector<uint8_t>& b, uint32_t n)
{
    for (int k=0;k<4;k++) b.push_back((uint8_t)(n>>(k*8)));
}
void setWord(std::vector<uint8_t>& b, size_t at, uint32_t n)
{
    for (int k=0;k<4;k++) b[at+k]=(uint8_t)(n>>(k*8));
}
std::vector<uint8_t> complete()
{
    std::vector<uint8_t> b { 'H','N','D','1' };
    word(b,2);
    for(int k=0;k<23;k++) word(b,0); // finite zero header and no slide travel
    word(b,3); word(b,24);           // complete gun only, expected polygon count
    word(b,1); word(b,1); word(b,1); word(b,0); word(b,0xffffffff); // white texel
    word(b,4); word(b,1); word(b,0); word(b,3); // four meshes, frame's single triangle
    for(int v=0;v<3;v++) { for(int f=0;f<8;f++) word(b,0); word(b,0xffffffff); }
    word(b,0); word(b,0); word(b,0);
    return b;
}
int passed=0,failed=0;
void check(const char *name, const std::vector<uint8_t>& bytes, vr::hands::Source::Kind kind,
        bool expectedModel, int expectedLoads, bool expectedMissing=false)
{
    using namespace vr::xr;
    for(auto& slot:slots) slot=Slot {};
    genericLoads=0;
    source=vr::hands::Source {};
    source.kind=kind;
    source.guns[1].file=kind==vr::hands::Source::NinjaGun?"gun-na-mach.bin":"gun-lp-pistol.bin";
    source.guns[1].wholePolys=kind==vr::hands::Source::NinjaGun?64:24;
    const auto path=std::filesystem::u8path(dataRoot)/source.guns[1].file;
    if(bytes.empty()) std::filesystem::remove(path);
    else { std::ofstream f(path,std::ios::binary|std::ios::trunc); f.write((const char*)bytes.data(),bytes.size()); }
    const bool loaded=gunModel(GunKind::Pistol)!=nullptr;
    const bool ok=loaded==expectedModel && genericLoads==expectedLoads && slots[2].missing==expectedMissing;
    printf("%s %s: model %d, generic loads %d, missing %d\n",ok?"PASS":"FAIL",name,loaded,genericLoads,slots[2].missing);
    ok?++passed:++failed;
    std::filesystem::remove(path);
}
}
int main()
{
    using namespace vr::hands;
    const auto dir=std::filesystem::current_path()/"lupin-render-cache-files";
    std::filesystem::create_directory(dir);
    vr::xr::dataRoot=dir.u8string();
    const auto good=complete();
    GunTarget target; target.wholePolys=24;
    if(!validateGunCache(good,target)) { puts("Fixture error: synthetic complete cache is invalid"); return 2; }
    check("lupin-complete-reaches-renderer",good,Source::LupinGun,true,1);
    auto damaged=good; setWord(damaged,100,ModelGunOnly);
    check("lupin-incomplete-stays-on-fallback",damaged,Source::LupinGun,false,0);
    check("cm-incomplete-keeps-existing-policy",damaged,Source::CmGuns,true,1);
    damaged=good; setWord(damaged,8,0x7fc00000);
    check("lupin-nan-muzzle-never-reaches-renderer",damaged,Source::LupinGun,false,0);
    damaged=good; damaged.pop_back();
    check("lupin-truncation-never-reaches-renderer",damaged,Source::LupinGun,false,0);
    damaged=good; setWord(damaged,104,23);
    check("lupin-wrong-count-never-reaches-renderer",damaged,Source::LupinGun,false,0);
    check("lupin-missing-remains-not-made",{},Source::LupinGun,false,0,true);
    // Ninja uses the same strict complete-cache boundary, with its own file identity.
    const auto ninja=Source::NinjaGun;
    Source native; native.kind=ninja; native.file="hands.bin";
    native.guns[1].file="gun-na-mach.bin";
    const bool isolated=native.gunModels() && std::string(native.modelFile())=="gun-na-mach.bin";
    printf("%s ninja-native-cache-isolation\n",isolated?"PASS":"FAIL"); isolated?++passed:++failed;
    auto ninjaGood=good; setWord(ninjaGood,104,64);
    check("ninja-complete-reaches-renderer",ninjaGood,ninja,true,1);
    damaged=ninjaGood; setWord(damaged,100,ModelGunOnly);
    check("ninja-incomplete-stays-on-fallback",damaged,ninja,false,0);
    damaged=ninjaGood; setWord(damaged,8,0x7fc00000);
    check("ninja-nan-muzzle-never-reaches-renderer",damaged,ninja,false,0);
    damaged=ninjaGood; damaged.pop_back();
    check("ninja-truncation-never-reaches-renderer",damaged,ninja,false,0);
    damaged=ninjaGood; setWord(damaged,104,63);
    check("ninja-wrong-count-never-reaches-renderer",damaged,ninja,false,0);
    check("ninja-missing-remains-not-made",{},ninja,false,0,true);
    std::filesystem::remove(dir);
    printf("lupin_render_cache_test: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
