// Fragmentation churn. Each phase allocates a fixed number of bytes of objects,
// then frees most of them in random order; the few survivors live on for a
// fixed number of phases, scattered among the pages the garbage has left.
//
// Object sizes come from a schedule of size bands, which is what makes the
// survivors cost memory. If every phase asked for the same sizes, the holes
// around survivors would simply be refilled and pinning would cost nothing.
// When the sizes change, a page pinned by a few survivors of an earlier band
// can't serve the current one (another size class, or holes too small), so an
// allocator that mixes survivors with garbage must hold pages for several
// bands at once. One that separates them reuses the garbage's memory for any
// size. Schedules:
//   cycle   band (p / band-phases) % bands: with the default bands, sizes grow
//           and then start again from the smallest (a sawtooth);
//   random  a band from the list for each band period, from the seed.
// A single band (--bands=16-4096) is the control: there hints shouldn't
// change RSS. Survivors must outlive a band, or they die before sizes change.
//
// Survival is decided when an object is allocated, as a program knows a type's
// lifetime, so with --hints=on the roles say so: garbage is Transient and
// survivors are Default. With --hints=off everything is Default, which is the
// same program without hints.
//
// Per phase p:
//   1. free the survivors born in phase p - survivor-phases;
//   2. allocate the phase's objects, writing every byte (a real program
//      writes what it allocates, and RSS needs the pages touched);
//   3. sample RSS (the phase's peak);
//   4. free the garbage in a random order;
//   5. keep the survivors, endPhase(), sample RSS (the phase's trough).
//
// Sizes, survival and free order come from a tape generated before the
// measured region, in mapped memory outside the allocator, so the region runs
// no RNG and the tape's memory is subtracted from heap RSS. The same seed gives
// every run the same tape.

#include "alloc/seam.hpp"
#include "harness/args.hpp"
#include "harness/mapped.hpp"
#include "harness/region.hpp"
#include "harness/report.hpp"
#include "harness/session.hpp"
#include "harness/summary.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace alloc = tabench::alloc;
using alloc::Role;

enum class Schedule : std::uint8_t { Cycle, Random };

// Indexed by Schedule; the static_assert catches a schedule without a name.
inline constexpr std::array<std::string_view, 2> scheduleNames{"cycle",
                                                               "random"};
static_assert(std::to_underlying(Schedule::Random) + 1 == scheduleNames.size());

std::optional<Schedule> parseSchedule(std::string_view text) noexcept {
  for (std::size_t i = 0; i < scheduleNames.size(); ++i)
    if (text == scheduleNames[i])
      return static_cast<Schedule>(i);
  return std::nullopt;
}

// Sizes in [min, max], log-uniform.
struct Band {
  std::size_t min;
  std::size_t max;
};

struct Config {
  std::size_t phases;
  std::size_t phaseBytes;     // allocated per phase
  double survivorFraction;    // of each phase's objects
  std::size_t survivorPhases; // how long a survivor lives
  std::vector<Band> bands;
  std::size_t bandPhases; // phases per band
  Schedule schedule;
  std::uint64_t seed;
  bool hints;
};

// Every request is a multiple of this, which the seam requires of sizes.
inline constexpr std::size_t objectAlign = 8;
// The tape stores sizes in 31 bits; the top bit marks a survivor.
inline constexpr std::uint32_t survivorBit = std::uint32_t{1} << 31;

std::size_t parseSize(std::string_view text, std::string_view bands) {
  const auto value = tabench::parseU64(text);
  if (!value)
    throw tabench::ArgError(std::format("--bands: '{}' in '{}': {}", text,
                                        bands, describe(value.error())));
  return static_cast<std::size_t>(*value);
}

// "16-64,64-256": each band "min-max" in bytes.
std::vector<Band> parseBands(std::string_view text) {
  std::vector<Band> bands;
  std::string_view rest = text;
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view band = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view{}
                                           : rest.substr(comma + 1);
    const std::size_t dash = band.find('-');
    if (dash == std::string_view::npos)
      throw tabench::ArgError(
          std::format("--bands: '{}' in '{}' is not min-max", band, text));
    const Band b{parseSize(band.substr(0, dash), text),
                 parseSize(band.substr(dash + 1), text)};
    if (b.min < objectAlign || b.min > b.max || b.max >= survivorBit)
      throw tabench::ArgError(
          std::format("--bands: '{}' must have 8 <= min <= max < 2 GiB", band));
    bands.push_back(b);
  }
  if (bands.empty() || bands.size() > std::numeric_limits<std::uint8_t>::max())
    throw tabench::ArgError("--bands must list between 1 and 255 bands");
  return bands;
}

