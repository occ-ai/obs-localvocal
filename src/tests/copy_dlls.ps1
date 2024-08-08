
# change into the root directory of the repository from the location of this script
$scriptPath = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location "$scriptPath\..\.."

$testToolPath = ".\release\Release\test"

# make sure the test tool directory exists
if (-not (Test-Path $testToolPath)) {
    New-Item -ItemType Directory -Path $testToolPath | Out-Null
}

# copy the required DLLs to the test tool directory
$obsDlls = @(
    ".\release\Release\obs-plugins\64bit\ctranslate2.dll",
    ".\release\Release\obs-plugins\64bit\cublas64_12.dll",
    ".\release\Release\obs-plugins\64bit\cublasLt64_12.dll",
    ".\release\Release\obs-plugins\64bit\cudart64_12.dll",
    ".\release\Release\obs-plugins\64bit\libopenblas.dll",
    ".\release\Release\obs-plugins\64bit\onnxruntime_providers_shared.dll",
    ".\release\Release\obs-plugins\64bit\onnxruntime.dll",
    ".\release\Release\obs-plugins\64bit\whisper.dll",
    ".\release\Release\obs-plugins\64bit\ggml.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\avcodec-60.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\avdevice-60.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\avfilter-9.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\avformat-60.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\avutil-58.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\libx264-164.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\swresample-4.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\swscale-7.dll",
    ".deps\obs-deps-2024-03-19-x64\bin\zlib.dll"
    ".deps\obs-deps-2024-03-19-x64\bin\librist.dll"
    ".deps\obs-deps-2024-03-19-x64\bin\srt.dll"
    ".deps\obs-studio-30.1.2\build_x64\rundir\Debug\bin\64bit\obs-frontend-api.dll",
    ".deps\obs-studio-30.1.2\build_x64\rundir\Debug\bin\64bit\obs.dll",
    ".deps\obs-studio-30.1.2\build_x64\rundir\Debug\bin\64bit\w32-pthreads.dll"
)

$obsDlls | ForEach-Object {
    Copy-Item -Force -Path $_ -Destination $testToolPath
}
