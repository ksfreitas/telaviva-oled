// Test entry point: runs every test, or only those whose name contains argv[1].
// Pass --list to print test names without running them.
#include "TestHarness.h"

#include <cstring>

int main(int argc, char** argv)
{
    const bool listOnly = argc > 1 && std::strcmp(argv[1], "--list") == 0;
    const std::string filter = (argc > 1 && !listOnly) ? argv[1] : "";
    return telaviva_test::runTests(filter, listOnly);
}
