avsdlls            += jubeathax

deplibs_jubeathax    := \
    avs \

ldflags_jubeathax    := \
	-lpsapi

libs_jubeathax       := \
	util \
	minhook \
	libdisasm

srcpp_jubeathax      := \
	dllmain.cc \
	external_io.cc
