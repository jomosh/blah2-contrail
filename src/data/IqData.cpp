#include "IqData.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filewritestream.h"

// constructor
IqData::IqData(uint32_t _n)
{
  n = _n;
  data.resize(n, {0.0, 0.0});
  head = 0;
  length = 0;
}

uint32_t IqData::get_n()
{
  return n;
}

uint32_t IqData::get_length()
{
  return length;
}

void IqData::lock()
{
  mutex_lock.lock();
}

void IqData::unlock()
{
  mutex_lock.unlock();
}

void IqData::unlock_and_notify()
{
  mutex_lock.unlock();
  data_ready.notify_all();
}

void IqData::wait_for_min_length(uint32_t minLength)
{
  if (minLength > n)
  {
    throw std::invalid_argument("IqData::wait_for_min_length minLength exceeds buffer capacity");
  }

  std::unique_lock<std::mutex> lock(mutex_lock);
  data_ready.wait(lock, [&] { return length >= minLength; });
}

void IqData::wait_for_max_length(uint32_t maxLength)
{
  if (maxLength > n)
  {
    throw std::invalid_argument("IqData::wait_for_max_length maxLength exceeds buffer capacity");
  }

  std::unique_lock<std::mutex> lock(mutex_lock);
  data_ready.wait(lock, [&] { return length <= maxLength; });
}

std::deque<std::complex<double>> IqData::get_data()
{
  std::deque<std::complex<double>> out;
  for (uint32_t i = 0; i < length; i++)
  {
    out.push_back(data[(head + i) % n]);
  }
  return out;
}

std::complex<double> IqData::at(uint32_t index) const
{
  if (index >= length)
  {
    throw std::out_of_range("IqData::at index out of range");
  }
  return data[(head + index) % n];
}

std::complex<double> IqData::at_unchecked(uint32_t index) const
{
  return data[(head + index) % n];
}

void IqData::push_back(std::complex<double> sample)
{
  if (length < n)
  {
    data[(head + length) % n] = sample;
    length++;
  }
  else
  {
    data[head] = sample;
    head = (head + 1) % n;
  }
}

std::complex<double> IqData::pop_front()
{
  if (length == 0) {
    throw std::runtime_error("Attempting to pop from an empty IqData ring buffer");
  }
  std::complex<double> sample = data[head];
  head = (head + 1) % n;
  length--;
  return sample;
}

static_assert(std::is_trivially_copyable<std::complex<double>>::value,
  "IqData bulk transfer relies on std::complex<double> being trivially copyable");

void IqData::append(const std::complex<double> *samples, uint32_t count)
{
  if (count == 0 || samples == nullptr)
  {
    return;
  }

  if (count >= n)
  {
    // The incoming block overwrites the entire buffer; keep only the newest n.
    const uint32_t offset = count - n;
    std::memcpy(data.data(), samples + offset,
      n * sizeof(std::complex<double>));
    head = 0;
    length = n;
    return;
  }

  const uint32_t available = n - length;
  if (count <= available)
  {
    // No overwrite: append after the current newest sample.
    uint32_t write = (head + length) % n;
    const uint32_t first = std::min(count, n - write);
    std::memcpy(data.data() + write, samples,
      first * sizeof(std::complex<double>));
    if (first < count)
    {
      std::memcpy(data.data(), samples + first,
        (count - first) * sizeof(std::complex<double>));
    }
    length += count;
  }
  else
  {
    // Overwrite the oldest samples to make room.
    const uint32_t overflow = count - available;
    head = (head + overflow) % n;
    uint32_t write = (head + (n - count)) % n;
    const uint32_t first = std::min(count, n - write);
    std::memcpy(data.data() + write, samples,
      first * sizeof(std::complex<double>));
    if (first < count)
    {
      std::memcpy(data.data(), samples + first,
        (count - first) * sizeof(std::complex<double>));
    }
    length = n;
  }
}

void IqData::pop_into(std::complex<double> *out, uint32_t count)
{
  if (count == 0 || out == nullptr)
  {
    return;
  }
  if (count > length)
  {
    throw std::runtime_error("IqData::pop_into count exceeds available samples");
  }

  const uint32_t first = std::min(count, n - head);
  std::memcpy(out, data.data() + head,
    first * sizeof(std::complex<double>));
  if (first < count)
  {
    std::memcpy(out + first, data.data(),
      (count - first) * sizeof(std::complex<double>));
  }
  head = (head + count) % n;
  length -= count;
}

void IqData::print()
{
  std::cout << length << std::endl;
  for (uint32_t i = 0; i < length; i++)
  {
    std::cout << data[(head + i) % n] << std::endl;
  }
}

void IqData::clear()
{
  head = 0;
  length = 0;
}

void IqData::update_spectrum(const std::vector<std::complex<double>> &_spectrum)
{
  spectrum = _spectrum;
}

void IqData::update_spectrum_surv(const std::vector<std::complex<double>> &_spectrum_surv)
{
  spectrum_surv = _spectrum_surv;
}

void IqData::update_frequency(const std::vector<double> &_frequency)
{
  frequency = _frequency;
}

void IqData::update_iq_decimated(const std::vector<double> &iq_ref, const std::vector<double> &iq_surv)
{
  iq_decimated_ref = iq_ref;
  iq_decimated_surv = iq_surv;
}

const std::vector<double> &IqData::get_frequency() const
{
  return frequency;
}

std::string IqData::to_json(uint64_t timestamp)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  // store frequency array
  rapidjson::Value arrayFrequency(rapidjson::kArrayType);
  for (size_t i = 0; i < frequency.size(); i++)
  {
    arrayFrequency.PushBack(frequency[i], allocator);
  }

  // store spectrum array (clamp -Inf to -200 dB for zero/empty bins)
  rapidjson::Value arraySpectrum(rapidjson::kArrayType);
  for (size_t i = 0; i < spectrum.size(); i++)
  {
    double mag = std::abs(spectrum[i]);
    double db = (mag > 0.0) ? 10 * std::log10(mag) : -200.0;
    arraySpectrum.PushBack(db, allocator);
  }

  // store surveillance spectrum array
  rapidjson::Value arraySpectrumSurv(rapidjson::kArrayType);
  for (size_t i = 0; i < spectrum_surv.size(); i++)
  {
    double mag = std::abs(spectrum_surv[i]);
    double db = (mag > 0.0) ? 10 * std::log10(mag) : -200.0;
    arraySpectrumSurv.PushBack(db, allocator);
  }

  // store decimated IQ samples for reference IQ scatter
  rapidjson::Value arrayIqRef(rapidjson::kArrayType);
  for (size_t i = 0; i < iq_decimated_ref.size(); i++)
  {
    arrayIqRef.PushBack(iq_decimated_ref[i], allocator);
  }

  // store decimated IQ samples for surveillance IQ scatter
  rapidjson::Value arrayIqSurv(rapidjson::kArrayType);
  for (size_t i = 0; i < iq_decimated_surv.size(); i++)
  {
    arrayIqSurv.PushBack(iq_decimated_surv[i], allocator);
  }

  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("min", min, allocator);
  document.AddMember("max", max, allocator);
  document.AddMember("mean", mean, allocator);
  document.AddMember("frequency", arrayFrequency, allocator);
  document.AddMember("spectrum", arraySpectrum, allocator);
  document.AddMember("spectrumSurv", arraySpectrumSurv, allocator);
  document.AddMember("iqRef", arrayIqRef, allocator);
  document.AddMember("iqSurv", arrayIqSurv, allocator);

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}