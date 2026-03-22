package mental

import _ "unsafe"

//go:cgo_import_dynamic libc_dlopen dlopen "libdl.so.2"
//go:cgo_import_dynamic libc_dlsym dlsym "libdl.so.2"
//go:cgo_import_dynamic libc_dlclose dlclose "libdl.so.2"
//go:cgo_import_dynamic libc_dlerror dlerror "libdl.so.2"
