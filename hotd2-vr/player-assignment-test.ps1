# Test the actual production player-assignment and joining code without an XR runtime.
param([string]$Output='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(-not $Output){$Output=Join-Path $repo 'build-win\vrtest\player-assignment'}
$out=[IO.Path]::GetFullPath($Output)
if(-not $out.StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'build-win'))+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Test output must stay under build-win'}
New-Item -ItemType Directory -Path $out -Force | Out-Null
$hostSource=[IO.File]::ReadAllText((Join-Path $repo 'core\rend\vr\xr_host.cpp')).Replace("`r`n","`n")
$menuSource=[IO.File]::ReadAllText((Join-Path $repo 'core\rend\vr\xr_menu.cpp')).Replace("`r`n","`n")
function ProductionBlock([string]$Source,[string]$Signature){
 $start=$Source.IndexOf($Signature)
 if($start -lt 0){throw "Missing production code: $Signature"}
 $begin=$Source.IndexOf('{',$start);$depth=1;$end=$begin+1
 while($depth -gt 0 -and $end -lt $Source.Length){if($Source[$end] -eq '{'){$depth++};if($Source[$end] -eq '}'){$depth--};$end++}
 if($depth -ne 0){throw "Unbalanced production code: $Signature"}
 $Source.Substring($start,$end-$start)
}
$reprojectSource=[IO.File]::ReadAllText((Join-Path $repo 'core/rend/vr_reproject.cpp')).Replace("`r`n","`n")
$code=@((ProductionBlock $reprojectSource 'bool gameStartLoaded()'),(ProductionBlock $reprojectSource 'bool gameStartReady()'))
foreach($signature in @('int twoPlayerP1Hand()','int dualP1Hand()','int p1Hand()','void suppressGunInput(Gun& g)','void swapGunHands()','int startButtonOwner(int hand)','void playerStartButtons(','void resetStartGestures()', 'bool serviceMenuAvailable()', 'bool serviceMenuExitAvailable()', 'bool openServiceMenu()', 'void notifyServiceGameBoot()', 'void sampleStartButtons(')){
 if($hostSource.Contains($signature)){$code+=ProductionBlock $hostSource $signature}
}
$update=$hostSource.IndexOf('void updateLightgun(')
$resetBlock=ProductionBlock $hostSource.Substring($update) 'if (config::VrXrGun && newGame.exchange(false))'
$focusBlock=ProductionBlock $hostSource.Substring($update) 'if (!focused)'
if(-not $focusBlock.Contains('resetStartGestures();')){throw 'Focus loss must reset pending Start/service gestures'}
$syncBegin=$hostSource.IndexOf('if (!XR_SUCCEEDED(XR(xrSyncActions)', $update)
$syncEnd=$hostSource.IndexOf('if (!boolAction(startAction, Right))', $syncBegin)
if(-not $hostSource.Substring($syncBegin,$syncEnd-$syncBegin).Contains('resetStartGestures();')){throw 'Action sync failure must reset pending Start/service gestures'}
$code+='void resetForNewGame(){'+$resetBlock+'}'
$chooseBegin=$menuSource.IndexOf('void choose(')
$serviceBegin=$menuSource.IndexOf('case ServiceMenu:', $chooseBegin)
$serviceEnd=$menuSource.IndexOf('case Quit:', $serviceBegin)
if($serviceBegin -lt 0 -or $serviceEnd -le $serviceBegin){throw 'Missing Service menu action'}
$serviceCase=$menuSource.Substring($serviceBegin+'case ServiceMenu:'.Length,$serviceEnd-$serviceBegin-'case ServiceMenu:'.Length)
$code+='void chooseServiceRow(){switch(0){case 0:'+$serviceCase+'}}'