Config parseConfig(tabench::Args &args) {
  Config config{};
  config.phases = args.getU64("phases", 32);
  config.phaseBytes = args.getU64("phase-bytes", std::uint64_t{16} << 20);
  config.survivorFraction = args.getDouble("survivors", 0.05);
  config.survivorPhases = args.getU64("survivor-phases", 8);
  config.bands =
      parseBands(args.getString("bands", "16-64,64-256,256-1024,1024-4096"));
  config.bandPhases = args.getU64("band-phases", 2);
  const std::string_view schedule = args.getString("schedule", "cycle");
  const std::optional<Schedule> parsed = parseSchedule(schedule);
  if (!parsed)
    throw tabench::ArgError(
        std::format("--schedule must be cycle or random (got '{}')", schedule));
  config.schedule = *parsed;
  config.seed = args.getU64("seed", 0x5eed);
  const std::string_view hints = args.getString("hints", "on");
  if (hints != "on" && hints != "off")
    throw tabench::ArgError(
        std::format("--hints must be on or off (got '{}')", hints));
  config.hints = hints == "on";

  auto require = [](bool ok, std::string_view what) {
    if (!ok)
      throw tabench::ArgError(std::string(what));
  };
  require(config.phases > 0, "--phases must be positive");
  require(config.phaseBytes > 0, "--phase-bytes must be positive");
  require(config.survivorFraction >= 0.0 && config.survivorFraction <= 1.0,
          "--survivors must be a fraction in [0, 1]");
  require(config.survivorPhases > 0, "--survivor-phases must be positive");
  require(config.bandPhases > 0, "--band-phases must be positive");
  // With one band nothing changes, so the control has no such requirement.
  require(config.bands.size() == 1 || config.survivorPhases > config.bandPhases,
          "--survivor-phases must exceed --band-phases, or survivors die "
          "before the sizes change");
  return config;
}

// The tape. Phase p's objects are [start[p], start[p + 1]) in sizes and order:
// allocated in index order; the first survivors[p] entries of the phase's
// order are its survivors, the rest the order the garbage is freed in.
struct Tape {
  std::vector<std::uint8_t> band; // per phase
  std::vector<std::size_t> start; // phases + 1 offsets
  std::vector<std::size_t> survivors;
  tabench::MappedArray<std::uint32_t> sizes; // survivorBit marks survivors
  tabench::MappedArray<std::uint32_t> order; // indices within the phase

  std::size_t objects(std::size_t p) const noexcept {
    return start[p + 1] - start[p];
  }
  std::size_t maxObjects() const noexcept {
    std::size_t most = 0;
    for (std::size_t p = 0; p + 1 < start.size(); ++p)
      most = std::max(most, objects(p));
    return most;
  }
  std::size_t maxSurvivors() const noexcept {
    return survivors.empty() ? 0 : std::ranges::max(survivors);
  }
  std::size_t bytes() const noexcept { return sizes.bytes() + order.bytes(); }
};

std::vector<std::uint8_t> makeSchedule(const Config &config) {
  std::vector<std::uint8_t> band(config.phases);
  std::mt19937_64 rng(config.seed ^ 0x5c4ed01e);
  std::uniform_int_distribution<std::size_t> pick(0, config.bands.size() - 1);
  std::size_t current = 0;
  for (std::size_t p = 0; p < config.phases; ++p) {
    const std::size_t period = p / config.bandPhases;
    if (p % config.bandPhases == 0)
      current = config.schedule == Schedule::Cycle
                    ? period % config.bands.size()
                    : pick(rng);
    band[p] = static_cast<std::uint8_t>(current);
  }
  return band;
}

// Draws one phase's sizes, log-uniform in its band and rounded up to
// objectAlign, until the phase's byte budget is spent; calls emit(size) for
// each. Both tape passes call this with an identically seeded rng.
template <class Emit>
void drawSizes(const Config &config, const Band &band, std::mt19937_64 &rng,
               Emit &&emit) {
  std::uniform_real_distribution<double> logSize(
      std::log(static_cast<double>(band.min)),
      std::log(static_cast<double>(band.max)));
  std::size_t total = 0;
  while (total < config.phaseBytes) {
    auto bytes = static_cast<std::size_t>(std::lround(std::exp(logSize(rng))));
    bytes = std::clamp(bytes, band.min, band.max);
    bytes = (bytes + objectAlign - 1) / objectAlign * objectAlign;
    emit(static_cast<std::uint32_t>(bytes));
    total += bytes;
  }
}

