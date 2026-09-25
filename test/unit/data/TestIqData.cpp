/// @file TestIqData.cpp
/// @brief Unit test for IqData.cpp
/// @author GitHub Copilot

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <vector>

#include "data/IqData.h"

namespace
{
std::vector<int> pop_real_samples(IqData &iqData, uint32_t nSamples)
{
  std::vector<int> out;
  out.reserve(nSamples);
  for (uint32_t i = 0; i < nSamples; i++)
  {
    out.push_back(static_cast<int>(iqData.pop_front().real()));
  }
  return out;
}
}

TEST_CASE("Wait_For_Min_Length_Rejects_Impossible_Request", "[iqdata]")
{
  IqData iqData(8);

  CHECK_THROWS_AS(iqData.wait_for_min_length(9), std::invalid_argument);
}

TEST_CASE("Wait_For_Max_Length_Blocks_Until_Buffer_Has_Space", "[iqdata]")
{
  IqData iqData(2);
  iqData.lock();
  iqData.push_back({1.0, 0.0});
  iqData.push_back({2.0, 0.0});
  iqData.unlock_and_notify();

  std::atomic<bool> waiterStarted{false};
  std::atomic<bool> waiterReleased{false};
  std::thread waiter([&]() {
    waiterStarted.store(true, std::memory_order_release);
    iqData.wait_for_max_length(1);
    waiterReleased.store(true, std::memory_order_release);
  });

  const auto waitForState = [](const std::atomic<bool> &state,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!state.load(std::memory_order_acquire)
      && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    return state.load(std::memory_order_acquire);
  };

  const bool waiterStartedInTime = waitForState(waiterStarted,
    std::chrono::milliseconds(200));
  const bool waiterReleasedWhileFull = waiterReleased.load(
    std::memory_order_acquire);

  iqData.lock();
  (void)iqData.pop_front();
  iqData.unlock_and_notify();

  const bool waiterReleasedInTime = waitForState(waiterReleased,
    std::chrono::milliseconds(200));
  waiter.join();

  CHECK(waiterStartedInTime == true);
  CHECK(waiterReleasedWhileFull == false);
  CHECK(waiterReleasedInTime == true);
}

TEST_CASE("Overwrite_PreservesNewestSamplesInFifoOrder", "[iqdata]")
{
  IqData iqData(3);

  for (int sample = 0; sample < 5; sample++)
  {
    iqData.push_back({static_cast<double>(sample), static_cast<double>(-sample)});
  }

  REQUIRE(iqData.get_length() == 3);

  const std::deque<std::complex<double>> snapshot = iqData.get_data();
  REQUIRE(snapshot.size() == 3);
  CHECK(snapshot[0] == std::complex<double>(2.0, -2.0));
  CHECK(snapshot[1] == std::complex<double>(3.0, -3.0));
  CHECK(snapshot[2] == std::complex<double>(4.0, -4.0));

  CHECK(iqData.at(0) == std::complex<double>(2.0, -2.0));
  CHECK(iqData.at(1) == std::complex<double>(3.0, -3.0));
  CHECK(iqData.at_unchecked(2) == std::complex<double>(4.0, -4.0));

  CHECK(pop_real_samples(iqData, 3) == std::vector<int>{2, 3, 4});
  CHECK(iqData.get_length() == 0);
}

TEST_CASE("Append_BulkMatchesPushBackSemantics", "[iqdata]")
{
  IqData bulk(5);
  IqData single(5);

  const std::vector<std::complex<double>> samples = {
    {1.0, -1.0}, {2.0, -2.0}, {3.0, -3.0}
  };
  bulk.append(samples.data(), static_cast<uint32_t>(samples.size()));
  for (const auto &s : samples) {
    single.push_back(s);
  }

  REQUIRE(bulk.get_length() == single.get_length());
  REQUIRE(bulk.get_data() == single.get_data());
}

