#include "katana/Reduction.h"

#include <algorithm>
#include <functional>
#include <iostream>

#include "katana/Galois.h"
#include "katana/SharedMemSys.h"

struct Move {
  Move() = default;
  ~Move() = default;
  Move(const Move&) = delete;
  Move(Move&&) noexcept {}
  Move& operator=(const Move&) = delete;
  Move& operator=(Move&&) noexcept { return *this; }
};

void
test_move() {
  auto merge_fn = [](Move& a, Move&&) -> Move& { return a; };

  auto identity_fn = []() { return Move(); };

  auto r = katana::make_reducible(merge_fn, identity_fn);

  Move x;
  r.update(std::move(x));
  r.reduce();

  // And as expected, this will not compile:
  // reducible.update(x);
}

void
test_map() {
  using Map = std::map<std::string, int>;

  auto reduce = [](Map& a, Map&& b) -> Map& {
    Map v{std::move(b)};

    for (auto& kv : v) {
      if (a.count(kv.first) == 0) {
        a[kv.first] = 0;
      }
      a[kv.first] += kv.second;
    }

    return a;
  };

  auto zero_fn = []() -> Map { return Map(); };

  auto r = katana::make_reducible(reduce, zero_fn);
  r.update(Map{std::make_pair("key", 1)});
  Map& result = r.reduce();

  KATANA_LOG_ASSERT(result["key"] == 1);
}

void
other() {}

void
test_max() {
  const int& (*int_max)(const int&, const int&) = std::max<int>;
  std::function<const int&(const int&, const int&)> fn{int_max};

  auto r = katana::make_reducible(fn, []() { return 0; });

  constexpr int num = 10;

  r.update(num);
  r.update(1);

  KATANA_LOG_ASSERT(r.reduce() == num);
}

void
test_accum() {
  katana::GAccumulator<int> accum;

  constexpr int num = 123456;

  katana::do_all(katana::iterate(0, num), [&](int) { accum += 1; });

  KATANA_LOG_ASSERT(accum.reduce() == num);
}

int
main() {
  katana::SharedMemSys sys;
  katana::setActiveThreads(2);

  static_assert(
      sizeof(katana::GAccumulator<int>) <=
      sizeof(katana::PerThreadStorage<int>));

  test_map();
  test_move();
  test_max();
  test_accum();

  return 0;
}
