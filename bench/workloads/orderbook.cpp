// Order book replay. A stream of ITCH-like messages (add, execute, cancel,
// delete, replace) updates a limit order book, as a market-data handler does.
// It tests the hotness half of the hypothesis: the orders, price levels and
// index nodes every message touches are hot, and each order is allocated next
// to a cold metadata block (--cold-bytes) that the hot path never reads.
// Without hints, an allocator that places by size interleaves them, spreading
// the hot set over pages of cold data; when --cold-bytes is close to
// sizeof(Order) they share a size class and only hints can separate them. A
// large --cold-bytes is the control, where size classes already do.
//
// Roles with --hints=on: orders, levels and index nodes Hot; order metadata
// and fills Cold; the per-message parse buffer Transient, with endPhase() every
// --phase-messages (where the fill log is flushed too). With --hints=off,
// everything is Default.
//
// The tape is generated before the region, in mapped memory outside the
// allocator. Messages reference orders by id, as ITCH does, so the generator
// needs no matching engine: it tracks the live orders and only references
// those. The first --orders messages build the book with adds, then
// --warmup-messages of the normal mix run untimed; the rest are measured, each
// message timed on its own, so p99 is per message. Executes are drawn near the
// mid price; the book may cross as the mid drifts, which matters here only in
// that nothing is matched.

#include "alloc/seam.hpp"
#include "harness/args.hpp"
#include "harness/mapped.hpp"
#include "harness/perf.hpp"
#include "harness/region.hpp"
#include "harness/report.hpp"
#include "harness/session.hpp"
#include "harness/summary.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace alloc = tabench::alloc;
using alloc::Role;

enum class Type : std::uint8_t { Add, Execute, Cancel, Delete, Replace };
inline constexpr std::array<std::string_view, 5> typeNames{
    "add", "execute", "cancel", "delete", "replace"};
static_assert(std::to_underlying(Type::Replace) + 1 == typeNames.size());

enum class Side : std::uint8_t { Buy, Sell };

// One message. qty is the new order's size for add and replace, and the amount
// executed or cancelled otherwise; price and newId are only for add/replace.
struct Message {
  std::uint64_t id;
  std::uint64_t newId;
  std::int32_t price;
  std::uint32_t qty;
  Type type;
  Side side;
};

struct Config {
  std::size_t orders;         // live orders the book hovers around
  std::size_t messages;       // measured
  std::size_t warmupMessages; // normal mix, after the build-up, untimed
  std::size_t phaseMessages;  // between endPhase() and fill flushes
  std::size_t sampleMessages; // between RSS samples
  std::size_t coldBytes;      // per order's metadata
  double meanDepth;           // ticks from the mid, geometric
  double midMove;             // probability the mid moves per message
  std::uint64_t seed;
  bool hints;
};

inline constexpr std::size_t coldAlign = 8;

struct Level;

struct Order {
  std::uint64_t id;
  Order *prev;
  Order *next;
  Level *level;
  void *meta; // cold; written at add, freed with the order, never read
  std::int32_t price;
  std::uint32_t qty;
  Side side;
};

struct Level {
  std::int32_t price;
  std::uint32_t count;
  std::uint64_t qty;
  Order *head;
  Order *tail;
};

struct Fill {
  std::uint64_t orderId;
  std::uint64_t sequence;
  std::int32_t price;
  std::uint32_t qty;
};

