param([string]$Repo='')
$ErrorActionPreference='Stop'
if(-not $Repo){$Repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path}
$android='http://schemas.android.com/apk/res/android'
[xml]$main=Get-Content -Raw -LiteralPath (Join-Path $Repo 'shell/android-studio/flycast/src/main/AndroidManifest.xml')
[xml]$vr=Get-Content -Raw -LiteralPath (Join-Path $Repo 'shell/android-studio/flycast/src/vr/AndroidManifest.xml')
function Attr($node,[string]$name){if($null -eq $node){return ''};$node.GetAttribute($name,$android)}
$gameName='com.flycast.emulator.NativeGLActivity'
$mainGame=@($main.manifest.application.activity)|Where-Object {(Attr $_ 'name') -eq $gameName}
$vrGame=@($vr.manifest.application.activity)|Where-Object {(Attr $_ 'name') -eq $gameName}
$setup=@($vr.manifest.application.activity)|Where-Object {@($_.'intent-filter'.category)|Where-Object {(Attr $_ 'name') -eq 'com.oculus.intent.category.2D'}}
$checks=0;$failed=0
function Check([bool]$ok,[string]$why){$script:checks++;if($ok){Write-Output "PASS $why"}else{$script:failed++;Write-Output "FAIL $why"}}
$defaultAffinity=Attr $main.manifest.application 'taskAffinity'
if(-not $defaultAffinity){$defaultAffinity='${applicationId}'}
$gameAffinity=Attr $vrGame 'taskAffinity';if(-not $gameAffinity){$gameAffinity=Attr $mainGame 'taskAffinity'};if(-not $gameAffinity){$gameAffinity=$defaultAffinity}
$setupAffinity=Attr $setup 'taskAffinity';if(-not $setupAffinity){$setupAffinity=$defaultAffinity}
Check ($null -ne $mainGame) 'resolved OpenXR render activity exists'
Check ($null -ne $setup) 'library still opens the 2D setup panel'
Check ($gameAffinity -ne $setupAffinity) 'clearing/restarting the immersive task cannot clear its calling setup task'
Check ($gameAffinity.StartsWith('${applicationId}')) 'immersive task affinity remains private to this game package'
$actualFilters=@($vrGame.'intent-filter')|Where-Object {
 $cats=@($_.category)|ForEach-Object {Attr $_ 'name'}
 (Attr $_.action 'name') -eq 'android.intent.action.MAIN' -and
 $cats -contains 'android.intent.category.DEFAULT' -and
 $cats -contains 'com.oculus.intent.category.VR' -and
 $cats -contains 'org.khronos.openxr.intent.category.IMMERSIVE_HMD'
}
$actualCategories=@($actualFilters.category)|ForEach-Object {Attr $_ 'name'}
Check ($actualCategories -contains 'com.oculus.intent.category.VR' -and $actualCategories -contains 'org.khronos.openxr.intent.category.IMMERSIVE_HMD') 'actual OpenXR lifecycle activity is classified as immersive, independently of its alias'
$alias=@($vr.manifest.application.'activity-alias')|Where-Object {(Attr $_ 'name') -eq 'com.flycast.emulator.MainActivity'}
Check ((Attr $alias 'targetActivity') -eq $gameName) 'existing installer and adb immersive entry is preserved'
$categories=@($alias.'intent-filter'.category)|ForEach-Object {Attr $_ 'name'}
Check ($categories -contains 'com.oculus.intent.category.VR') 'existing immersive entry is classified as Quest VR'
$setupSource=if((Attr $setup 'name') -eq 'com.flycast.emulator.CmStartActivity'){'CmStartActivity.java'}else{'VrSetupActivity.java'}
$text=[IO.File]::ReadAllText((Join-Path $Repo ('shell/android-studio/flycast/src/main/java/com/flycast/emulator/'+$setupSource)))
$start=$text.IndexOf('private void play(');$end=$text.IndexOf('private void endGameProcess(',$start)
if($start -lt 0 -or $end -lt 0){throw 'Could not extract production Play handoff'}
$play=$text.Substring($start,$end-$start)
Check ($play.Contains('endGameProcess();')) 'fresh Play still ends only the previous game process'
Check ($play.Contains('Intent.FLAG_ACTIVITY_NEW_TASK') -and $play.Contains('Intent.FLAG_ACTIVITY_CLEAR_TASK')) 'game restart still launches a fresh immersive task'
$launch=$play.IndexOf('startActivity(intent);');$close=$play.IndexOf('finishAndRemoveTask();')
Check ($launch -ge 0 -and $close -gt $launch) 'exclusive hybrid handoff removes the old panel task after launching VR'
Check ($play.Contains('intent.putExtra(EXTRA_GAME,')) 'selected original game path is retained'
Write-Output "Quest launch contract: $checks checks, $failed failures"
if($failed){exit 1}