$begin=$hostSource.IndexOf('int hand1 = p1Hand(), hand2 = 1 - hand1;',$update)
$joinBegin=$hostSource.IndexOf('const bool canJoin =',$update)
if($joinBegin -ge 0 -and ($begin -lt 0 -or $joinBegin -lt $begin)){$begin=$joinBegin}
$begin=$hostSource.LastIndexOf("`n",$begin)+1
$end=$hostSource.IndexOf("// The game's first Start of player 1's or trigger pull",$begin)
if($begin -lt 0 -or $end -le $begin){throw 'Missing production hand/join block'}
$menuBegin=$hostSource.IndexOf('if (menuOpen)',$update)
$sampleAt=$hostSource.IndexOf('sampleStartButtons(time, leftStart, rightStart,',$update)
$prepAt=$hostSource.IndexOf('if (preparing)',$update)
if($sampleAt -lt 0 -or $sampleAt -gt $menuBegin -or $sampleAt -gt $prepAt){throw 'Physical Start ownership must sample before menu/preparation returns'}
$menuBlock=ProductionBlock $hostSource.Substring($update) 'if (menuOpen)'
$code+='void menuFrame(bool leftStart,bool rightStart){XrTime time=0;bool menuEdge=false,skip=false,skipEdge=false;bool p1Start,p2Start;playerStartButtons(leftStart,rightStart,p1Start,p2Start);'+$menuBlock+'}'
$code+='void frameCode(XrTime time, const bool tracked[2], float pulls[2], bool leftStart, bool skip, u32 dpad=0){bool p1Start,p2Start;playerStartButtons(leftStart,boolAction(startAction,Right)&&!startSwallow,p1Start,p2Start);'+$hostSource.Substring($begin,$end-$begin)+'lastP1=p1;if(dualWield)lastP2=p2;}'
$code+=[regex]::Match($menuSource,'const char \*const HandsNames\[2\] \{[^;]+;').Value
$cases=[regex]::Matches($menuSource,'(?s)case (?:Hands|TwoPlayers):(.*?)case Laser:')
if($cases.Count -ne 2){throw 'Missing production menu cases'}
$code+='std::string menuValue(){switch(0){case 0:'+$cases[0].Groups[1].Value+'}return {};}'
$code+='void toggleMenu(){switch(0){case 0:'+$cases[1].Groups[1].Value+'}}'
$jvsSource=[IO.File]::ReadAllText((Join-Path $repo 'core/hw/maple/maple_jvs.cpp')).Replace("`r`n","`n")
$keySource=[IO.File]::ReadAllText((Join-Path $repo 'core/hw/maple/maple_devs.h'))
$inputSource=[IO.File]::ReadAllText((Join-Path $repo 'core/hw/naomi/naomi_roms_input.h'))
$cartSource=[IO.File]::ReadAllText((Join-Path $repo 'core/hw/naomi/naomi_cart.h'))
$code+=[regex]::Match($keySource,'(?s)enum NAOMI_KEYS\s*\{.*?\};').Value
$code+=[regex]::Match($cartSource,'(?s)struct ButtonDescriptor\s*\{.*?\};').Value
$code+='struct InputDescriptors {ButtonDescriptor buttons[18];};'
$code+=[regex]::Match($inputSource,'(?s)#define NAO_BASE_BTN_DESC.*?(?=#define)').Value
$code+=[regex]::Match($inputSource,'(?s)static InputDescriptors ninjaslt_inputs = \{.*?\};').Value
$code+='struct MapleInputState {struct {int x=0,y=0;}absPos;};MapleInputState mapleInputState[4];'
$code+='#define verify(x) assert(x)'
$code+='struct NativeJvsMapper {std::array<u32,32>cur_mapping{},p1_mapping{},p2_mapping{};unsigned first_player=0;bool lightgun_as_analog=false;InputDescriptors* NaomiGameInputs=&ninjaslt_inputs;'+(ProductionBlock $jvsSource 'void init_mappings()')+(ProductionBlock $jvsSource 'virtual void read_digital_in(const u32 *buttons, u32 *v)')+'};'
$code+='u32 serviceEnterMask(ServiceStyle serviceStyle){'+[regex]::Match($jvsSource,'(?s)const u32 enter=.*?;').Value+'return enter;}'
[IO.File]::WriteAllText((Join-Path $out 'under_test.inc'),($code -join "`n"),[Text.UTF8Encoding]::new($false))
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$cpp=Join-Path $PSScriptRoot 're\player_assignment_test.cpp';$exe=Join-Path $out 'player_assignment_test.exe';$log=Join-Path $out 'build.log'
$batch=Join-Path $out 'build.cmd'
@"
@echo off
call "$vcvars" >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"$out" /Fo"$out\player_assignment.obj" /Fe"$exe" "$cpp" >"$log" 2>&1
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $env:ComSpec /d /c $batch
if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log;throw 'Player assignment test compilation failed'}
& $exe
exit $LASTEXITCODE
