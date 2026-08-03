TEMPLATE = subdirs
SUBDIRS = perigee_tests

perigee_tests.file = perigee-tests.pro

# Support debug and release builds from command line for CI
CONFIG += debug_and_release
