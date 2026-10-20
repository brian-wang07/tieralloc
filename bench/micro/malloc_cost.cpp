// benchmarks allocation + free latency for a given allocator (under LD_PRELOAD)
// and a set of parameters.

#include "harness/args.hpp"
#include "harness/clock.hpp"
#include "harness/counters.hpp"
#include "harness/memstats.hpp"
#include "harness/region.hpp"
#include "harness/report.hpp"
#include "harness/summary.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
enum class Pattern : std::uint8_t { Pair, BurstLifo, BurstFifo, Churn };

// Indexed by Pattern, so the order must match the enum; the static_assert
// catches a pattern added without a name. string_view: the names only feed the
// record and std::format.
inline constexpr std::array<std::string_view, 4> patternNames{
    "pair", "burst-lifo", "burst-fifo", "churn"};
static_assert(std::to_underlying(Pattern::Churn) + 1 == patternNames.size());

std::string_view toString(Pattern pattern) noexcept {
  return patternNames[std::to_underlying(pattern)];
}

std::optional<Pattern> parsePattern(std::string_view text) noexcept {
  for (std::size_t i = 0; i < patternNames.size(); ++i) {
    if (text == patternNames[i])
      return static_cast<Pattern>(i);
  }
  return std::nullopt;
}

struct Config {
  Pattern pattern;
  std::size_t size = 0;
  std::size_t minSize = 0;
  std::size_t maxSize = 0;
  std::size_t burst = 0;
  std::size_t live = 0;
  std::size_t opsPerBatch;
  std::size_t batches;
  std::size_t warmup;
  std::uint64_t seed = 0;
  std::string_view out;
  tabench::CounterMode counters;
  tabench::TableChoice table;
};

std::size_t getSize(tabench::Args &args, std::string_view key,
                    std::size_t defaultValue) {
  return static_cast<std::size_t>(args.getU64(key, defaultValue));
}

Config parseConfig(tabench::Args &args) {
  Config config{};
  const std::string_view name = args.getString("pattern", "");
  const std::optional<Pattern> pattern = parsePattern(name);
  if (!pattern) {
    std::string names;
    for (std::string_view n : patternNames)
      names += std::format("{}{}", names.empty() ? "" : ", ", n);
    throw tabench::ArgError(
        std::format("--pattern must be one of {} (got '{}')", names, name));
  }
  config.pattern = *pattern;

  switch (config.pattern) {
  case Pattern::Pair:
    config.size = getSize(args, "size", 64);
    break;
  case Pattern::BurstLifo:
  case Pattern::BurstFifo:
    config.size = getSize(args, "size", 64);
    config.burst = getSize(args, "burst", 256);
    break;
  case Pattern::Churn:
    config.minSize = getSize(args, "min-size", 16);
    config.maxSize = getSize(args, "max-size", 1024);
    config.live = getSize(args, "live", 10000);
    config.seed = args.getU64("seed", 0x5eed);
    break;
  }

  config.opsPerBatch = getSize(args, "ops-per-batch", 1024);
  config.batches = getSize(args, "batches", 1000);
  config.warmup = getSize(args, "warmup", 10);
  config.out = args.getString("out", "");

  const std::string_view counters = args.getString("counters", "on");
  const std::optional<tabench::CounterMode> mode =
      tabench::parseCounterMode(counters);
  if (!mode)
    throw tabench::ArgError(std::format(
        "--counters must be off, on or required (got '{}')", counters));
  config.counters = *mode;
  const std::string_view table = args.getString("counter-table", "auto");
  const std::optional<tabench::TableChoice> choice =
      tabench::parseTableChoice(table);
  if (!choice)
    throw tabench::ArgError(std::format(
        "--counter-table must be auto, raw or generic (got '{}')", table));
  config.table = *choice;

  // The touch writes one word, and the churn plan stores sizes and victims as
  // 32-bit values.
  constexpr std::size_t minBytes = sizeof(std::uintptr_t);
  constexpr std::size_t maxPlanValue =
      std::numeric_limits<std::uint32_t>::max();
  auto require = [](bool ok, std::string_view what) {
    if (!ok)
      throw tabench::ArgError(std::string(what));
  };
  switch (config.pattern) {
  case Pattern::Pair:
    require(config.size >= minBytes, "--size must be at least 8");
    break;
  case Pattern::BurstLifo:
  case Pattern::BurstFifo:
    require(config.size >= minBytes, "--size must be at least 8");
    require(config.burst > 0, "--burst must be positive");
    require(config.opsPerBatch % config.burst == 0,
            "--ops-per-batch must be a multiple of --burst");
    break;
  case Pattern::Churn:
    require(config.minSize >= minBytes, "--min-size must be at least 8");
    require(config.minSize <= config.maxSize,
            "--min-size must not exceed --max-size");
    require(config.maxSize <= maxPlanValue, "--max-size must fit in 32 bits");
    require(config.live > 0, "--live must be positive");
    require(config.live <= maxPlanValue, "--live must fit in 32 bits");
    break;
  }
  require(config.opsPerBatch > 0, "--ops-per-batch must be positive");
  require(config.batches > 0, "--batches must be positive");
  return config;
}

