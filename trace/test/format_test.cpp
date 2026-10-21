// Checks for tatrace/format.hpp. The layout and the round-trips are checked at
// compile time; the runtime part checks saturation and that the packing
// contracts abort. Exits nonzero if any check fails.

#include "ta/testing.hpp"
#include "tatrace/format.hpp"

#include <cstdint>

namespace {

using namespace tatrace;
using namespace ta::test;

// flags
static_assert(flagBits() == 0);
static_assert(flagBits(Flag::Nothrow, Flag::Sized) == 0b101);
static_assert(hasFlag(flagBits(Flag::Aligned, Flag::InPlace), Flag::InPlace));
static_assert(!hasFlag(flagBits(Flag::Aligned), Flag::Bootstrap));

// tags
static_assert(makeTag(0, 0) != untaggedTag);
static_assert(!isTagged(untaggedTag) && isTagged(makeTag(0, 0)));
static_assert(tagTier(makeTag(15, 7)) == 15 && tagLife(makeTag(15, 7)) == 7);
static_assert(tagTier(makeTag(0, 7)) == 0 && tagLife(makeTag(15, 0)) == 0);
static_assert(tagTier(makeTag(2, 1)) == 2 && tagLife(makeTag(2, 1)) == 1);

// alloc aux
static_assert(allocAlignLog2(packAlloc(0, 0)) == 0);
static_assert(allocAlignLog2(packAlloc(63, auxLowMask)) == 63);
static_assert(allocSlack(packAlloc(63, auxLowMask)) == auxLowMask);
static_assert(allocSlack(packAlloc(12, 16)) == 16);
static_assert(allocAlignLog2(packAlloc(12, 16)) == 12);

// blob aux and sizing
static_assert(blobType(packBlob(BlobType::Stack, 256)) == BlobType::Stack);
static_assert(blobBytes(packBlob(BlobType::String, 0)) == 0);
static_assert(blobBytes(packBlob(BlobType::String, maxBlobBytes)) ==
              maxBlobBytes);
static_assert(slotsFor(0) == 1 && slotsFor(1) == 2 && slotsFor(40) == 2);
static_assert(slotsFor(41) == 3 && slotsFor(280) == 8);

// marker aux
static_assert(markerKind(packMarker(MarkerKind::LiveBytes)) ==
              MarkerKind::LiveBytes);

// chunk padding
static_assert(paddedBytes(0) == 0 && paddedBytes(1) == 8);
static_assert(paddedBytes(8) == 8 && paddedBytes(9) == 16);

// Volatile, so the calls happen at run time where contracts are checked.
volatile unsigned runtimeTier = 16;
volatile std::uint8_t runtimeTag = untaggedTag;
volatile std::size_t runtimeBytes = maxBlobBytes + 1;
volatile std::uint64_t runtimeSlack = std::uint64_t{1} << 40;

void checkSaturation() {
  std::uint32_t aux = packAlloc(3, runtimeSlack);
  check(allocSlack(aux) == auxLowMask && allocAlignLog2(aux) == 3, "slack",
        "2^40 B of slack saturates to %#x without touching alignLog2",
        allocSlack(aux));
}

void checkContracts() {
  if (!contractsAbort()) {
    skip("contract", "contracts not enforced in this build");
    return;
  }
  check(abortsInChild([] { static_cast<void>(makeTag(runtimeTier, 0)); }),
        "contract", "makeTag with tier 16 aborts");
  check(abortsInChild([] { static_cast<void>(tagTier(runtimeTag)); }),
        "contract", "tagTier of an untagged record aborts");
  check(abortsInChild(
            [] { static_cast<void>(packBlob(BlobType::Stack, runtimeBytes)); }),
        "contract", "packBlob past maxBlobBytes aborts");
}

} // namespace

int main() {
  checkSaturation();
  checkContracts();
  return ta::test::finish();
}