// Two passes over the same seeded sizes: the first counts each phase's
// objects, the second fills the mapped tape. Building it in a std::vector
// instead would go through the allocator under test before the region, and
// whatever that allocator kept of it would count as heap RSS.
Tape makeTape(const Config &config) {
  Tape tape;
  tape.band = makeSchedule(config);
  tape.start.assign(config.phases + 1, 0);
  tape.survivors.assign(config.phases, 0);
  std::mt19937_64 sizeRng(config.seed);
  for (std::size_t p = 0; p < config.phases; ++p) {
    std::size_t count = 0;
    drawSizes(config, config.bands[tape.band[p]], sizeRng,
              [&](std::uint32_t) { ++count; });
    tape.start[p + 1] = tape.start[p] + count;
    tape.survivors[p] = static_cast<std::size_t>(
        std::llround(config.survivorFraction * static_cast<double>(count)));
  }
  if (tape.maxObjects() > std::numeric_limits<std::uint32_t>::max())
    throw tabench::ArgError("a phase has more than 2^32 objects; lower "
                            "--phase-bytes or raise the bands' sizes");

  tape.sizes = tabench::MappedArray<std::uint32_t>(tape.start.back());
  tape.order = tabench::MappedArray<std::uint32_t>(tape.start.back());
  sizeRng.seed(config.seed);
  std::mt19937_64 orderRng(config.seed ^ 0x0de4);
  for (std::size_t p = 0; p < config.phases; ++p) {
    const std::size_t n = tape.objects(p);
    std::span<std::uint32_t> sizes =
        tape.sizes.span().subspan(tape.start[p], n);
    std::span<std::uint32_t> order =
        tape.order.span().subspan(tape.start[p], n);
    std::size_t i = 0;
    drawSizes(config, config.bands[tape.band[p]], sizeRng,
              [&](std::uint32_t size) { sizes[i++] = size; });
    std::iota(order.begin(), order.end(), std::uint32_t{0});
    std::ranges::shuffle(order, orderRng);
    for (std::size_t s = 0; s < tape.survivors[p]; ++s)
      sizes[order[s]] |= survivorBit;
  }
  return tape;
}

struct Block {
  void *p;
  std::uint32_t bytes;
  bool survivor;
};

// The roles the hints declare. Without hints, both are Default.
template <bool Survivor, bool Hints> constexpr Role roleFor() {
  if constexpr (!Hints || Survivor)
    return Role::Default;
  else
    return Role::Transient;
}

template <bool Hints>
[[gnu::always_inline]] inline void *allocateObject(std::size_t bytes,
                                                   bool survivor) {
  return survivor
             ? alloc::allocate<roleFor<true, Hints>()>(bytes, objectAlign)
             : alloc::allocate<roleFor<false, Hints>()>(bytes, objectAlign);
}

template <bool Hints>
[[gnu::always_inline]] inline void freeObject(const Block &b) noexcept {
  if (b.survivor)
    alloc::deallocate<roleFor<true, Hints>()>(b.p, b.bytes, objectAlign);
  else
    alloc::deallocate<roleFor<false, Hints>()>(b.p, b.bytes, objectAlign);
}

// The workload's own bookkeeping, mapped outside the allocator. Bucket b holds
// the survivors of the phases p with p % survivorPhases == b.
struct State {
  tabench::MappedArray<Block> phaseBlocks;     // maxObjects
  tabench::MappedArray<Block> survivorBuckets; // survivorPhases * maxSurvivors
  tabench::MappedArray<std::uint64_t> phaseTicks;
  std::size_t bucketSize;

  std::span<Block> bucket(std::size_t phase, std::size_t survivorPhases) {
    return survivorBuckets.span().subspan((phase % survivorPhases) * bucketSize,
                                          bucketSize);
  }
  std::size_t bytes() const noexcept {
    return phaseBlocks.bytes() + survivorBuckets.bytes() + phaseTicks.bytes();
  }
};

template <bool Hints> void freeBucket(std::span<Block> bucket) noexcept {
  for (Block &b : bucket) {
    if (b.p != nullptr)
      freeObject<Hints>(b);
    b = {};
  }
}

