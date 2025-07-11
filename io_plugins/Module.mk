plugindlls            += \
    io_launchpad \
    io_dummy

srcpp_io_dummy          := \
    io_dummy.cc

ldflags_io_launchpad    := \
    -lwinmm -lpsapi

srcpp_io_launchpad      := \
    io_launchpad.cc