// Churn's random choices, made before timing. Indexed with
// opIndex & (churnPlanLength - 1), so its size and cache footprint don't grow
// with the run: 2 x 4 x 2^16 bytes = 512 KiB, read sequentially.
struct ChurnPlan {
  std::vector<std::uint32_t> sizes;
  std::vector<std::uint32_t> victims;
};

inline constexpr std::size_t churnPlanLength = std::size_t{1} << 16;
static_assert(std::has_single_bit(churnPlanLength));

// Sizes are log-uniform, so 16-32 B blocks are as common as 512-1024 B ones,
// as in real programs; uniform sizes would be dominated by large blocks.
ChurnPlan makeChurnPlan(const Config &config) {
  std::mt19937_64 rng(config.seed);
  std::uniform_real_distribution<double> logSize(
      std::log(static_cast<double>(config.minSize)),
      std::log(static_cast<double>(config.maxSize)));
  std::uniform_int_distribution<std::uint32_t> victim(
      0, static_cast<std::uint32_t>(config.live - 1));
  ChurnPlan plan;
  plan.sizes.resize(churnPlanLength);
  plan.victims.resize(churnPlanLength);
  for (std::size_t i = 0; i < churnPlanLength; ++i) {
    // Clamped, so rounding can't step just outside the range.
    const auto size =
        static_cast<std::size_t>(std::lround(std::exp(logSize(rng))));
    plan.sizes[i] = static_cast<std::uint32_t>(
        std::clamp(size, config.minSize, config.maxSize));
    plan.victims[i] = victim(rng);
  }
  return plan;
}

struct Block {
  void *p;
  std::size_t bytes;
};

// the only two places that touch the allocator.doNotOptimize's memory clobber
// keeps both the store and the new/delete pair from being elided.
[[gnu::always_inline]] inline void *allocate(std::size_t bytes) {
  void *p = ::operator new(bytes);
  *static_cast<std::uintptr_t *>(p) = bytes;
  tabench::doNotOptimize(p);
  return p;
}

[[gnu::always_inline]] inline void release(void *p,
                                           std::size_t bytes) noexcept {
  ::operator delete(p, bytes);
}

// the kernels are noinline so each is its own symbol, for checking with
// objdump that new and delete are really called. Each fills ticks with one
// entry per batch
[[gnu::noinline]] void runPair(const Config &config,
                               std::span<std::uint64_t> ticks) {
  const std::size_t size = config.size;
  tabench::timeBatches([&](std::size_t) { release(allocate(size), size); },
                       config.opsPerBatch, ticks, 0);
}

[[gnu::noinline]] void runBurst(const Config &config, bool lifo,
                                std::span<Block> slots,
                                std::span<std::uint64_t> ticks) {
  const std::size_t size = config.size;
  tabench::timeBatches(
      [&](std::size_t) {
        for (Block &b : slots)
          b.p = allocate(size);
        if (lifo) {
          for (std::size_t i = slots.size(); i-- > 0;)
            release(slots[i].p, size);
        } else {
          for (const Block &b : slots)
            release(b.p, size);
        }
      },
      config.opsPerBatch / config.burst, ticks, 0);
  // Every burst frees what it allocated, so the slots hold only freed
  // pointers; clear them (untimed) so freeBlocks() can't free them again.
  std::ranges::fill(slots, Block{});
}

[[gnu::noinline]] void runChurn(const Config &config, const ChurnPlan &plan,
                                std::span<Block> liveSet,
                                std::span<std::uint64_t> ticks) {
  tabench::timeBatches(
      [&](std::size_t opIndex) {
        const std::size_t i = opIndex & (churnPlanLength - 1);
        Block &victim = liveSet[plan.victims[i]];
        release(victim.p, victim.bytes);
        victim.bytes = plan.sizes[i];
        victim.p = allocate(victim.bytes);
      },
      config.opsPerBatch, ticks, 0);
}

