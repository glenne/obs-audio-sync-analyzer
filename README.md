# Audio Sync Analyzer

## Introduction

Audio Sync Analyzer adds an OBS audio filter that analyzes and corrects sync between sources using cross-correlation.

Audio is sampled from the two audio inputs and a numerical correlation is done to identify the delay which provides the maximum correlation.  Provided the confidence level is high enough, the delay is considered 'valid'.

## Usage

1. Provide a significant audio source level that is captured by both audio inputs of interest
2. Select the non-delayed input source (typically straight from audio mixer)
3. Select the audio source associated with one of the cameras as the 'target' audio device
4. Click on 'Measure Now'
5. If delay is reasonable, click on 'Apply to Sync Offset'

## Building the Plugin Locally

```bash
CI=1 ./.github/scripts/build-macos
open ./release/RelWithDebInfo/audio-sync-analyzer.pkg
# restart OBS
```

## Formating Code

```bash
clang-format -i src/*
```

## References

This plugin was leveraged from the [OBS plugin template example](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide).
