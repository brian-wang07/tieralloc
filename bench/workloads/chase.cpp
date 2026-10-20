// Pointer chase. Hot nodes are allocated interleaved with cold payloads, then
// only the hot nodes are traversed, in a random cyclic order that defeats the
// prefetchers, so every step's cost is the latency of wherever the next node
// landed. An allocator that packs the hot nodes together keeps the hot set in
// fewer cache lines and pages; one that places by allocation order spreads it
// over the payloads too.
//
// Variants, by size:
//   same-class  --cold-bytes == --hot-bytes: nodes and payloads share a size
//               class, so only hints can separate them;
//   split       otherwise: size classes already separate them in jemalloc and
//               mimalloc, the control (not for glibc, which has no size
//               classes for these sizes).
//
// Roles with --hints=on: nodes Hot, payloads Cold; with --hints=off, Default.
// The payloads stay live for the whole run and are written once, never read.

#include "alloc/seam.hpp"
#include "harness/args.hpp"
#include "harness/mapped.hpp"
#include "harness/perf.hpp"
#include "harness/region.hpp"
#include "harness/report.hpp"
#include "harness/session.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace alloc = tabench::alloc;
using alloc::Role;

struct Config {
  std::size_t nodes;
  std::size_t hotBytes;   // per node, at least sizeof(Node)
  std::size_t coldPerHot; // payloads allocated after each node
  std::size_t coldBytes;  // per payload
  std::size_t laps;       // measured traversals of the whole cycle
  std::uint64_t seed;
  bool hints;
};

// The part of a node the traversal reads; the rest of hotBytes is padding a
// real node would fill with other fields.
struct Node {
  Node *next;
  std::uint64_t value;
};

inline constexpr std::size_t objectAlign = 8;

Config parseConfig(tabench::Args &args) {
  Config config{};
  config.nodes = args.getU64("nodes", 262'144);
  config.hotBytes = args.getU64("hot-bytes", 32);
  config.coldPerHot = args.getU64("cold-per-hot", 3);
  config.coldBytes = args.getU64("cold-bytes", 32);
  config.laps = args.getU64("laps", 20);
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
  require(config.nodes > 1, "--nodes must be at least 2");
  require(config.hotBytes >= sizeof(Node) && config.hotBytes % objectAlign == 0,
          "--hot-bytes must be a multiple of 8, at least 16");
  require(config.coldBytes >= objectAlign &&
              config.coldBytes % objectAlign == 0,
          "--cold-bytes must be a positive multiple of 8");
  require(config.laps > 0, "--laps must be positive");
  return config;
}

template <bool Hints> struct Roles {
  static constexpr Role hot = Hints ? Role::Hot : Role::Default;
  static constexpr Role cold = Hints ? Role::Cold : Role::Default;
};

// The structure, built before the region. Pointers are kept in mapped arrays
// so the bookkeeping never goes through the allocator under test.
template <bool Hints> struct Graph {
  tabench::MappedArray<Node *> nodes;
  tabench::MappedArray<void *> payloads;

  Graph(const Config &config)
      : nodes(config.nodes), payloads(config.nodes * config.coldPerHot) {
    using R = Roles<Hints>;
    for (std::size_t i = 0; i < config.nodes; ++i) {
      void *raw = alloc::allocate<R::hot>(config.hotBytes, objectAlign);
      std::memset(raw, 0, config.hotBytes);
      nodes[i] = static_cast<Node *>(raw);
      for (std::size_t c = 0; c < config.coldPerHot; ++c) {
        void *payload = alloc::allocate<R::cold>(config.coldBytes, objectAlign);
        std::memset(payload, static_cast<int>(c), config.coldBytes);
        payloads[i * config.coldPerHot + c] = payload;
      }
    }
    // One random cycle through every node, so the traversal visits each once
    // per lap in an order no prefetcher can follow.
    tabench::MappedArray<std::uint32_t> order(config.nodes);
    std::iota(order.span().begin(), order.span().end(), std::uint32_t{0});
    std::mt19937_64 rng(config.seed);
    std::ranges::shuffle(order.span(), rng);
    for (std::size_t i = 0; i < config.nodes; ++i) {
      Node *n = nodes[order[i]];
      n->next = nodes[order[(i + 1) % config.nodes]];
      n->value = i;
    }
  }

  void release(const Config &config) noexcept {
    using R = Roles<Hints>;
    for (Node *n : nodes.span())
      alloc::deallocate<R::hot>(n, config.hotBytes, objectAlign);
    for (void *p : payloads.span())
      alloc::deallocate<R::cold>(p, config.coldBytes, objectAlign);
  }
};

// Follows next for steps nodes; the sum keeps every load on the chain live.
[[gnu::noinline]] std::uint64_t walk(const Node *start, std::size_t steps) {
  std::uint64_t sum = 0;
  const Node *n = start;
  for (std::size_t s = 0; s < steps; ++s) {
    sum += n->value;
    n = n->next;
  }
  tabench::doNotOptimize(n);
  return sum;
}

template <bool Hints>
void run(const Config &config, tabench::Session &session) {
  Graph<Hints> graph(config);
  session.addInputBytes(graph.nodes.bytes() + graph.payloads.bytes());

  const std::size_t steps = config.laps * config.nodes;
  // One untimed lap to fill the caches and TLB as far as they go.
  std::uint64_t sum = walk(graph.nodes[0], config.nodes);
  session.warmUpCPU();
  alloc::SeamRegion seam;
  session.measure([&] {
    seam.begin = alloc::seamStats.beginRegion();
    sum += walk(graph.nodes[0], steps);
    seam.end = alloc::seamStats;
  });
  tabench::doNotOptimize(sum);
  // Each lap visits every node once: the sum of 0..nodes-1, laps + 1 times.
  const std::uint64_t expected =
      (config.laps + 1) *
      (std::uint64_t{config.nodes} * (config.nodes - 1) / 2);
  if (sum != expected)
    throw std::runtime_error("the traversal did not visit every node once "
                             "per lap");
  graph.release(config);

  const std::string_view variant =
      config.coldBytes == config.hotBytes ? "same-class" : "split";
  tabench::Json record = session.record(variant);
  tabench::Json &m = record["metrics"];
  m["visits"] = steps;
  m["nsPerVisit"] =
      session.region().ns(session.clockInfo()) / static_cast<double>(steps);
  const std::optional<tabench::CounterResult> counted =
      session.region().counters();
  m["cyclesPerVisit"] =
      counted && counted->status == tabench::CounterStatus::Ok
          ? tabench::Json(
                static_cast<double>(counted->value(tabench::Counter::Cycles)) /
                static_cast<double>(steps))
          : tabench::Json(nullptr);
  m["hotSetBytes"] = config.nodes * config.hotBytes;
  m["coldSetBytes"] = config.nodes * config.coldPerHot * config.coldBytes;
  m["seam"] = alloc::seamJson(seam, alloc::seamStats.totalLiveBytes);
  session.write(record);
}

} // namespace

int main(int argc, char **argv) {
  return tabench::Session::run("chase", [&] {
    tabench::Session session(argc, argv, "chase");
    const Config config = parseConfig(session.args());
    session.start();
    if (config.hints)
      run<true>(config, session);
    else
      run<false>(config, session);
  });
}
