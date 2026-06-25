#include "doctest/doctest.h"
#include "core/bounded_queue.h"
#include <thread>
#include <atomic>
#include <vector>
using namespace suji;
TEST_CASE("producers/consumer transfer all items, close drains") {
  BoundedQueue<int> q(8);
  const int N=1000, P=4;
  std::atomic<int> produced{0};
  std::vector<std::thread> prod;
  for(int t=0;t<P;t++) prod.emplace_back([&]{ int x; while((x=produced.fetch_add(1))<N) q.push(x); });
  std::atomic<long long> sum{0}; std::atomic<int> got{0};
  std::thread cons([&]{ int v; while(q.pop(v)){ sum+=v; got++; } });
  for(auto& t:prod) t.join();
  q.close();
  cons.join();
  CHECK(got == N);
  CHECK(sum == (long long)N*(N-1)/2);   // 0+1+...+(N-1)
}
TEST_CASE("pop returns false when closed and empty") {
  BoundedQueue<int> q(4); q.close(); int v; CHECK_FALSE(q.pop(v));
}
TEST_CASE("try_pop batches what's available") {
  BoundedQueue<int> q(8); q.push(1); q.push(2);
  int a,b,c; CHECK(q.try_pop(a)); CHECK(q.try_pop(b)); CHECK_FALSE(q.try_pop(c));
}
