#!/usr/bin/env sh
set -eu

./scripts/apply-upstream-patches.sh
./scripts/verify-upstream-patch.sh
./scripts/verify-editor-audio-isolation.sh
./tests/release-tag-test.sh
./tests/github-workflow-policy-test.sh
./tests/windows-build-policy-test.sh

dotnet build bridge/OpenUtau.Vst.sln --configuration Release -p:WarningLevel=0
./tests/package-manifest-test.sh
dotnet build upstream/OpenUtau/OpenUtau.csproj --configuration Release --verbosity minimal -p:WarningLevel=0
dotnet test upstream/OpenUtau.Test/OpenUtau.Test.csproj --configuration Release \
    --filter FullyQualifiedName~PhonemizerRunnerTest --verbosity minimal \
    -p:WarningLevel=0
dotnet run --project bridge/OpenUtau.Vst.Protocol.Tests --configuration Release --no-build
dotnet run --project bridge/OpenUtau.Vst.Engine.Tests --configuration Release --no-build
dotnet run --project bridge/OpenUtau.Vst.EditorHost.Tests --configuration Release --no-build
./scripts/verify-windows-engine-publish.sh
cmake -S plugin -B .build/plugin -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/plugin
ctest --test-dir .build/plugin --output-on-failure