TEST_CASE("Append_WrapBoundaryOverwritesOldestSamples", "[iqdata]")
{
  IqData bulk(4);
  IqData single(4);

  const std::vector<std::complex<double>> first = {
    {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}
  };
  const std::vector<std::complex<double>> second = {
    {4.0, 0.0}, {5.0, 0.0}, {6.0, 0.0}
  };

  bulk.append(first.data(), 3);
  for (const auto &s : first) {
    single.push_back(s);
  }

  // Cross the wrap boundary so append must overwrite oldest samples.
  bulk.append(second.data(), 3);
  for (const auto &s : second) {
    single.push_back(s);
  }

  REQUIRE(bulk.get_length() == 4);
  REQUIRE(single.get_length() == 4);
  REQUIRE(bulk.get_data() == single.get_data());
}

TEST_CASE("Pop_Into_BulkMatchesPopFrontSemantics", "[iqdata]")
{
  IqData bulk(6);
  IqData single(6);

  const std::vector<std::complex<double>> samples = {
    {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}
  };
  bulk.append(samples.data(), 5);
  single.append(samples.data(), 5);

  std::vector<std::complex<double>> outBulk(3);
  std::vector<std::complex<double>> outSingle(3);
  bulk.pop_into(outBulk.data(), 3);
  for (uint32_t i = 0; i < 3; i++) {
    outSingle[i] = single.pop_front();
  }

  REQUIRE(outBulk == outSingle);
  REQUIRE(bulk.get_length() == single.get_length());
  REQUIRE(bulk.get_data() == single.get_data());
}

TEST_CASE("Append_CountEqualsN_ReplacesWholeBuffer", "[iqdata]")
{
  IqData iq(3);
  const std::vector<std::complex<double>> initial = {
    {9.0, 0.0}, {8.0, 0.0}, {7.0, 0.0}
  };
  iq.append(initial.data(), 3);
  REQUIRE(iq.get_length() == 3);

  const std::vector<std::complex<double>> replacement = {
    {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}
  };
  iq.append(replacement.data(), 3);

  REQUIRE(iq.get_length() == 3);
  const auto snapshot = iq.get_data();
  CHECK(snapshot[0] == std::complex<double>(1.0, 0.0));
  CHECK(snapshot[1] == std::complex<double>(2.0, 0.0));
  CHECK(snapshot[2] == std::complex<double>(3.0, 0.0));
}

TEST_CASE("Pop_Into_CountExceedsLength_Throws", "[iqdata]")
{
  IqData iq(4);
  const std::vector<std::complex<double>> samples = {
    {1.0, 0.0}, {2.0, 0.0}
  };
  iq.append(samples.data(), 2);

  std::vector<std::complex<double>> out(3);
  CHECK_THROWS_AS(iq.pop_into(out.data(), 3), std::runtime_error);

  // Guard must leave the buffer unchanged.
  REQUIRE(iq.get_length() == 2);
}

TEST_CASE("Append_CountGreaterThanN_KeepsNewestN", "[iqdata]")
{
  IqData iq(3);
  const std::vector<std::complex<double>> samples = {
    {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}
  };
  iq.append(samples.data(), 5);

  REQUIRE(iq.get_length() == 3);
  const auto snapshot = iq.get_data();
  CHECK(snapshot[0] == std::complex<double>(3.0, 0.0));
  CHECK(snapshot[1] == std::complex<double>(4.0, 0.0));
  CHECK(snapshot[2] == std::complex<double>(5.0, 0.0));
}

