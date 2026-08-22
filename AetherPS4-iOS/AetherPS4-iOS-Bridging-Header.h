// libshadps4_ios.a is linked directly into this app's binary (see the "Frameworks"
// build phase) -- shadps4_init/shadps4_run/shadps4_stop etc. are already present in
// the executable at launch, so this just exposes their C declarations to Swift. No
// dlopen/dlsym needed; there is no separate libshadps4_ios.dylib to load.
#import "shadps4_ios_api.h"
#import "pkg_extractor.h"
