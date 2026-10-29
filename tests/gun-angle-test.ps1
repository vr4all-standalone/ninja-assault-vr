# Exercise the real settings/INI/menu and shared gun/muzzle/laser/UI pose path without XR.
param([string]$Output='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(-not $Output){$Output=Join-Path $repo 'build-win/vrtest/gun-angle'}
$out=[IO.Path]::GetFullPath($Output)
if(-not $out.StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'build-win'))+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Output must stay under build-win'}
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Source([string]$Path){[IO.File]::ReadAllText((Join-Path $repo $Path)).Replace("`r`n","`n")}
function Block([string]$Text,[string]$Signature){
 $start=$Text.IndexOf($Signature);if($start -lt 0){throw "Missing production block: $Signature"}
 $begin=$Text.IndexOf('{',$start);$depth=1;$end=$begin+1
 while($depth -gt 0 -and $end -lt $Text.Length){if($Text[$end] -eq '{'){$depth++};if($Text[$end] -eq '}'){$depth--};$end++}
 if($depth -ne 0){throw "Unbalanced production block: $Signature"}
 $Text.Substring($start,$end-$start)
}
$option=Source 'core/cfg/option.h';$definitions=Source 'core/cfg/option.cpp';$hostSource=Source 'core/rend/vr/xr_host.cpp';$menu=Source 'core/rend/vr/xr_menu.cpp'
$iniHeader=(Source 'core/cfg/ini.h').Replace('#include "oslib/storage.h"','').Replace('#pragma once','')
$ini=Source 'core/cfg/ini.cpp'
$ini=[regex]::Replace($ini,'(?m)^#include[^\n]*\n','')
foreach($signature in @('void IniFile::load(hostfs::File *file','void IniFile::save(hostfs::File *file')){$ini=$ini.Replace((Block $ini $signature),'')}
$stdclass=Source 'core/stdclass.h'
$trim=[regex]::Match((Source 'core/stdclass.cpp'),'(?m)^const std::string defaultWs[^\n]+').Value+"`n"+(Block $stdclass 'static inline std::string trim_ws(')
$cfgHeader='#include "cfg/cfg.h"'
$base=Block $option 'class BaseOption';$settings=Block $option 'class Settings';$optionTemplate=Block $option 'template<typename T, bool PerGameOption = true>'
$cfg='namespace config { static IniFile cfgdb; static bool autoSaving=true; std::string persisted; int writeCount=0; void saveFile(){cfgdb.save(persisted);++writeCount;} void autoSave(){if(autoSaving)saveFile();}'
$cfgSource=Source 'core/cfg/cfg.cpp'
foreach($signature in @('void saveStr(','std::string loadStr(','void saveInt(','int loadInt(','void saveBool(','bool loadBool(','void setTransient(','bool isTransient(','bool hasSection(','void deleteSection(','void deleteEntry(','void setAutoSave(','void saveFloat(','float loadFloat(','std::vector<std::string> getEntries(')){$cfg+="`n"+(Block $cfgSource $signature)}
$cfg+="`n}"
$configCode="namespace config {`n$base;`n$settings;`n$optionTemplate;`n"
$alias=[regex]::Match($option,'(?m)^using GunAngleOption[^\n]+').Value
if($alias){$configCode+=$alias+"`n"}
$declaration=[regex]::Match($definitions,'(?m)^(?:Option<int>|GunAngleOption) VrGunAngle[^\n]+').Value
if(-not $declaration){throw 'Missing actual GunAngle definition'}
$configCode+=$declaration+"`n"+[regex]::Match($definitions,'(?m)^Option<int> VrReload[^\n]+').Value+"`nOption<int> Sentinel(`"Sentinel`", 0);`n}"
$policy=''
if(Test-Path -LiteralPath (Join-Path $repo 'core/rend/vr/gun_angle_settings.h')){$policy+='#include "rend/vr/gun_angle_settings.h"'+"`n"}
if(Test-Path -LiteralPath (Join-Path $repo 'core/rend/vr/gun_angle.h')){$policy+='#include "rend/vr/gun_angle.h"'+"`nnamespace gun_angle = vr::gun_angle;`n"}
$angleMenu=[regex]::Match($menu,'(?m)^constexpr int GunAngleMax[^\n]+').Value+"`n"+(Block $menu 'int gunAngle()')
$choose=$menu.IndexOf('case GunAngle:', $menu.IndexOf('void change('));$end=$menu.IndexOf('case Reload:', $choose)
$angleMenu+="`nbool gunsTurned=false; void chooseAngle(int dir){switch(0){case 0:"+$menu.Substring($choose+'case GunAngle:'.Length,$end-$choose-'case GunAngle:'.Length)+"}}`n"
$update=$hostSource.Substring($hostSource.IndexOf('void updateGun('))
$tiltBegin=$update.IndexOf('const int angle =');$tiltEnd=$update.IndexOf('if (g.muzzleLogged', $tiltBegin)
if($tiltBegin -lt 0 -or $tiltEnd -le $tiltBegin){throw 'Missing actual gun tilt path'}
$pose="struct TestView{glm::mat4 pose;glm::vec3 restMuzzle,restForward,aimPoint;};struct TestGun{TestView view;};struct TestRay{glm::vec3 origin,dir;};struct Probe{glm::mat4 tilt;TestView view;TestRay ray;};`nProbe productionPose(int displayed,const glm::vec3& palm,const glm::vec3& muzzle,const glm::mat4& pose,const glm::quat& rot,const glm::mat4& placement){config::VrGunAngle.set(displayed);TestGun g;TestView& view=g.view;TestRay ray;const TestGun* gun=&g;const glm::mat4 recoilMat(1.f);float dist=3.f,s=1.f;`n"
$pose+=$update.Substring($tiltBegin,$tiltEnd-$tiltBegin)
foreach($pattern in @('const glm::vec3 o =[^;]+;','const glm::vec3 d =[^;]+;','view\.pose =[^;]+;','view\.restMuzzle =[^;]+;','view\.restForward =[^;]+;','view\.aimPoint =[^;]+;')){$statement=[regex]::Match($update,$pattern).Value;if(-not $statement){throw "Missing pose statement $pattern"};$pose+="`n"+$statement}
$menuRay=(Block $hostSource 'void updateMenu(')
foreach($pattern in @('ray\.origin = gun->view\.restMuzzle;','ray\.dir = gun->view\.restForward;')){$pose+="`n"+[regex]::Match($menuRay,$pattern).Value}
$pose+="`nreturn {tilt,view,ray};}"
$underTest=@($trim,$iniHeader,$ini,$cfgHeader,$cfg,$policy,$configCode,$angleMenu,$pose)-join "`n"
[IO.File]::WriteAllText((Join-Path $out 'under_test.inc'),$underTest,[Text.UTF8Encoding]::new($false))
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$cpp=Join-Path $PSScriptRoot 'gun-angle-test.cpp';$exe=Join-Path $out 'gun-angle-test.exe';$log=Join-Path $out 'build.log';$batch=Join-Path $out 'build.cmd'
@"
@echo off
call "$vcvars" >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"$out" /I"$repo/core" /I"$repo/core/deps/glm" /Fo"$out/gun-angle.obj" /Fe"$exe" "$cpp" >"$log" 2>&1
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $env:ComSpec /d /c $batch
if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log;throw 'Gun angle production extraction compilation failed'}
& $exe
exit $LASTEXITCODE
