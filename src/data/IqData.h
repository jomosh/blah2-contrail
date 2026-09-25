/// @file IqData.h
/// @class IqData
/// @brief A class to store IQ data.
/// @details Implements a FIFO queue to store IQ samples.
/// @author 30hours

#ifndef IQDATA_H
#define IQDATA_H

#include <stdint.h>
#include <deque>
#include <vector>
#include <complex>
#include <mutex>
#include <condition_variable>

class IqData
{
private:
  /// @brief Maximum number of samples.
  uint32_t n;

  /// @brief True if should not push to buffer (mutex).
  std::mutex mutex_lock;

  /// @brief Notifies when new data may be available.
  std::condition_variable data_ready;

  /// @brief Ring-buffer storage for IQ data.
  std::vector<std::complex<double>> data;

  /// @brief Index of oldest sample in ring-buffer.
  uint32_t head;

  /// @brief Number of valid samples currently in buffer.
  uint32_t length;

  /// @brief Minimum value.
  double min;

  /// @brief Maximum value.
  double max;

  /// @brief Mean value.
  double mean;

  /// @brief Spectrum vector (reference channel).
  std::vector<std::complex<double>> spectrum;

  /// @brief Spectrum vector (surveillance channel).
  std::vector<std::complex<double>> spectrum_surv;

  /// @brief Frequency vector (kHz).
  std::vector<double> frequency;

  /// @brief Decimated IQ samples for reference channel IQ scatter.
  std::vector<double> iq_decimated_ref;

  /// @brief Decimated IQ samples for surveillance channel IQ scatter.
  std::vector<double> iq_decimated_surv;

public:
  /// @brief Constructor.
  /// @param n Number of samples.
  /// @return The object.
  IqData(uint32_t n);

  /// @brief Getter for maximum number of samples.
  /// @return Maximum number of samples.
  uint32_t get_n();

  /// @brief Getter for current data length.
  /// @return Number of samples currently in data.
  uint32_t get_length();

  /// @brief Locker for mutex.
  /// @return Void.
  void lock();

  /// @brief Unlocker for mutex.
  /// @return Void.
  void unlock();

  /// @brief Unlock mutex and notify waiters that data may be available.
  /// @return Void.
  void unlock_and_notify();

  /// @brief Wait until at least minLength samples are available.
  /// @param minLength Minimum required samples in buffer.
  /// @return Void.
  void wait_for_min_length(uint32_t minLength);

  /// @brief Wait until the buffer length is at most maxLength.
  /// @param maxLength Maximum allowed samples in buffer.
  /// @return Void.
  void wait_for_max_length(uint32_t maxLength);

  /// @brief Getter for data.
  /// @return IQ data.
  std::deque<std::complex<double>> get_data();

  /// @brief Get sample by index relative to oldest sample.
  /// @param index Zero-based index from oldest sample.
  /// @return Sample at index.
  std::complex<double> at(uint32_t index) const;

  /// @brief Get sample by index without bounds checking.
  /// @param index Zero-based index from oldest sample.
  /// @return Sample at index.
  std::complex<double> at_unchecked(uint32_t index) const;

  /// @brief Push a sample to the queue.
  /// @param sample A single sample.
  /// @return Void.
  void push_back(std::complex<double> sample);

  /// @brief Pop the front of the queue.
  /// @return Sample from the front of the queue.
  std::complex<double> pop_front();

  /// @brief Append a contiguous block of samples in bulk.
  /// @details Produces the same final ring state as calling push_back once per
  /// sample in order, but transfers data with memcpy. When count exceeds the
  /// available capacity the oldest samples are overwritten; when count >= n the
  /// resulting buffer contains the newest n samples of the input block.
  /// Intermediate overwrite ordering may differ from a single-sample loop, but
  /// the final state is identical.
  /// @note samples must not overlap this buffer's internal storage.
  /// @note Not internally synchronized; the caller must hold the buffer's
  /// mutex when sharing it across threads.
  /// @param samples Pointer to contiguous samples to append.
  /// @param count Number of samples to append.
  /// @return Void.
  void append(const std::complex<double> *samples, uint32_t count);

  /// @brief Pop a contiguous block of samples from the front in bulk.
  /// @details Semantically equivalent to calling pop_front for each sample in
  /// order, but transfers data with memcpy. Caller must guarantee
  /// count <= get_length().
  /// @note out must not overlap this buffer's internal storage.
  /// @note Not internally synchronized; the caller must hold the buffer's
  /// mutex when sharing it across threads.
  /// @param out Pointer to contiguous destination of at least count samples.
  /// @param count Number of samples to pop.
  /// @return Void.
  void pop_into(std::complex<double> *out, uint32_t count);

  /// @brief Print to stdout (debug).
  /// @return Void.
  void print();

  /// @brief Clear samples from the queue.
  /// @return Void.
  void clear();

  /// @brief Update the reference spectrum vector.
  /// @param spectrum Spectrum vector.
  /// @return Void.
  void update_spectrum(const std::vector<std::complex<double>> &spectrum);

  /// @brief Update the surveillance spectrum vector.
  /// @param spectrum_surv Spectrum vector for surveillance channel.
  /// @return Void.
  void update_spectrum_surv(const std::vector<std::complex<double>> &spectrum_surv);

  /// @brief Update the decimated IQ samples for IQ scatter views.
  /// @param iq_ref Flat vector of I/Q interleaved for reference.
  /// @param iq_surv Flat vector of I/Q interleaved for surveillance.
  /// @return Void.
  void update_iq_decimated(const std::vector<double> &iq_ref, const std::vector<double> &iq_surv);

  /// @brief Update the frequency vector.
  /// @param frequency Frequency vector.
  /// @return Void.
  void update_frequency(const std::vector<double> &frequency);

  /// @brief Getter for frequency bins written by SpectrumAnalyser.
  /// @return Const reference to frequency vector (kHz).
  const std::vector<double> &get_frequency() const;

  /// @brief Generate JSON of the signal and metadata.
  /// @param timestamp Current time (POSIX ms).
  /// @return JSON string.
  std::string to_json(uint64_t timestamp);
};

#endif