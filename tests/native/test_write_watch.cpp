#include <catch_amalgamated.hpp>

#include <memory>

#include "native/write_watch.h"

using svr::native::write_watch::PageStamps;

TEST_CASE("page stamps report writes after a token by 4 KB page") {
  auto pages = std::make_unique<PageStamps>();
  CHECK_FALSE(pages->WrittenAfter(0x10000, 0x3000, 0));

  pages->Stamp(0x11FFF, 1, 5);  // one byte: page 0x11 only
  CHECK(pages->WrittenAfter(0x10000, 0x3000, 4));
  CHECK_FALSE(pages->WrittenAfter(0x10000, 0x3000, 5));
  CHECK_FALSE(pages->WrittenAfter(0x10000, 0x1000, 0));
  CHECK_FALSE(pages->WrittenAfter(0x12000, 0x1000, 0));
}

TEST_CASE("entries sharing a page keep their own view of it") {
  auto pages = std::make_unique<PageStamps>();
  // A and B share page 0x20; A watched at epoch 1, B at epoch 1.
  pages->Stamp(0x20100, 4, 2);  // the game writes A's part
  const uint32_t token_b = 2;   // B re-reads the page and re-watches
  CHECK_FALSE(pages->WrittenAfter(0x20800, 16, token_b));
  // A, still at its old token, sees the write.
  CHECK(pages->WrittenAfter(0x20100, 16, 1));
}

TEST_CASE("stamps only move forward and clamp to physical memory") {
  auto pages = std::make_unique<PageStamps>();
  pages->Stamp(0x5000, 0x1000, 9);
  pages->Stamp(0x5000, 0x1000, 3);
  CHECK(pages->WrittenAfter(0x5000, 1, 8));
  pages->Stamp(0x1FFFF000, 0x2000, 1);  // clamped to the last page
  CHECK(pages->WrittenAfter(0x1FFFF000, 0x1000, 0));
  // Out of range or empty ranges always read as written.
  CHECK(pages->WrittenAfter(0x20000000, 0x1000, 100));
  CHECK(pages->WrittenAfter(0x5000, 0, 100));
}
