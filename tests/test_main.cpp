// tests/test_main.cpp
//
// Asks doctest to generate main(), which finds and runs all tests.
// Kept in its own file so this heavy part compiles only once, not every
// time you change a test.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"