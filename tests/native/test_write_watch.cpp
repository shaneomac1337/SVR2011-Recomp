#include <catch_amalgamated.hpp>

#include <memory>

#include "native/write_watch.h"

using svr::native::write_watch::PageBitmap;

TEST_CASE("page bitmap tracks clean ranges by 4 KB page") {
  auto pages = std::make_unique<PageBitmap>();
  CHECK_FALSE(pages->AllClean(0x1000, 0x100));

  pages->MarkClean(0x10000, 0x3000);  // pages 0x10..0x12
  CHECK(pages->AllClean(0x10000, 0x3000));
  CHECK(pages->AllClean(0x10800, 0x100));
  CHECK_FALSE(pages->AllClean(0xF000, 0x2000));
  CHECK_FALSE(pages->AllClean(0x12000, 0x1001));

  // A one-byte write dirties its whole page only.
  pages->MarkDirty(0x11FFF, 1);
  CHECK_FALSE(pages->AllClean(0x10000, 0x3000));
  CHECK(pages->AllClean(0x10000, 0x1000));
  CHECK(pages->AllClean(0x12000, 0x1000));
}

TEST_CASE("page bitmap ranges across 64-page blocks and the end of memory") {
  auto pages = std::make_unique<PageBitmap>();
  pages->MarkClean(0x3F000, 0x82000);  // pages 0x3F..0xC0, three blocks
  CHECK(pages->AllClean(0x3F000, 0x82000));
  pages->MarkDirty(0x80000, 4);
  CHECK_FALSE(pages->AllClean(0x3F000, 0x82000));
  CHECK(pages->AllClean(0x81000, 0x40000));

  pages->MarkClean(0x1FFFF000, 0x2000);  // clamped to the last page
  CHECK(pages->AllClean(0x1FFFF000, 0x1000));
  CHECK_FALSE(pages->AllClean(0x20000000, 0x1000));
  CHECK_FALSE(pages->AllClean(0x5000, 0));
}
