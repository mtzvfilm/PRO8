#!/bin/sh
# Builds plugin/build/PRO8-<version>.pkg: CLAP, VST3 and AU into /Library/Audio/Plug-Ins (each format can be deselected).
# Unsigned (no Apple developer account): on another Mac open it with right-click > Open (or System Settings > Privacy &
# Security > Open Anyway). Files installed by the installer carry no quarantine flag, so the plugins load in the DAW.
set -e
cd "$(dirname "$0")"
V=1.0.0; OUT=build/out; W=build/pkg; rm -rf $W; mkdir -p $W
for f in clap:CLAP:PRO8.clap vst3:VST3:PRO8.vst3 au:Components:PRO8.component; do
    id=${f%%:*}; rest=${f#*:}; dir=${rest%%:*}; b=${rest#*:}
    mkdir -p $W/root_$id/Library/Audio/Plug-Ins/$dir
    ditto $OUT/$b $W/root_$id/Library/Audio/Plug-Ins/$dir/$b
    pkgbuild --analyze --root $W/root_$id $W/comp_$id.plist >/dev/null
    plutil -replace 0.BundleIsRelocatable -bool NO $W/comp_$id.plist            # always install to /Library/Audio/Plug-Ins
    pkgbuild --root $W/root_$id --component-plist $W/comp_$id.plist --identifier com.mtzvfilm.pro8.$id --version $V \
             --install-location / $W/pro8_$id.pkg >/dev/null
done
cat > $W/dist.xml <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>PRO8 $V</title>
    <welcome mime-type="text/plain"><![CDATA[PRO8 - 8-voice analog polysynth (CLAP, VST3, AU).
DSP by mtzvfilm - ig: mtzv_film

The plugins are installed to /Library/Audio/Plug-Ins.]]></welcome>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="clap"/><line choice="vst3"/><line choice="au"/>
    </choices-outline>
    <choice id="clap" title="CLAP" description="PRO8.clap"><pkg-ref id="com.mtzvfilm.pro8.clap"/></choice>
    <choice id="vst3" title="VST3" description="PRO8.vst3"><pkg-ref id="com.mtzvfilm.pro8.vst3"/></choice>
    <choice id="au" title="Audio Unit" description="PRO8.component (Logic, GarageBand, ...)"><pkg-ref id="com.mtzvfilm.pro8.au"/></choice>
    <pkg-ref id="com.mtzvfilm.pro8.clap" version="$V">pro8_clap.pkg</pkg-ref>
    <pkg-ref id="com.mtzvfilm.pro8.vst3" version="$V">pro8_vst3.pkg</pkg-ref>
    <pkg-ref id="com.mtzvfilm.pro8.au" version="$V">pro8_au.pkg</pkg-ref>
</installer-gui-script>
XML
productbuild --distribution $W/dist.xml --package-path $W build/PRO8-$V.pkg
echo "built plugin/build/PRO8-$V.pkg"
