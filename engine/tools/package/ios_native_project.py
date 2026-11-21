#!/usr/bin/env python3
# Turns a copy of the Go-engine "CvF Online" Xcode project into the native
# engine app (IOS_PORT.md, implementation status): libikemen.a + ffmpeg/vpx/
# xmp + MoltenVK out, CvfEngine.xcframework (Frameworks/) + OpenGLES in,
# version 2.0 (2). Idempotent. Also replace #import "libikemen.h" with
# #import "cvf_host.h" in ViewController.m and TouchOverlayView.m, put
# CvfEngine.xcframework (tools/package/ios_build.sh) and an SDL2.xcframework
# with a Simulator slice in Frameworks/, and DefaultAssets.zip from
# tools/package/ios_assets.sh next to the project.
# Usage: ios_native_project.py PATH/I.K.E.M.E.N-Go.xcodeproj/project.pbxproj
import sys
pbx = sys.argv[1]
s = open(pbx).read()
if 'CvfEngine.xcframework' not in s:
    s = s.replace('F1E2A08330409079001C162E /* libikemen.a in Frameworks */ = {isa = PBXBuildFile; fileRef = F1E2A08230409079001C162E /* libikemen.a */; };',
                  'F1E2A08330409079001C162E /* CvfEngine.xcframework in Frameworks */ = {isa = PBXBuildFile; fileRef = F1E2A08230409079001C162E /* CvfEngine.xcframework */; };')
    s = s.replace('F1E2A08230409079001C162E /* libikemen.a */ = {isa = PBXFileReference; lastKnownFileType = archive.ar; path = libikemen.a; sourceTree = "<group>"; };',
                  'F1E2A08230409079001C162E /* CvfEngine.xcframework */ = {isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; name = CvfEngine.xcframework; path = Frameworks/CvfEngine.xcframework; sourceTree = "<group>"; };')
    s = s.replace('F1E2A08330409079001C162E /* libikemen.a in Frameworks */,', 'F1E2A08330409079001C162E /* CvfEngine.xcframework in Frameworks */,')
    s = s.replace('\t\t\t\tF1E2A08230409079001C162E /* libikemen.a */,\n', '')
    s = s.replace('\t\t\t\tF1E2A069303E3CA7001C162E /* SDL2.xcframework */,\n',
                  '\t\t\t\tF1E2A069303E3CA7001C162E /* SDL2.xcframework */,\n\t\t\t\tF1E2A08230409079001C162E /* CvfEngine.xcframework */,\n')
    for name in ['libswresample.a', 'libswscale.a', 'libavformat.a', 'libavcodec.a', 'libavutil.a',
                 'libavfilter.a', 'libvpx.a', 'libavdevice.a', 'libxmp.a', 'MoltenVK.xcframework', 'libikemen.h']:
        s = '\n'.join(l for l in s.split('\n') if ('/* %s ' % name) not in l and ('/* %s in ' % name) not in l) + '\n'
    s = s.replace('"-lc++",\n', '"-lc++",\n\t\t\t\t\t"-framework",\n\t\t\t\t\tOpenGLES,\n')
    s = s.replace('MARKETING_VERSION = 1.1;', 'MARKETING_VERSION = 2.0;')
    s = s.replace('CURRENT_PROJECT_VERSION = 1;', 'CURRENT_PROJECT_VERSION = 2;')
open(pbx, 'w').write(s)
for bad in ['libikemen', 'MoltenVK', 'libavcodec']:
    assert bad not in s, bad
print('project: CvfEngine.xcframework linked, %d OpenGLES flags' % s.count('OpenGLES'))
