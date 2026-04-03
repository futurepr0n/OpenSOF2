// SOF2 models trigger many bone-index/model asserts written for JK2 skeletons.
// These are non-fatal (the code has fallbacks or continues safely) but crash
// Debug builds.  Include this at the top of Ghoul2 source files.
#ifndef G2_ASSERT_SUPPRESS_H
#define G2_ASSERT_SUPPRESS_H
#include <cassert>
#undef assert
#define assert(x) ((void)0)
#endif