TEST_CASE("Append_CountGreaterThanN_WithExistingData", "[iqdata]")
{
  IqData iq(3);
  const std::vector<std::complex<double>> pre = {
    {7.0, 0.0}, {8.0, 0.0}
  };
  iq.append(pre.data(), 2);
  REQUIRE(iq.get_length() == 2);

  const std::vector<std::complex<double>> samples = {
    {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}
  };
  iq.append(samples.data(), 5);

  // count >= n path must discard prior contents entirely and keep newest n.
  REQUIRE(iq.get_length() == 3);
  const auto snapshot = iq.get_data();
  CHECK(snapshot[0] == std::complex<double>(3.0, 0.0));
  CHECK(snapshot[1] == std::complex<double>(4.0, 0.0));
  CHECK(snapshot[2] == std::complex<double>(5.0, 0.0));
}

TEST_CASE("Append_CountGreaterThanN_WithHeadOffset", "[iqdata]")
{
  IqData iq(4);
  const std::vector<std::complex<double>> initial = {
    {10.0, 0.0}, {20.0, 0.0}, {30.0, 0.0}, {40.0, 0.0}
  };
  iq.append(initial.data(), 4);

  // Advance head off zero before taking the count >= n path.
  std::vector<std::complex<double>> two(2);
  iq.pop_into(two.data(), 2);

  const std::vector<std::complex<double>> samples = {
    {10.0, 0.0}, {20.0, 0.0}, {30.0, 0.0}, {40.0, 0.0}, {50.0, 0.0}
  };
  iq.append(samples.data(), 5);

  // count >= n resets head and keeps only the newest n of the input.
  REQUIRE(iq.get_length() == 4);
  const auto snapshot = iq.get_data();
  CHECK(snapshot[0] == std::complex<double>(20.0, 0.0));
  CHECK(snapshot[1] == std::complex<double>(30.0, 0.0));
  CHECK(snapshot[2] == std::complex<double>(40.0, 0.0));
  CHECK(snapshot[3] == std::complex<double>(50.0, 0.0));
}

TEST_CASE("Append_CountEqualsN_WithHeadOffset", "[iqdata]")
{
  IqData iq(4);
  const std::vector<std::complex<double>> initial = {
    {10.0, 0.0}, {20.0, 0.0}, {30.0, 0.0}, {40.0, 0.0}
  };
  iq.append(initial.data(), 4);

  // Advance head off zero before taking the count == n path.
  std::vector<std::complex<double>> two(2);
  iq.pop_into(two.data(), 2);

  const std::vector<std::complex<double>> replacement = {
    {11.0, 0.0}, {22.0, 0.0}, {33.0, 0.0}, {44.0, 0.0}
  };
  iq.append(replacement.data(), 4);

  // count == n resets head and replaces the entire buffer contents.
  REQUIRE(iq.get_length() == 4);
  const auto snapshot = iq.get_data();
  CHECK(snapshot[0] == std::complex<double>(11.0, 0.0));
  CHECK(snapshot[1] == std::complex<double>(22.0, 0.0));
  CHECK(snapshot[2] == std::complex<double>(33.0, 0.0));
  CHECK(snapshot[3] == std::complex<double>(44.0, 0.0));
}

TEST_CASE("Append_NullPointer_Throws", "[iqdata]")
{
  IqData iq(4);
  CHECK_THROWS_AS(iq.append(nullptr, 2), std::invalid_argument);
  REQUIRE(iq.get_length() == 0);
}

TEST_CASE("Pop_Into_NullPointer_Throws", "[iqdata]")
{
  IqData iq(4);
  const std::vector<std::complex<double>> samples = {
    {1.0, 0.0}, {2.0, 0.0}
  };
  iq.append(samples.data(), 2);

  CHECK_THROWS_AS(iq.pop_into(nullptr, 2), std::invalid_argument);
  REQUIRE(iq.get_length() == 2);
}

TEST_CASE("Append_ZeroCount_IsNoOp", "[iqdata]")
{
  IqData iq(4);
  iq.append(nullptr, 0);
  REQUIRE(iq.get_length() == 0);
}

TEST_CASE("Pop_Into_ZeroCount_IsNoOp", "[iqdata]")
{
  IqData iq(4);
  const std::vector<std::complex<double>> samples = {
    {1.0, 0.0}, {2.0, 0.0}
  };
  iq.append(samples.data(), 2);

  iq.pop_into(nullptr, 0);
  REQUIRE(iq.get_length() == 2);
}

