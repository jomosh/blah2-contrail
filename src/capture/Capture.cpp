#include "Capture.h"
#include "rspduo/RspDuo.h"
#include "usrp/Usrp.h"
#include "hackrf/HackRf.h"
#include "kraken/Kraken.h"
#include <chrono>
#include <exception>
#include <iostream>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <httplib.h>

// constants
const std::string Capture::VALID_TYPE[4] = {"RspDuo", "Usrp", "HackRF", "Kraken"};

// constructor
Capture::Capture(std::string _type, uint32_t _fs, uint32_t _fc, std::string _path)
{
  type = _type;
  fs = _fs;
  fc = _fc;
  path = _path;
  replay = false;
  saveIq.store(false);
}

bool Capture::is_saving_iq() const
{
  return saveIq.load();
}

Capture::ActiveIqCapture Capture::get_active_iq_capture() const
{
  std::lock_guard<std::mutex> lock(currentIqSaveFileMutex);
  return {currentIqSaveFile, currentIqCaptureStartMs};
}

void Capture::process(IqData *buffer1, IqData *buffer2, c4::yml::NodeRef config, 
  std::string ip_capture, uint16_t port_capture)
{
  std::cout << "Setting up device " + type << std::endl;

  device = factory_source(type, config);

  // capture status thread
  std::atomic<bool> pollCaptureStatus{true};
  std::exception_ptr captureStatusError;
  std::thread captureStatusThread([&]{
    try
    {
      while (pollCaptureStatus.load())
      {
        httplib::Client cli("http://" + ip_capture + ":" 
          + std::to_string(port_capture));
        httplib::Result res = cli.Get("/capture");
        if (!res)
        {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          continue;
        }

        const bool captureRequested = res->body == "true";

        // if capture status changed
        if (captureRequested != saveIq.load())
        {
          if (captureRequested)
          {
            // Open the file before exposing saveIq=true to live callbacks.
            const std::string iqSaveFile = device->open_file();
            const uint64_t captureStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
            {
              std::lock_guard<std::mutex> lock(currentIqSaveFileMutex);
              currentIqSaveFile = iqSaveFile;
              currentIqCaptureStartMs = captureStartMs;
            }
            saveIq.store(true);
          }
          else
          {
            saveIq.store(false);
            device->close_file();
            {
              std::lock_guard<std::mutex> lock(currentIqSaveFileMutex);
              currentIqSaveFile.clear();
              currentIqCaptureStartMs = 0;
            }
          }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
    catch (...)
    {
      captureStatusError = std::current_exception();
      pollCaptureStatus.store(false);
    }
  });

  const auto join_capture_status_thread = [&]() {
    if (captureStatusThread.joinable())
    {
      captureStatusThread.join();
    }
  };

  const auto rethrow_capture_status_error = [&]() {
    if (captureStatusError != nullptr)
    {
      std::rethrow_exception(captureStatusError);
    }
  };

  try
  {
    if (!replay)
    {
      device->start();
      device->process(buffer1, buffer2);

      // Async capture devices return after arming callbacks, so keep polling
      // capture state for the runtime lifetime.
      join_capture_status_thread();
      rethrow_capture_status_error();
      return;
    }
    else
    {
      device->replay(buffer1, buffer2, file, loop);
    }

    pollCaptureStatus.store(false);
    join_capture_status_thread();
    rethrow_capture_status_error();
  }
  catch (const std::exception &exception)
  {
    pollCaptureStatus.store(false);
    join_capture_status_thread();
    throw std::runtime_error("Capture " + type + " failed: "
      + exception.what());
  }
}

std::unique_ptr<Source> Capture::factory_source(const std::string& type, c4::yml::NodeRef config)
{
    // SDRplay RSPduo
    if (type == VALID_TYPE[0])
    {
        int agcSetPoint, bandwidthNumber, gainReductionA, gainReductionB, lnaState;
        bool dabNotch, rfNotch;
        config["agcSetPoint"] >> agcSetPoint;
        config["bandwidthNumber"] >> bandwidthNumber;
        config["gainReduction"][0] >> gainReductionA;
        config["gainReduction"][1] >> gainReductionB;
        config["lnaState"] >> lnaState;
        config["dabNotch"] >> dabNotch;
        config["rfNotch"] >> rfNotch;
        return std::make_unique<RspDuo>(type, fc, fs, path, &saveIq,
          agcSetPoint, bandwidthNumber, gainReductionA, gainReductionB, lnaState,
          dabNotch, rfNotch);
    }
    // Usrp
    else if (type == VALID_TYPE[1])
    {
        std::string address, subdev;
        std::vector<std::string> antenna;
        std::vector<double> gain;
        double bandwidth = static_cast<double>(fs);
        std::string _antenna;
        double _gain;
        config["address"] >> address;
        config["subdev"] >> subdev;
        config["antenna"][0] >> _antenna;
        antenna.push_back(_antenna);
        config["antenna"][1] >> _antenna;
        antenna.push_back(_antenna);
        config["gain"][0] >> _gain;
        gain.push_back(_gain);
        config["gain"][1] >> _gain;
        gain.push_back(_gain);
        config["bandwidth"] >> bandwidth;
        return std::make_unique<Usrp>(type, fc, fs, path, &saveIq, 
          address, subdev, antenna, gain, bandwidth);
    }
    // HackRF
    else if (type == VALID_TYPE[2])
    {
      std::vector<std::string> serial;
      std::vector<uint32_t> gainLna, gainVga;
      std::vector<bool> ampEnable;
      uint32_t bandwidth;
      std::string _serial;
      uint32_t gain;
      int _gain;
      bool _ampEnable;
      config["serial"][0] >> _serial;
      serial.push_back(_serial);
      config["serial"][1] >> _serial;
      serial.push_back(_serial);
      config["bandwidth"] >> bandwidth;
      config["gain_lna"][0] >> _gain;
      gain = static_cast<uint32_t> (_gain);
      gainLna.push_back(gain);
      config["gain_lna"][1] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainLna.push_back(gain);
      config["gain_vga"][0] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainVga.push_back(gain);
      config["gain_vga"][1] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainVga.push_back(gain);
      config["amp_enable"][0] >> _ampEnable;
      ampEnable.push_back(_ampEnable);
      config["amp_enable"][1] >> _ampEnable;
      ampEnable.push_back(_ampEnable);
      return std::make_unique<HackRf>(type, fc, fs, path, &saveIq,
        serial, gainLna, gainVga, ampEnable, bandwidth);
    }
    // Kraken
    else if (type == VALID_TYPE[3])
    {
      std::vector<double> gain;
      bool alignmentEnabled = true;
      uint32_t alignmentWindowCount = 3;
      int64_t lagConsensusToleranceSamples = 16;
      uint32_t driftCheckIntervalMinutes = 10;
      float _gain;
      for (auto child : config["gain"].children())
      {
        c4::atof(child.val(), &_gain);
        gain.push_back(static_cast<double>(_gain));
      }

      auto alignmentNode = config["alignment"];
      if (alignmentNode.valid())
      {
        auto alignmentEnabledNode = alignmentNode["enabled"];
        if (alignmentEnabledNode.valid())
        {
          alignmentEnabledNode >> alignmentEnabled;
        }

        auto alignmentWindowCountNode = alignmentNode["windowCount"];
        if (alignmentWindowCountNode.valid())
        {
          alignmentWindowCountNode >> alignmentWindowCount;
        }

        auto consensusToleranceNode = alignmentNode["consensusToleranceSamples"];
        if (consensusToleranceNode.valid())
        {
          consensusToleranceNode >> lagConsensusToleranceSamples;
        }

        auto recheckIntervalNode = alignmentNode["recheckIntervalMinutes"];
        if (recheckIntervalNode.valid())
        {
          recheckIntervalNode >> driftCheckIntervalMinutes;
        }
      }

      return std::make_unique<Kraken>(type, fc, fs, path, &saveIq, gain,
        alignmentEnabled,
        static_cast<size_t>(alignmentWindowCount), lagConsensusToleranceSamples,
        std::chrono::minutes(driftCheckIntervalMinutes));
    }
    // handle unknown type
    std::cerr << "Error: Source type does not exist." << std::endl;
    return nullptr;
}

void Capture::set_replay(bool _loop, std::string _file)
{
  replay = true;
  loop = _loop;
  file = _file;
}
