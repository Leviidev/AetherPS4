require 'xcodeproj'
require 'fileutils'

project_name = 'AetherPS4-iOS'
project_dir = 'AetherPS4-iOS'
project_path = "#{project_dir}/#{project_name}.xcodeproj"

FileUtils.mkdir_p(project_dir)

# Create a new Xcode project
project = Xcodeproj::Project.new(project_path)

# Add iOS target
# Must match CMAKE_OSX_DEPLOYMENT_TARGET used to build libshadps4_ios.a (see
# runtime/build/shadps4-ios's configure invocation) -- a lower value here links
# fine but is inconsistent metadata, and some of libshadps4_ios.a's object code
# was compiled assuming this floor.
app_target = project.new_target(:application, project_name, :ios, '26.0')

# Source directory
sources_dir = project.main_group.new_group('Sources', 'Sources')
# Add all swift files under Sources to the target
Dir.glob("#{project_dir}/Sources/**/*.swift").each do |file|
  next if File.directory?(file)
  file_ref = sources_dir.new_file(File.absolute_path(file))
  app_target.add_file_references([file_ref])
end

# Frameworks directory
frameworks_dir = project.main_group.new_group('Frameworks', 'Frameworks')
# Add BreakpointJIT.framework
bp_jit_ref = frameworks_dir.new_file(File.absolute_path("#{project_dir}/Frameworks/BreakpointJIT.framework"))
app_target.frameworks_build_phase.add_file_reference(bp_jit_ref, true)

# Embed Frameworks build phase
embed_frameworks_phase = project.new(Xcodeproj::Project::Object::PBXCopyFilesBuildPhase)
embed_frameworks_phase.name = 'Embed Frameworks'
embed_frameworks_phase.dst_subfolder_spec = '10' # Frameworks
app_target.build_phases << embed_frameworks_phase
embed_frameworks_phase.add_file_reference(bp_jit_ref)

# Resources directory
resources_dir = project.main_group.new_group('Resources', 'Resources')
# Info.plist and Entitlements
info_plist_ref = resources_dir.new_file(File.absolute_path("#{project_dir}/Info.plist"))
# app_target.resources_build_phase.add_file_reference(info_plist_ref, true)

# Bridging header
bridging_header = project.main_group.new_file(File.absolute_path("#{project_dir}/AetherPS4-iOS-Bridging-Header.h"))

# Add all .a libraries from the CMake build directory
libs_group = project.main_group.new_group('Libs')
build_dir = File.absolute_path('runtime/build/shadps4-ios')
Dir.glob("#{build_dir}/**/*.a").each do |lib|
  next if lib.include?('CMakeFiles')
  # No 'ffmpeg-' exclusion here: that was a workaround for a stale x86_64 ffmpeg
  # build left over in this same build directory from an earlier (pre-iOS-cross-
  # compile) configure. A clean `rm -rf` + reconfigure of runtime/build/shadps4-ios
  # only ever produces genuine arm64 libraries (verified via `lipo -info` on every
  # .a here), so excluding anything by path substring is unnecessary and would
  # incorrectly drop the real iOS FFmpeg libs (externals/ffmpeg-94dde08/lib/*.a).
  lib_ref = libs_group.new_file(lib)
  app_target.frameworks_build_phase.add_file_reference(lib_ref, true)
end

# Embed libshadps4_ios.dylib
dylib_path = "#{build_dir}/libshadps4_ios.dylib"
if File.exist?(dylib_path)
  dylib_ref = frameworks_dir.new_file(dylib_path)
  embed_frameworks_phase.add_file_reference(dylib_ref)
end

# Add libiconv and libc++
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('usr/lib/libc++.tbd', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('usr/lib/libiconv.tbd', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/Metal.framework', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/QuartzCore.framework', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/GameController.framework', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/CoreHaptics.framework', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/AVFoundation.framework', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/CoreAudio.framework', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/AudioToolbox.framework', :sdk_root), true)
app_target.frameworks_build_phase.add_file_reference(project.frameworks_group.new_file('System/Library/Frameworks/CoreBluetooth.framework', :sdk_root), true)

# Configure Build Settings
app_target.build_configurations.each do |config|
  config.build_settings['PRODUCT_BUNDLE_IDENTIFIER'] = "com.aether.ps4ios"
  config.build_settings['INFOPLIST_FILE'] = "Info.plist"
  config.build_settings['CODE_SIGN_ENTITLEMENTS'] = "AetherPS4-iOS.entitlements"
  config.build_settings['SWIFT_VERSION'] = '5.0'
  config.build_settings['ENABLE_BITCODE'] = 'NO'
  config.build_settings['ONLY_ACTIVE_ARCH'] = 'YES'
  config.build_settings['VALID_ARCHS'] = 'arm64'
  config.build_settings['CLANG_CXX_LANGUAGE_STANDARD'] = 'c++20'
  config.build_settings['CLANG_CXX_LIBRARY'] = 'libc++'
  config.build_settings['OTHER_LDFLAGS'] = ['-ObjC', '-lc++']
  config.build_settings['LIBRARY_SEARCH_PATHS'] = ['$(inherited)', build_dir, "#{build_dir}/**"]
  config.build_settings['FRAMEWORK_SEARCH_PATHS'] = ['$(inherited)', "#{File.absolute_path(project_dir)}/Frameworks"]
  config.build_settings['HEADER_SEARCH_PATHS'] = ['$(inherited)', File.absolute_path('src/platform/ios')]
  config.build_settings['SWIFT_OBJC_BRIDGING_HEADER'] = "AetherPS4-iOS-Bridging-Header.h"
  
  # Important for JIT and entitlements
  config.build_settings['CODE_SIGN_IDENTITY'] = 'Apple Development'
  config.build_settings['DEVELOPMENT_TEAM'] = '' # Left blank for sideloading
end

project.save
puts "Generated #{project_path}"