TEST_CASE("Append_CountEqualsAvailable_Boundary", "[iqdata]")
{
  IqData iq(4);
  const std::vector<std::complex<double>> first = {{0.0, 0.0}};
  iq.append(first.data(), 1);

  const std::vector<std::complex<double>> fill = {
    {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}
  };
  iq.append(fill.data(), 3);

  REQUIRE(iq.get_length() == 4);
  const auto snapshot = iq.get_data();
  CHECK(snapshot[0] == std::complex<double>(0.0, 0.0));
  CHECK(snapshot[1] == std::complex<double>(1.0, 0.0));
  CHECK(snapshot[2] == std::complex<double>(2.0, 0.0));
  CHECK(snapshot[3] == std::complex<double>(3.0, 0.0));
}

TEST_CASE("Pop_Into_CountEqualsLength", "[iqdata]")
{
  IqData iq(5);
  const std::vector<std::complex<double>> samples = {
    {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}
  };
  iq.append(samples.data(), 3);

  std::vector<std::complex<double>> out(3);
  iq.pop_into(out.data(), 3);

  REQUIRE(iq.get_length() == 0);
  CHECK(out[0] == std::complex<double>(1.0, 0.0));
  CHECK(out[1] == std::complex<double>(2.0, 0.0));
  CHECK(out[2] == std::complex<double>(3.0, 0.0));
}

TEST_CASE("Pop_Into_WrapBoundaryPreservesOrder", "[iqdata]")
{
  IqData iq(4);
  const std::vector<std::complex<double>> samples = {
    {0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}
  };
  iq.append(samples.data(), 4);

  // Pop 1 to move head away from 0, then verify a full-ish wrap pop.
  std::vector<std::complex<double>> one(1);
  iq.pop_into(one.data(), 1);

  // Refill across the wrap.
  const std::vector<std::complex<double>> more = {
    {4.0, 0.0}, {5.0, 0.0}, {6.0, 0.0}
  };
  iq.append(more.data(), 3);

  const auto snapshot = iq.get_data();
  REQUIRE(snapshot.size() == 4);

  // Pop past the wrap boundary (head + count > n).
  std::vector<std::complex<double>> out(4);
  iq.pop_into(out.data(), 4);

  CHECK(iq.get_length() == 0);
  CHECK(out[0] == snapshot[0]);
  CHECK(out[1] == snapshot[1]);
  CHECK(out[2] == snapshot[2]);
  CHECK(out[3] == snapshot[3]);
}

TEST_CASE("Paired_BuffersSkewWhenOneChannelOverwritesBeforePeerCatchesUp", "[iqdata]")
{
  constexpr uint32_t nSamples = 4;
  IqData reference(nSamples + 1);
  IqData surveillance(nSamples + 1);

  for (int sample = 0; sample < static_cast<int>(nSamples) + 2; sample++)
  {
    reference.push_back({static_cast<double>(sample), 0.0});
  }
  for (int sample = 0; sample < static_cast<int>(nSamples); sample++)
  {
    surveillance.push_back({static_cast<double>(sample), 0.0});
  }

  REQUIRE(reference.get_length() == nSamples + 1);
  REQUIRE(surveillance.get_length() == nSamples);

  reference.lock();
  surveillance.lock();
  const std::vector<int> extractedReference = pop_real_samples(reference, nSamples);
  const std::vector<int> extractedSurveillance = pop_real_samples(surveillance, nSamples);
  reference.unlock();
  surveillance.unlock();

  CHECK(extractedReference == std::vector<int>{1, 2, 3, 4});
  CHECK(extractedSurveillance == std::vector<int>{0, 1, 2, 3});
  CHECK(extractedReference != extractedSurveillance);
}