Config parseConfig(tabench::Args &args) {
  Config config{};
  config.orders = args.getU64("orders", 200'000);
  config.messages = args.getU64("messages", 2'000'000);
  config.warmupMessages = args.getU64("warmup-messages", 200'000);
  config.phaseMessages = args.getU64("phase-messages", 10'000);
  config.sampleMessages = args.getU64("sample-messages", 50'000);
  config.coldBytes = args.getU64("cold-bytes", sizeof(Order));
  config.meanDepth = args.getDouble("mean-depth", 8.0);
  config.midMove = args.getDouble("mid-move", 0.1);
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
  require(config.orders > 0, "--orders must be positive");
  require(config.messages > 0, "--messages must be positive");
  require(config.phaseMessages > 0, "--phase-messages must be positive");
  require(config.sampleMessages > 0, "--sample-messages must be positive");
  require(config.coldBytes >= coldAlign && config.coldBytes % coldAlign == 0,
          "--cold-bytes must be a positive multiple of 8");
  require(config.meanDepth >= 1.0, "--mean-depth must be at least 1");
  require(config.midMove >= 0.0 && config.midMove <= 1.0,
          "--mid-move must be a probability");
  return config;
}

// The live orders as the generator sees them: a dense list for uniform picks,
// and per-id arrays (ids are sequential) for the rest. All mapped, so the
// allocator under test sees nothing before the region; unmapped once the tape
// is built.
struct Generator {
  tabench::MappedArray<std::uint64_t> live;
  tabench::MappedArray<std::uint32_t> position;
  tabench::MappedArray<std::int32_t> price;
  tabench::MappedArray<std::uint32_t> qty;
  tabench::MappedArray<Side> side;
  std::size_t liveCount = 0;
  std::uint64_t nextId = 1;

  explicit Generator(std::size_t maxIds)
      : live(maxIds), position(maxIds), price(maxIds), qty(maxIds),
        side(maxIds) {}

  std::uint64_t add(Side s, std::int32_t p, std::uint32_t q) {
    const std::uint64_t id = nextId++;
    price[id] = p;
    qty[id] = q;
    side[id] = s;
    position[id] = static_cast<std::uint32_t>(liveCount);
    live[liveCount++] = id;
    return id;
  }

  void remove(std::uint64_t id) {
    const std::uint32_t at = position[id];
    const std::uint64_t last = live[--liveCount];
    live[at] = last;
    position[last] = at;
  }
};

struct Tape {
  tabench::MappedArray<Message> messages;
  std::size_t buildMessages; // the adds that build the book
  std::size_t liveAtEnd;
  std::array<std::uint64_t, typeNames.size()> mix{}; // measured messages

  std::size_t firstMeasured(const Config &config) const noexcept {
    return buildMessages + config.warmupMessages;
  }
};

Tape makeTape(const Config &config) {
  const std::size_t total =
      config.orders + config.warmupMessages + config.messages;
  // Every message adds at most one id; id 0 is unused.
  Generator gen(total + 1);
  Tape tape{tabench::MappedArray<Message>(total), config.orders, 0, {}};

  std::mt19937_64 rng(config.seed);
  std::geometric_distribution<std::int32_t> depth(1.0 / config.meanDepth);
  std::uniform_int_distribution<std::uint32_t> size(1, 100);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::int32_t mid = 100'000;

  auto newOrderPrice = [&](Side s) {
    const std::int32_t offset = 1 + depth(rng);
    return s == Side::Buy ? mid - offset : mid + offset;
  };
  auto randomLive = [&] {
    std::uniform_int_distribution<std::size_t> pick(0, gen.liveCount - 1);
    return gen.live[pick(rng)];
  };
  // Executions happen at the top of the book: the closest to the mid of a
  // few random live orders.
  auto nearTop = [&] {
    std::uint64_t best = randomLive();
    for (int i = 0; i < 3; ++i) {
      const std::uint64_t candidate = randomLive();
      if (std::abs(gen.price[candidate] - mid) <
          std::abs(gen.price[best] - mid))
        best = candidate;
    }
    return best;
  };

  for (std::size_t i = 0; i < total; ++i) {
    Message &m = tape.messages[i];
    if (unit(rng) < config.midMove)
      mid += unit(rng) < 0.5 ? -1 : 1;

    Type type = Type::Add;
    if (i >= tape.buildMessages && gen.liveCount > 0) {
      // Cancel-heavy, as real feeds are; the add/delete balance tilts to keep
      // the book near --orders live.
      const double fill = static_cast<double>(gen.liveCount) /
                          static_cast<double>(config.orders);
      // Base mix: 45% add, 35% delete, 10% cancel, 5% execute, 5% replace.
      // Adds and deletes share the first 80%, split by how full the book is.
      const double addEdge = fill < 0.98 ? 0.55 : fill > 1.02 ? 0.35 : 0.45;
      const double r = unit(rng);
      type = r < addEdge ? Type::Add
             : r < 0.80  ? Type::Delete
             : r < 0.90  ? Type::Cancel
             : r < 0.95  ? Type::Execute
                         : Type::Replace;
    }

    switch (type) {
    case Type::Add: {
      const Side s = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
      const std::int32_t p = newOrderPrice(s);
      const std::uint32_t q = size(rng);
      m = {gen.add(s, p, q), 0, p, q, Type::Add, s};
      break;
    }
    case Type::Execute: {
      const std::uint64_t id = nearTop();
      const std::uint32_t q =
          std::uniform_int_distribution<std::uint32_t>(1, gen.qty[id])(rng);
      m = {id, 0, 0, q, Type::Execute, gen.side[id]};
      if (q == gen.qty[id])
        gen.remove(id);
      else
        gen.qty[id] -= q;
      break;
    }
    case Type::Cancel: {
      const std::uint64_t id = randomLive();
      if (gen.qty[id] <= 1) {
        m = {id, 0, 0, 0, Type::Delete, gen.side[id]};
        gen.remove(id);
        break;
      }
      const std::uint32_t q =
          std::uniform_int_distribution<std::uint32_t>(1, gen.qty[id] - 1)(rng);
      m = {id, 0, 0, q, Type::Cancel, gen.side[id]};
      gen.qty[id] -= q;
      break;
    }
    case Type::Delete: {
      const std::uint64_t id = randomLive();
      m = {id, 0, 0, 0, Type::Delete, gen.side[id]};
      gen.remove(id);
      break;
    }
    case Type::Replace: {
      const std::uint64_t id = randomLive();
      const Side s = gen.side[id];
      gen.remove(id);
      const std::int32_t p = newOrderPrice(s);
      const std::uint32_t q = size(rng);
      m = {id, gen.add(s, p, q), p, q, Type::Replace, s};
      break;
    }
    }
    if (i >= tape.firstMeasured(config))
      ++tape.mix[std::to_underlying(m.type)];
  }
  tape.liveAtEnd = gen.liveCount;
  return tape;
}

template <bool Hints> struct Roles {
  static constexpr Role hot = Hints ? Role::Hot : Role::Default;
  static constexpr Role cold = Hints ? Role::Cold : Role::Default;
  static constexpr Role transient = Hints ? Role::Transient : Role::Default;
};

template <bool Hints> class Book {
  using R = Roles<Hints>;
  using Bids = alloc::Map<std::int32_t, Level, R::hot, std::greater<>>;
  using Asks = alloc::Map<std::int32_t, Level, R::hot, std::less<>>;
  using Index = alloc::UnorderedMap<std::uint64_t, Order *, R::hot>;

public:
  Book(const Config &config, std::span<Fill *> fillLog)
      : coldBytes(config.coldBytes), fills(fillLog) {
    // Before the region: a rehash in the middle would allocate a large bucket
    // array, and one such spike would dominate the p99.
    index.reserve(config.orders * 2);
  }

  Book(const Book &) = delete;
  Book &operator=(const Book &) = delete;
  ~Book() { clear(); }

  [[gnu::always_inline]] void apply(const Message &m) {
    switch (m.type) {
    case Type::Add:
      add(m.id, m.side, m.price, m.qty);
      return;
    case Type::Execute: {
      Order *o = find(m.id);
      fills[fillCount++] =
          alloc::make<Fill, R::cold>(Fill{o->id, ++sequence, o->price, m.qty});
      reduce(o, m.qty);
      return;
    }
    case Type::Cancel:
      reduce(find(m.id), m.qty);
      return;
    case Type::Delete:
      remove(find(m.id));
      return;
    case Type::Replace:
      remove(find(m.id));
      add(m.newId, m.side, m.price, m.qty);
      return;
    }
  }

  // The fills are written once and never read in the hot path; freed in
  // batches, as a handler hands them on.
  void flushFills() noexcept {
    for (Fill *f : fills.first(fillCount))
      alloc::destroy<Fill, R::cold>(f);
    fillCount = 0;
  }

  void clear() noexcept {
    flushFills();
    while (!index.empty())
      remove(index.begin()->second);
  }

  std::size_t liveOrders() const noexcept { return index.size(); }
  std::size_t bidLevels() const noexcept { return bids.size(); }
  std::size_t askLevels() const noexcept { return asks.size(); }
  std::uint64_t fillsTotal() const noexcept { return sequence; }

  // Every level's count and quantity match its orders, and the index holds
  // exactly the orders on the levels. Untimed; empty if consistent.
  std::string inconsistency() const {
    std::size_t onLevels = 0;
    auto checkSide = [&](const auto &levels) -> std::string {
      for (const auto &[price, level] : levels) {
        std::uint32_t count = 0;
        std::uint64_t qty = 0;
        for (const Order *o = level.head; o != nullptr; o = o->next) {
          if (o->level != &level || o->price != price)
            return std::format("order {} is linked to the wrong level", o->id);
          ++count;
          qty += o->qty;
        }
        if (count != level.count || qty != level.qty || count == 0)
          return std::format("level {} holds {} orders / {} qty, records {} / "
                             "{}",
                             price, count, qty, level.count, level.qty);
        onLevels += count;
      }
      return {};
    };
    if (std::string bad = checkSide(bids); !bad.empty())
      return bad;
    if (std::string bad = checkSide(asks); !bad.empty())
      return bad;
    if (onLevels != index.size())
      return std::format("{} orders on levels but {} indexed", onLevels,
                         index.size());
    return {};
  }

private:
  // The generator only references live orders, so the id is always there.
  Order *find(std::uint64_t id) const noexcept {
    const auto it = index.find(id);
    TA_ASSERT(it != index.end());
    return it->second;
  }

  void add(std::uint64_t id, Side side, std::int32_t price, std::uint32_t qty) {
    Level &level =
        side == Side::Buy ? levelAt(bids, price) : levelAt(asks, price);
    Order *o = alloc::make<Order, R::hot>(
        Order{id, level.tail, nullptr, &level, nullptr, price, qty, side});
    // Allocated right after the order, as a handler that keeps per-order
    // client data does; written, never read.
    o->meta = alloc::allocate<R::cold>(coldBytes, coldAlign);
    std::memset(o->meta, static_cast<int>(id & 0xff), coldBytes);
    (level.tail != nullptr ? level.tail->next : level.head) = o;
    level.tail = o;
    ++level.count;
    level.qty += qty;
    index.emplace(id, o);
  }

  template <class Levels>
  static Level &levelAt(Levels &levels, std::int32_t price) {
    return levels.try_emplace(price, Level{price, 0, 0, nullptr, nullptr})
        .first->second;
  }

  void reduce(Order *o, std::uint32_t qty) {
    o->qty -= qty;
    o->level->qty -= qty;
    if (o->qty == 0)
      remove(o);
  }

  void remove(Order *o) noexcept {
    Level &level = *o->level;
    (o->prev != nullptr ? o->prev->next : level.head) = o->next;
    (o->next != nullptr ? o->next->prev : level.tail) = o->prev;
    level.qty -= o->qty;
    if (--level.count == 0) {
      if (o->side == Side::Buy)
        bids.erase(o->price);
      else
        asks.erase(o->price);
    }
    index.erase(o->id);
    alloc::deallocate<R::cold>(o->meta, coldBytes, coldAlign);
    alloc::destroy<Order, R::hot>(o);
  }

  std::size_t coldBytes;
  std::span<Fill *> fills;
  std::size_t fillCount = 0;
  std::uint64_t sequence = 0;
  Bids bids;
  Asks asks;
  Index index;
};

// The parse buffer every message goes through: copied in, decoded from, freed.
// doNotOptimize keeps the copy, which would otherwise be forwarded away.
template <bool Hints>
[[gnu::always_inline]] inline Message parse(const Message &wire) {
  constexpr Role transient = Roles<Hints>::transient;
  void *buffer = alloc::allocate<transient>(sizeof(Message), alignof(Message));
  std::memcpy(buffer, &wire, sizeof(Message));
  tabench::doNotOptimize(buffer);
  Message m;
  std::memcpy(&m, buffer, sizeof(Message));
  alloc::deallocate<transient>(buffer, sizeof(Message), alignof(Message));
  return m;
}

// Messages [first, last). Timed: each message's ticks go to ticks[i - first],
// and RSS is sampled every sampleMessages, between messages.
template <bool Hints, bool Timed>
void replay(const Config &config, const Tape &tape, Book<Hints> &book,
            std::size_t first, std::size_t last, std::span<std::uint32_t> ticks,
            tabench::Session &session) {
  for (std::size_t i = first; i < last; ++i) {
    const std::size_t n = i - first;
    if constexpr (Timed) {
      if (n % config.sampleMessages == 0)
        session.sampleRSS(alloc::seamStats.totalLiveBytes);
      const std::uint64_t t0 = tabench::readTicks();
      book.apply(parse<Hints>(tape.messages[i]));
      ticks[n] = static_cast<std::uint32_t>(tabench::readTicks() - t0);
    } else {
      book.apply(parse<Hints>(tape.messages[i]));
    }
    if ((n + 1) % config.phaseMessages == 0) {
      book.flushFills();
      alloc::endPhase();
    }
  }
}

// Per-message latency in log-spaced buckets, 8 per octave: bucket b holds
// [2^(b/8), 2^((b+1)/8)) ns. Small enough for every record, and enough to draw
// the latency distribution, whose tail percentiles alone don't show its shape.
tabench::Json latencyHistogram(std::span<const double> ns) {
  constexpr int perOctave = 8;
  auto bucketOf = [](double v) {
    return static_cast<int>(
        std::floor(std::log2(std::max(v, 1.0)) * perOctave));
  };
  int first = std::numeric_limits<int>::max();
  int last = 0;
  for (double v : ns) {
    first = std::min(first, bucketOf(v));
    last = std::max(last, bucketOf(v));
  }
  std::vector<std::uint64_t> counts(ns.empty() ? 0 : last - first + 1);
  for (double v : ns)
    ++counts[bucketOf(v) - first];
  tabench::Json j;
  j["bucketsPerOctave"] = perOctave;
  j["firstBucket"] = ns.empty() ? 0 : first;
  j["counts"] = counts;
  return j;
}

tabench::Json metricsJson(const Config &config, const Tape &tape,
                          std::size_t liveOrders, std::size_t bidLevels,
                          std::size_t askLevels, std::uint64_t fills,
                          std::span<const std::uint32_t> ticks,
                          const tabench::Session &session,
                          const alloc::SeamRegion &seam,
                          std::uint64_t leakedBytes) {
  const tabench::ClockInfo &clock = session.clockInfo();
  const std::size_t first = tape.firstMeasured(config);
  std::vector<double> all;
  std::array<std::vector<double>, typeNames.size()> byType;
  all.reserve(ticks.size());
  for (std::size_t n = 0; n < ticks.size(); ++n) {
    const double ns = clock.toNs(ticks[n]);
    all.push_back(ns);
    byType[std::to_underlying(tape.messages[first + n].type)].push_back(ns);
  }

  tabench::Json j;
  j["messages"] = ticks.size();
  tabench::Json mix;
  for (std::size_t t = 0; t < typeNames.size(); ++t)
    mix[std::string(typeNames[t])] = tape.mix[t];
  j["mix"] = mix;
  j["fills"] = fills;
  j["latencyHistogram"] = latencyHistogram(all);
  j["nsPerMessage"] = tabench::toJson(tabench::summarize(all));
  tabench::Json perType;
  for (std::size_t t = 0; t < typeNames.size(); ++t)
    perType[std::string(typeNames[t])] =
        byType[t].empty() ? tabench::Json(nullptr)
                          : tabench::toJson(tabench::summarize(byType[t]));
  j["nsPerMessageByType"] = perType;
  const std::optional<tabench::CounterResult> counted =
      session.region().counters();
  j["cyclesPerMessage"] =
      counted && counted->status == tabench::CounterStatus::Ok
          ? tabench::Json(
                static_cast<double>(counted->value(tabench::Counter::Cycles)) /
                static_cast<double>(ticks.size()))
          : tabench::Json(nullptr);
  j["book"] = {{"liveOrders", liveOrders},
               {"bidLevels", bidLevels},
               {"askLevels", askLevels},
               {"orderBytes", sizeof(Order)},
               {"coldBytes", config.coldBytes}};
  j["seam"] = alloc::seamJson(seam, leakedBytes);
  return j;
}

template <bool Hints>
void run(const Config &config, tabench::Session &session) {
  const Tape tape = makeTape(config);
  tabench::MappedArray<std::uint32_t> ticks(config.messages);
  tabench::MappedArray<Fill *> fillLog(config.phaseMessages);
  session.addInputBytes(tape.messages.bytes() + ticks.bytes() +
                        fillLog.bytes());

  auto book = std::make_unique<Book<Hints>>(config, fillLog.span());
  const std::size_t first = tape.firstMeasured(config);
  const std::size_t last = first + config.messages;
  // Untimed: build the book, then run the normal mix to a steady state.
  replay<Hints, false>(config, tape, *book, 0, first, ticks.span(), session);
  book->flushFills();
  alloc::endPhase();

  session.warmUpCPU();
  alloc::SeamRegion seam;
  session.measure([&] {
    seam.begin = alloc::seamStats.beginRegion();
    replay<Hints, true>(config, tape, *book, first, last, ticks.span(),
                        session);
    seam.end = alloc::seamStats;
  });

  if (std::string bad = book->inconsistency(); !bad.empty())
    throw std::runtime_error("book inconsistent after replay: " + bad);
  if (book->liveOrders() != tape.liveAtEnd)
    throw std::runtime_error(
        std::format("book holds {} orders, the tape expects {}",
                    book->liveOrders(), tape.liveAtEnd));
  const std::size_t liveOrders = book->liveOrders();
  const std::size_t bidLevels = book->bidLevels();
  const std::size_t askLevels = book->askLevels();
  const std::uint64_t fills = book->fillsTotal();
  // The whole book, index buckets included, goes back before the leak check.
  book.reset();

  tabench::Json record = session.record("default");
  record["metrics"] =
      metricsJson(config, tape, liveOrders, bidLevels, askLevels, fills,
                  ticks.span(), session, seam, alloc::seamStats.totalLiveBytes);
  session.write(record);
}

} // namespace

int main(int argc, char **argv) {
  return tabench::Session::run("orderbook", [&] {
    tabench::Session session(argc, argv, "orderbook");
    const Config config = parseConfig(session.args());
    session.start();
    if (config.hints)
      run<true>(config, session);
    else
      run<false>(config, session);
  });
}
