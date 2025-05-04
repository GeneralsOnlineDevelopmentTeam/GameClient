$patchDirectoryPath = "Patch"  
$buildDir = "build\win32\GeneralsMD\Release";
$buildVer = 1;

$crcFilesDir = "$patchDirectoryPath/private/genonlineserver/crcfiles"
$installerDir = "$patchDirectoryPath/public_html"
$updaterDir = "$patchDirectoryPath/public_html/updater"
$workingDir = "$patchDirectoryPath/working_dir"

# Check if the patch directory exists  
if (Test-Path $patchDirectoryPath)
{  
   # If it exists, delete all contents  
   Get-ChildItem -Path $patchDirectoryPath -Recurse -Force | Remove-Item -Recurse -Force  
}
else
{  
   # If it does not exist, create the directory  
   New-Item -ItemType Directory -Path $patchDirectoryPath
}

# make our folder structure
New-Item -ItemType Directory -Path "$crcFilesDir"
New-Item -ItemType Directory -Path "$updaterDir"
New-Item -ItemType Directory -Path "$workingDir"

# Copy libcurl, zlib and release exe to working dir
Copy-Item -Path "$buildDir\libcurl.dll" -Destination $workingDir
Copy-Item -Path "$buildDir\zlib1.dll" -Destination $workingDir
Copy-Item -Path "$buildDir\GeneralsOnlineZH.exe" -Destination $workingDir

# package up our patch and installer
Compress-Archive -Path "$workingDir\*" -DestinationPath "$workingDir\v$buildVer.zip"

# Copy the patch to the patch directory  
Copy-Item -Path "$workingDir\v$buildVer.zip" -Destination "$installerDir\v$buildVer.zip"
Copy-Item -Path "$workingDir\v$buildVer.zip" -Destination "$updaterDir\v$buildVer.gopatch"

# copy the exe to crcfiles
Copy-Item -Path "$buildDir\GeneralsOnlineZH.exe" -Destination "$crcFilesDir"

# cleanup working dir
if (Test-Path $workingDir)
{  
   # If it exists, delete all contents  
   Get-ChildItem -Path $workingDir -Recurse -Force | Remove-Item -Recurse -Force  

   # Remove the working directory itself
   Remove-Item -Path $workingDir -Recurse -Force
}