// before warmup, untimed.
void fillLiveSet(const ChurnPlan &plan, std::span<Block> liveSet) {
  for (std::size_t j = 0; j < liveSet.size(); ++j) {
    const std::size_t bytes = plan.sizes[j & (churnPlanLength - 1)];
    liveSet[j] = {allocate(bytes), bytes};
  }
}

void freeBlocks(std::span<Block> blocks) noexcept {
  for (Block &b : blocks) {
    if (b.p != nullptr)
      release(b.p, b.bytes);
    b = {};
  }
}

// blocks is the burst's slots or churn's live set; pair ignores it.
void runPattern(const Config &config, const ChurnPlan &plan,
                std::span<Block> blocks, std::span<std::uint64_t> ticks) {
  switch (config.pattern) {
  case Pattern::Pair:
    runPair(config, ticks);
    return;
  case Pattern::BurstLifo:
  case Pattern::BurstFifo:
    runBurst(config, config.pattern == Pattern::BurstLifo, blocks, ticks);
    return;
  case Pattern::Churn:
    runChurn(config, plan, blocks, ticks);
    return;
  }
}

// ns per op for each batch, ready for summarize(). Takes the recalibrated
// clock, so call it after recalibrate().
std::vector<double> nsPerOp(std::span<const std::uint64_t> ticks,
                            std::size_t opsPerBatch,
                            const tabench::ClockInfo &clock) {
  std::vector<double> ns;
  ns.reserve(ticks.size());
  for (std::uint64_t t : ticks)
    ns.push_back(clock.toNs(t) / static_cast<double>(opsPerBatch));
  return ns;
}

tabench::Json metricsJson(const Config &config, const tabench::Summary &ns) {
  tabench::Json j;
  j["ops"] = config.batches * config.opsPerBatch;
  j["touch"] = "word";
  j["nsPerOp"] = tabench::toJson(ns);
  return j;
}

} // namespace

int main(int argc, char **argv) {
  // before anything allocates: the baseline the summary subtracts.
  const tabench::MemoryStats startup = tabench::readMemoryStats();
  try {
    tabench::Args args(argc, argv);
    const Config config = parseConfig(args);
    args.checkAllUsed();

    tabench::ClockInfo clock = tabench::calibrate();
    const tabench::TimerOverhead overhead = tabench::measureTimerOverhead();
    // regions hold a pointer into counters, so it stays put from here on.
    tabench::Counters counters =
        tabench::openCounters(config.counters, config.table);

    // everything the timed loops read is built here, untimed.
    const bool churn = config.pattern == Pattern::Churn;
    const ChurnPlan plan = churn ? makeChurnPlan(config) : ChurnPlan{};
    std::vector<Block> blocks(churn ? config.live : config.burst);
    if (churn)
      fillLiveSet(plan, blocks);
    std::vector<std::uint64_t> warmupTicks(config.warmup);
    std::vector<std::uint64_t> ticks(config.batches);

    // outside the region, so its counters cover exactly the measured ops.
    if (config.warmup > 0)
      runPattern(config, plan, blocks, warmupTicks);

    tabench::MemoryReport memory;
    memory.startup = startup;
    memory.peakReset = tabench::resetPeakRSS();
    memory.before = tabench::readMemoryStats();
    tabench::Region region(counters.get());
    region.begin();
    runPattern(config, plan, blocks, ticks);
    region.end();
    memory.after = tabench::readMemoryStats();
    tabench::recalibrate(clock);
    // after the snapshot, so memory shows the steady state with the live set.
    freeBlocks(blocks);

    std::vector<double> ns = nsPerOp(ticks, config.opsPerBatch, clock);
    const tabench::Summary summary = tabench::summarize(ns);
    tabench::Json record =
        tabench::makeRecord("malloc-cost", toString(config.pattern), clock,
                            overhead, counters, region.result(), memory, args);
    record["metrics"] = metricsJson(config, summary);
    tabench::writeRecord(record, config.out);
    return 0;
  } catch (const tabench::ArgError &e) {
    std::fprintf(stderr, "malloc-cost: %s\n", e.what());
    return 2;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "malloc-cost: %s\n", e.what());
    return 1;
  }
}