template <bool Hints>
void runPhases(const Config &config, const Tape &tape, State &state,
               tabench::Session &session) {
  for (std::size_t p = 0; p < config.phases; ++p) {
    const std::uint64_t start = tabench::readTicks();
    std::span<Block> bucket = state.bucket(p, config.survivorPhases);
    freeBucket<Hints>(bucket);

    const std::size_t n = tape.objects(p);
    const std::size_t kept = tape.survivors[p];
    const std::span<const std::uint32_t> sizes =
        tape.sizes.span().subspan(tape.start[p], n);
    const std::span<const std::uint32_t> order =
        tape.order.span().subspan(tape.start[p], n);
    for (std::size_t i = 0; i < n; ++i) {
      const bool survivor = (sizes[i] & survivorBit) != 0;
      const std::uint32_t bytes = sizes[i] & ~survivorBit;
      void *q = allocateObject<Hints>(bytes, survivor);
      std::memset(q, static_cast<int>(i & 0xff), bytes);
      state.phaseBlocks[i] = {q, bytes, survivor};
    }
    session.sampleRSS(alloc::seamStats.totalLiveBytes);

    for (std::size_t j = kept; j < n; ++j)
      freeObject<Hints>(state.phaseBlocks[order[j]]);
    for (std::size_t j = 0; j < kept; ++j)
      bucket[j] = state.phaseBlocks[order[j]];

    alloc::endPhase();
    session.sampleRSS(alloc::seamStats.totalLiveBytes);
    state.phaseTicks[p] = tabench::readTicks() - start;
  }
}

tabench::Json metricsJson(const Config &config, const Tape &tape,
                          const State &state, const tabench::Session &session,
                          const alloc::SeamRegion &seam,
                          std::uint64_t liveBytesEnd,
                          std::uint64_t leakedBytes) {
  // Every phase frees its garbage; phase p also frees the survivors of phase
  // p - survivorPhases, so the last survivorPhases phases' survivors are still
  // live when the region ends.
  std::uint64_t frees = 0;
  tabench::Json phases = tabench::Json::array();
  std::vector<double> phaseMs;
  for (std::size_t p = 0; p < config.phases; ++p) {
    frees += tape.objects(p) - tape.survivors[p];
    if (p >= config.survivorPhases)
      frees += tape.survivors[p - config.survivorPhases];
    const double ms = session.clockInfo().toNs(state.phaseTicks[p]) / 1e6;
    phaseMs.push_back(ms);
    tabench::Json phase;
    phase["band"] = tape.band[p];
    phase["objects"] = tape.objects(p);
    phase["survivors"] = tape.survivors[p];
    phase["ms"] = ms;
    phases.push_back(phase);
  }
  const std::uint64_t allocations = tape.start.back();

  tabench::Json j;
  j["allocations"] = allocations;
  j["frees"] = frees;
  j["nsPerOp"] = session.region().ns(session.clockInfo()) /
                 static_cast<double>(allocations + frees);
  j["phaseMs"] = tabench::toJson(tabench::summarize(phaseMs));
  j["liveBytesEnd"] = liveBytesEnd;
  // One entry per phase; its two RSS samples (peak, then trough) are
  // memory.samples[2p] and [2p + 1].
  j["phases"] = std::move(phases);
  j["seam"] = alloc::seamJson(seam, leakedBytes);
  return j;
}

template <bool Hints>
void run(const Config &config, tabench::Session &session) {
  const Tape tape = makeTape(config);
  State state{
      tabench::MappedArray<Block>(tape.maxObjects()),
      tabench::MappedArray<Block>(config.survivorPhases * tape.maxSurvivors()),
      tabench::MappedArray<std::uint64_t>(config.phases), tape.maxSurvivors()};
  session.addInputBytes(tape.bytes() + state.bytes());

  // The workload measures the heap from empty, so its only warmup is the
  // CPU's: a warm heap would hide the growth churn is about.
  session.warmUpCPU();

  alloc::SeamRegion seam;
  std::uint64_t liveBytesEnd = 0;
  session.measure([&] {
    seam.begin = alloc::seamStats.beginRegion();
    runPhases<Hints>(config, tape, state, session);
    seam.end = alloc::seamStats;
    liveBytesEnd = alloc::seamStats.totalLiveBytes;
  });
  freeBucket<Hints>(state.survivorBuckets.span());

  // A single band never changes, whatever the schedule: that's the control.
  tabench::Json record =
      session.record(config.bands.size() == 1
                         ? std::string_view("control")
                         : scheduleNames[std::to_underlying(config.schedule)]);
  record["metrics"] =
      metricsJson(config, tape, state, session, seam, liveBytesEnd,
                  alloc::seamStats.totalLiveBytes);
  session.write(record);
}

} // namespace

int main(int argc, char **argv) {
  return tabench::Session::run("churn", [&] {
    tabench::Session session(argc, argv, "churn");
    const Config config = parseConfig(session.args());
    session.start();
    if (config.hints)
      run<true>(config, session);
    else
      run<false>(config, session);
  });
}
