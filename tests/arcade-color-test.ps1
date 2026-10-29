# Compile the actual renderer draw/init/reset functions with only external GL/runtime boundaries recorded.
param([string]$NamcoGlb="")
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=[IO.Path]::GetFullPath((Join-Path $repo 'build-win/vrtest/arcade-color'))
if(-not $out.StartsWith($repo+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Output escaped source tree'}
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Read([string]$rel){[IO.File]::ReadAllText((Join-Path $repo $rel)).Replace("`r`n","`n")}
function Block([string]$s,[string]$sig){
 $start=$s.IndexOf($sig);if($start -lt 0){throw "Missing production block $sig"}
 $at=$s.IndexOf('{',$start);$end=$at+1;$depth=1
 while($depth -and $end -lt $s.Length){if($s[$end] -eq '{'){$depth++};if($s[$end] -eq '}'){$depth--};$end++}
 if($depth){throw "Unbalanced production block $sig"};$s.Substring($start,$end-$start)
}
function WriteGenerated([string]$name,[string]$s){[IO.File]::WriteAllText((Join-Path $out $name),$s,[Text.UTF8Encoding]::new($false))}
$h=Read 'core/rend/vr/xr_hands.cpp';$g=Read 'core/rend/vr/xr_gun.cpp';$own=Read 'core/rend/vr/xr_own.cpp';$ownHeader=Read 'core/rend/vr/xr_own.h'
$survivor=$h.Contains('Slot survivorSlots[');$mok=$h.Contains('uLightColor')
$types=@('namespace own {'+(Block $ownHeader 'struct GunFile')+';'+(Block $own 'std::string lower(')+(Block $own 'bool sameStem(')+(Block $own 'bool readFile(')+[regex]::Match($ownHeader,'(?s)constexpr size_t MaxTriangles.*?constexpr size_t MaxFileBytes[^;]+;').Value+'}',
 [regex]::Match($h,'enum \{ MeshFrame[^;]+;').Value)
foreach($sig in @('struct Texture','struct Part','struct Model','struct Slot','struct OwnGun')){$types+=(Block $h $sig)+';'}
$types+='constexpr size_t VertexSize = 36; constexpr int Slots=4; Slot slots[Slots]; std::vector<std::unique_ptr<OwnGun>> ownGuns;'
if($survivor){$types+='Slot survivorSlots[8];'}
$types+=[regex]::Match($h,'GLuint program;').Value
$types+=[regex]::Match($h,'GLint uMvp[^;]+;').Value
$begin=$g.IndexOf('struct Material');$end=$g.IndexOf('// A cheap deterministic random number',$begin)
$types+=$g.Substring($begin,$end-$begin)
WriteGenerated 'renderer_types.inc' ($types -join "`n")
$functions=@()
foreach($sig in @('bool initGl()','bool upload(','void releaseGl(','bool loadOwnGun(','void drawMesh(','void unloadOwn(','void drawHands(','void termHands()')){$functions+=Block $h $sig}
$functions+=[regex]::Match($g,'(?s)const char \*const ModelLightingSource = R"\(.*?\)";').Value
foreach($sig in @('bool initModel()','void drawModel(','glm::vec3 gunMuzzle(','static bool staffMagic(','static void muzzleNow(','void drawGunModel(','void termGun()')){if($g.Contains($sig)){$functions+=Block $g $sig}}
WriteGenerated 'renderer_functions.inc' ($functions -join "`n")
# Compile the exact production GLSL resolver and fragment entry prefix as GLM C++.
$resolver=''
$policy=Join-Path $repo 'core/rend/vr/arcade_gun_color.h'
if(Test-Path -LiteralPath $policy){$p=[IO.File]::ReadAllText($policy);$resolver=[regex]::Match($p,'(?s)R"\((.*?)\)"').Groups[1].Value}
$init=Block $h 'bool initGl()'
$frag=[regex]::Match($init,'(?s)fragment\.addSource\(PixelCompatShader\).*?R"\((.*?)\)"').Groups[1].Value
$main=Block $frag 'void main()'
$prefix=$main.Substring($main.IndexOf('{')+1,$main.IndexOf('highp vec3 n =')-$main.IndexOf('{')-1)
WriteGenerated 'shader_base.inc' ($resolver+"`nvec4 fragmentBase(vec4 sampled, vec4 vertex, vec4 overrideValue, float cutoff) { textureSample=sampled; vtx_color=vertex; arcadeBaseColor=overrideValue; alphaTest=cutoff;"+$prefix+"`nreturn base; }")
WriteGenerated 'types.h' "#pragma once`n#include <cstddef>`n#include <cstdint>`nusing u8=uint8_t; using u32=uint32_t;"
# Four semantic mesh/material pairs deliberately share exactly the same RGBA and image.
# Losing semantic identity while grouping would collapse plastic and metal into one draw.
$fixtureJson='{"asset":{"version":"2.0"},"buffers":[{"byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"materials":[{"name":"Material_1"},{"name":"Material_3"},{"name":"Material_2"},{"name":"Material_0"}],"meshes":[{"name":"hardsurfaces","primitives":[{"attributes":{"POSITION":0},"material":0}]},{"name":"lens","primitives":[{"attributes":{"POSITION":0},"material":1}]},{"name":"screws","primitives":[{"attributes":{"POSITION":0},"material":2}]},{"name":"softsurfaces","primitives":[{"attributes":{"POSITION":0},"material":3}]}],"nodes":[{"mesh":0},{"mesh":1},{"mesh":2},{"mesh":3}],"scenes":[{"nodes":[0,1,2,3]}],"scene":0}'
while($fixtureJson.Length%4){$fixtureJson+=' '}
$fixture=Join-Path $out 'semantic-parts.glb'
$stream=[IO.File]::Create($fixture);$writer=[IO.BinaryWriter]::new($stream)
try{$writer.Write([uint32]0x46546C67);$writer.Write([uint32]2);$writer.Write([uint32](12+8+$fixtureJson.Length+8+36));$writer.Write([uint32]$fixtureJson.Length);$writer.Write([uint32]0x4E4F534A);$writer.Write([Text.Encoding]::UTF8.GetBytes($fixtureJson));$writer.Write([uint32]36);$writer.Write([uint32]0x004E4942);foreach($v in @(0,0,0,1,0,0,0,1,0)){$writer.Write([float]$v)}}finally{$writer.Dispose();$stream.Dispose()}
WriteGenerated 'variant.inc' ("#define TEST_MOK "+[int]$mok+"`n#define TEST_SURVIVOR "+[int]$survivor)
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs){throw 'MSVC not found'}
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat';$exe=Join-Path $out 'arcade_color_test.exe';$log=Join-Path $out 'build.log';$cpp=Join-Path $PSScriptRoot 'arcade_color_test.cpp'
$batch=Join-Path $out 'build.cmd'
@"
@echo off
call "$vcvars" >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"$out" /I"$repo/core/deps/glm" /I"$repo/core/deps" /I"$repo/core/deps/stb" /I"$repo/core/rend/vr" /Fo"$out/" /Fe"$exe" "$cpp" "$repo/core/rend/vr/glb_mesh.cpp" >"$log" 2>&1
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $env:ComSpec /d /c $batch
if($LASTEXITCODE){Get-Content -LiteralPath $log;throw 'Arcade production renderer fixture compile failed'}
& $exe $fixture $NamcoGlb
exit $LASTEXITCODE
