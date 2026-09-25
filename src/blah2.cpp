/// @file blah2.cpp
/// @brief A real-time radar.
/// @author 30hours

#include "capture/Capture.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/meta/Timing.h"
#include "data/Track.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/clutter/WienerHopf.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include "process/tracker/Tracker.h"
#include "process/utility/Socket.h"
#include "data/meta/Constants.h"

#include <httplib.h>
#include <ryml/ryml.hpp>
#include <ryml/ryml_std.hpp> // optional header, provided for std:: interop
#include <c4/format.hpp> // needed for the examples below
#include <sys/types.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <exception>
#include <thread>
#include <chrono>
#include <sys/time.h>
#include <signal.h>
#include <atomic>
#include <memory>
#include <iostream>
#include <mutex>
#include <limits>
#include <cmath>
#include <cstdio>

Capture *CAPTURE_POINTER = NULL;
std::unique_ptr<Socket> socket_map;
std::unique_ptr<Socket> socket_detection;
std::unique_ptr<Socket> socket_track;
std::unique_ptr<Socket> socket_timestamp;
std::unique_ptr<Socket> socket_timing;
std::unique_ptr<Socket> socket_iqdata;
std::unique_ptr<Socket> socket_adsb;

void signal_callback_handler(int signum);
void getopt_print_help();
std::string getopt_process(int argc, char **argv);
std::string ryml_get_file(const char *filename);
std::string adsb_sidecar_path_from_iq_file(const std::string &iqFile);
bool append_json_array_entry(const std::string &json, const std::string &filename);
uint64_t current_time_ms();
uint64_t current_time_us();
void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name);

int main(int argc, char **argv)
{
  // input handling
  signal(SIGTERM, signal_callback_handler);
  std::string file = getopt_process(argc, argv);
  std::ifstream filePath(file);
  if (!filePath.is_open())
  {
    std::cout << "Error: Config file does not exist." << "\n";
    exit(1);
  }

  // config handling
  std::string contents = ryml_get_file(file.c_str());
  ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(contents));

  // set up capture
  uint32_t fs, fc;
  uint16_t port_capture;
  std::string type, path, replayFile, ip_capture;
  bool saveIq, state, loop;
  tree["capture"]["fs"] >> fs;
  tree["capture"]["fc"] >> fc;
  tree["capture"]["device"]["type"] >> type;
  tree["save"]["iq"] >> saveIq;
  tree["save"]["path"] >> path;
  tree["capture"]["replay"]["state"] >> state;
  tree["capture"]["replay"]["loop"] >> loop;
  tree["capture"]["replay"]["file"] >> replayFile;
  tree["network"]["ip"] >> ip_capture;
  tree["network"]["ports"]["api"] >> port_capture;

  // set up socket
  sleep(2);
  uint16_t port_map, port_detection, port_timestamp, 
    port_timing, port_iqdata, port_track, port_adsb;
  std::string ip;
  tree["network"]["ports"]["map"] >> port_map;
  tree["network"]["ports"]["detection"] >> port_detection;
  tree["network"]["ports"]["track"] >> port_track;
  tree["network"]["ports"]["timestamp"] >> port_timestamp;
  tree["network"]["ports"]["timing"] >> port_timing;
  tree["network"]["ports"]["iqdata"] >> port_iqdata;
  tree["network"]["ports"]["adsb"] >> port_adsb;
  tree["network"]["ip"] >> ip;
  
  try {
    socket_map = std::make_unique<Socket>(ip, port_map);
    socket_detection = std::make_unique<Socket>(ip, port_detection);
    socket_track = std::make_unique<Socket>(ip, port_track);
    socket_timestamp = std::make_unique<Socket>(ip, port_timestamp);
    socket_timing = std::make_unique<Socket>(ip, port_timing);
    socket_iqdata = std::make_unique<Socket>(ip, port_iqdata);
    socket_adsb = std::make_unique<Socket>(ip, port_adsb);
  } catch (const std::exception& e) {
    std::cerr << "Failed to initialize socket connections: " << e.what() << "\n";
    std::cerr << "Make sure the server at " << ip << " is reachable." << "\n";
    return 1;
  }

  // set up fftw multithread
  if (fftw_init_threads() == 0)
  {
    std::cout << "Error in FFTW multithreading." << "\n";
    return -1;
  }
  fftw_plan_with_nthreads(4);

  Capture *capture = new Capture(type, fs, fc, path);
  CAPTURE_POINTER = capture;
  if (state)
  {
    capture->set_replay(loop, replayFile);
  }

  // create shared queue
  double tCpi, tBuffer;
  tree["process"]["data"]["cpi"] >> tCpi;
  tree["process"]["data"]["buffer"] >> tBuffer;
  if (fs == 0 || !std::isfinite(tCpi) || tCpi <= 0.0 ||
    !std::isfinite(tBuffer) || tBuffer <= 0.0)
  {
    std::cerr << "Invalid process.data config: fs, cpi and buffer must be positive finite values" << "\n";
    return -1;
  }

  const double samplesPerCpi = static_cast<double>(fs) * tCpi;
  const double samplesPerBuffer = samplesPerCpi * tBuffer;
  if (samplesPerCpi < 1.0 || samplesPerBuffer < 1.0 ||
    samplesPerCpi > std::numeric_limits<uint32_t>::max() ||
    samplesPerBuffer > std::numeric_limits<uint32_t>::max())
  {
    std::cerr << "Invalid process.data config: derived sample counts must be between 1 and "
      << std::numeric_limits<uint32_t>::max() << "\n";
    return -1;
  }

  uint32_t nSamples = static_cast<uint32_t>(samplesPerCpi);
  uint32_t bufferSamples = static_cast<uint32_t>(samplesPerBuffer);
  if (bufferSamples < nSamples)
  {
    std::cerr << "Invalid process.data.buffer config: buffer must hold at least one CPI"
      << " (required " << nSamples << " samples, got " << bufferSamples << ")" << "\n";
    return -1;
  }

  IqData *buffer1 = new IqData(bufferSamples);
  IqData *buffer2 = new IqData(bufferSamples);

  // run capture
  std::thread t1([&]{
    try
    {
      capture->process(buffer1, buffer2,
        tree["capture"]["device"], ip_capture, port_capture);
    }
    catch (const std::exception &exception)
    {
      std::cerr << "Capture process failed: " << exception.what() << "\n";
      std::exit(EXIT_FAILURE);
    }
  });

  // set up process CPI
  IqData *x = new IqData(nSamples);
  IqData *y = new IqData(nSamples);
  std::vector<std::complex<double>> cpiScratch1(nSamples);
  std::vector<std::complex<double>> cpiScratch2(nSamples);
  Map<std::complex<double>> *map;
  std::unique_ptr<Detection> detection;
  std::unique_ptr<Detection> detection1;
  std::unique_ptr<Detection> detection2;
  std::unique_ptr<Track> track;

  // set up process ambiguity
  int32_t delayMin, delayMax;
  int32_t dopplerMin, dopplerMax;
  bool roundHamming = true;
  tree["process"]["ambiguity"]["delayMin"] >> delayMin;
  tree["process"]["ambiguity"]["delayMax"] >> delayMax;
  tree["process"]["ambiguity"]["dopplerMin"] >> dopplerMin;
  tree["process"]["ambiguity"]["dopplerMax"] >> dopplerMax;
  Ambiguity *ambiguity = new Ambiguity(delayMin, delayMax, 
    dopplerMin, dopplerMax, fs, nSamples, roundHamming);

  // set up process clutter
  int32_t delayMinClutter, delayMaxClutter;
  double diagonalLoadScale = 1e-6;
  tree["process"]["clutter"]["delayMin"] >> delayMinClutter;
  tree["process"]["clutter"]["delayMax"] >> delayMaxClutter;
  auto diagonalLoadNode = tree["process"]["clutter"]["diagonalLoadScale"];
  if (diagonalLoadNode.valid())
  {
    diagonalLoadNode >> diagonalLoadScale;
  }
  if (!std::isfinite(diagonalLoadScale) || diagonalLoadScale < 0.0)
  {
    std::cerr << "Invalid process.clutter.diagonalLoadScale config: must be a finite non-negative value" << "\n";
    return -1;
  }
  WienerHopf *filter = new WienerHopf(delayMinClutter, delayMaxClutter, nSamples,
    diagonalLoadScale);

  // set up process detection
  double pfa, minDoppler;
  int8_t nGuard, nTrain;
  int8_t minDelay;
  std::string cfarModeString = "CA";
  CfarMode cfarMode = CfarMode::CA;
  tree["process"]["detection"]["pfa"] >> pfa;
  tree["process"]["detection"]["nGuard"] >> nGuard;
  tree["process"]["detection"]["nTrain"] >> nTrain;
  tree["process"]["detection"]["minDelay"] >> minDelay;
  tree["process"]["detection"]["minDoppler"] >> minDoppler;
  auto cfarModeNode = tree["process"]["detection"]["cfarMode"];
  if (cfarModeNode.valid())
  {
    cfarModeNode >> cfarModeString;
  }
  if (cfarModeString == "CAGO")
  {
    cfarMode = CfarMode::CAGO;
  }
  else if (cfarModeString != "CA")
  {
    std::cout << "Warning: Unsupported cfarMode '" << cfarModeString << "'. Falling back to CA." << "\n";
    cfarMode = CfarMode::CA;
  }
  // set up exclusion zones
  bool exclusionZonesEnabled = false;
  std::vector<ExclusionZone> exclusionZones;
  auto exclusionZonesNode = tree["process"]["detection"]["exclusionZones"];
  if (exclusionZonesNode.valid())
  {
    exclusionZonesNode["enabled"] >> exclusionZonesEnabled;
    if (exclusionZonesEnabled)
    {
      auto zonesNode = tree["process"]["detection"]["exclusionZones"]["zones"];
      if (zonesNode.valid() && zonesNode.is_seq())
      {
        const double delayScaleBins = (fs > 0) ? (static_cast<double>(fs) / static_cast<double>(Constants::c)) : 0.0;
        for (const auto &zoneChild : zonesNode.children())
        {
          ExclusionZone zone;
          double delayMinMeters = 0.0, delayMaxMeters = 0.0;
          zoneChild["delayMin"] >> delayMinMeters;
          zoneChild["delayMax"] >> delayMaxMeters;
          zoneChild["dopplerMin"] >> zone.dopplerMinHz;
          zoneChild["dopplerMax"] >> zone.dopplerMaxHz;
          zone.delayMinBins = delayMinMeters * delayScaleBins;
          zone.delayMaxBins = delayMaxMeters * delayScaleBins;
          exclusionZones.push_back(zone);
        }
      }
    }
  }
  CfarDetector1D *cfarDetector1D = new CfarDetector1D(pfa, nGuard, nTrain, minDelay, minDoppler, cfarMode,
    exclusionZonesEnabled, std::move(exclusionZones));
  Interpolate *interpolate = new Interpolate(true, true);

  // set up process centroid
  uint16_t nCentroid;
  CentroidMode centroidMode = CentroidMode::LocalPeak;
  tree["process"]["detection"]["nCentroid"] >> nCentroid;
  auto centroidModeNode = tree["process"]["detection"]["postProcessMode"];
  if (centroidModeNode.valid())
  {
    std::string centroidModeString;
    centroidModeNode >> centroidModeString;
    if (!try_parse_centroid_mode(centroidModeString, centroidMode))
    {
      throw std::runtime_error("Unsupported process.detection.postProcessMode '" + centroidModeString + "'");
    }
  }
  Centroid *centroid = new Centroid(nCentroid, nCentroid, 1/tCpi, centroidMode);

  // set up process tracker
  uint8_t m, n, nDelete;
  double maxAcc, rangeRes, lambda;
  std::string smooth;
  tree["process"]["tracker"]["initiate"]["M"] >> m;
  tree["process"]["tracker"]["initiate"]["N"] >> n;
  tree["process"]["tracker"]["delete"] >> nDelete;
  tree["process"]["tracker"]["initiate"]["maxAcc"] >> maxAcc;
  rangeRes = (double)Constants::c/fs;
  lambda = (double)Constants::c/fc;
  Tracker *tracker = new Tracker(m, n, nDelete, ambiguity->get_cpi(), maxAcc, rangeRes, lambda);

  // set up ADS-B ingestion if available
  bool isAdsb = false;
  std::string adsbHost;
  std::string tar1090;
  double rxLatitude = 0.0, rxLongitude = 0.0, rxAltitude = 0.0;
  double txLatitude = 0.0, txLongitude = 0.0, txAltitude = 0.0;
  tree["truth"]["adsb"]["enabled"] >> isAdsb;
  tree["truth"]["adsb"]["adsb2dd"] >> adsbHost;
  tree["truth"]["adsb"]["tar1090"] >> tar1090;
  tree["location"]["rx"]["latitude"] >> rxLatitude;
  tree["location"]["rx"]["longitude"] >> rxLongitude;
  tree["location"]["rx"]["altitude"] >> rxAltitude;
  tree["location"]["tx"]["latitude"] >> txLatitude;
  tree["location"]["tx"]["longitude"] >> txLongitude;
  tree["location"]["tx"]["altitude"] >> txAltitude;

  // keep latest ADS-B payload ready outside CPI-critical loop
  std::mutex adsbMutex;
  std::string cachedAdsbJson = "{}";
  if (isAdsb && !adsbHost.empty())
  {
    std::thread([&]() {
      httplib::Client cli(("http://" + adsbHost).c_str());
      while (true)
      {
        std::ostringstream query;
        query << "/api/dd?rx=" << rxLatitude << "," << rxLongitude << "," << rxAltitude;
        query << "&tx=" << txLatitude << "," << txLongitude << "," << txAltitude;
        query << "&fc=" << (fc / 1000000.0);
        query << "&server=" << "http://" << tar1090;

        if (auto res = cli.Get(query.str().c_str()))
        {
          if (res->status == 200 && !res->body.empty())
          {
            std::lock_guard<std::mutex> lock(adsbMutex);
            cachedAdsbJson = res->body;
          }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }).detach();
  }

  // set up process spectrum analyser
  double spectrumBandwidth = 2000;
  SpectrumAnalyser *spectrumAnalyser = new SpectrumAnalyser(nSamples, spectrumBandwidth, static_cast<double>(fc));

  // process options
  bool isClutter, isDetection, isTracker;
  tree["process"]["clutter"]["enable"] >> isClutter;
  tree["process"]["detection"]["enable"] >> isDetection;
  tree["process"]["tracker"]["enable"] >> isTracker;
  if (!isDetection)
  {
    isTracker = false;
  }

  // set up output data
  bool saveMap, saveDetection;
  tree["save"]["map"] >> saveMap;
  tree["save"]["detection"] >> saveDetection;
  std::string savePath, saveMapPath, saveDetectionPath;
  if (saveIq || saveMap || saveDetection)
  {
    char startTimeStr[16];
    struct timeval currentTime = {0, 0};
    gettimeofday(&currentTime, NULL);
    strftime(startTimeStr, 16, "%Y%m%d-%H%M%S", localtime(&currentTime.tv_sec));
    savePath = path + startTimeStr;
  }
  if (saveMap)
  {
    saveMapPath = savePath + ".map";
  }
  if (saveDetection)
  {
    saveDetectionPath = savePath + ".detection";
  }
  std::string lastAdsbSavePath;
  bool adsbCaptureMetadataWritten = false;
  bool warnedAdsbSaveFailure = false;

  // set up output timing
  uint64_t tStart = current_time_ms();
  Timing *timing = new Timing(tStart);
  std::vector<std::string> timing_name;
  std::vector<double> timing_time;
  std::string jsonTiming;
  std::vector<uint64_t> time;

  // set up output json
  std::string mapJson, detectionJson, jsonTracker, jsonIqData;

  // run process
  std::thread t2([&]{
      while (true)
      {
        buffer1->wait_for_min_length(nSamples);
        buffer2->wait_for_min_length(nSamples);

        buffer1->lock();
        buffer2->lock();
        if ((buffer1->get_length() >= nSamples) && (buffer2->get_length() >= nSamples))
        {
          time.push_back(current_time_us());
          // extract data from buffer (bulk memcpy to minimise lock hold time)
          buffer1->pop_into(cpiScratch1.data(), nSamples);
          buffer2->pop_into(cpiScratch2.data(), nSamples);
          buffer1->unlock_and_notify();
          buffer2->unlock_and_notify();

          x->clear();
          y->clear();
          x->append(cpiScratch1.data(), nSamples);
          y->append(cpiScratch2.data(), nSamples);
          timing_helper(timing_name, timing_time, time, "extract_buffer");
          
          // spectrum
          spectrumAnalyser->process(x, y);
          timing_helper(timing_name, timing_time, time, "spectrum");
          
          // clutter filter
          if (isClutter)
          {
            if (!filter->process(x, y))
            {
              continue;
            }
            timing_helper(timing_name, timing_time, time, "clutter_filter");
          }
          
          // ambiguity process
          map = ambiguity->process(x, y);
          map->set_metrics();
          timing_helper(timing_name, timing_time, time, "ambiguity_processing");
          
          // detection process
          if (isDetection)
          {
            // build allowed zones from active tracks to override exclusion zones
            if (isTracker)
            {
              // Gate values match the tracker's Hungarian assignment gates
              // (Tracker.cpp lines 179-180).  Keeping them identical ensures
              // that a detection let through by the CFAR override is also
              // within range for track association on the next CPI.
              const double delayGateBins = 3.0;
              const double dopplerGateHz = 3.0 * (1.0 / tCpi);
              thread_local std::vector<ExclusionZone> allowedZones;
              allowedZones.clear();
              for (const auto &pos : tracker->get_active_track_positions())
              {
                const double delayBins = pos.get_delay().front();
                const double dopplerHz = pos.get_doppler().front();
                // ExclusionZone struct reused as a gate window around an
                // ACTIVE track — detections inside this region override
                // the exclusion zone suppression.
                allowedZones.push_back({delayBins - delayGateBins, delayBins + delayGateBins,
                  dopplerHz - dopplerGateHz, dopplerHz + dopplerGateHz});
              }
              cfarDetector1D->set_allowed_zones(allowedZones);
            }
            detection1 = cfarDetector1D->process(map);
            detection2 = centroid->process(detection1.get(), map);
            detection = interpolate->process(detection2.get(), map);
            timing_helper(timing_name, timing_time, time, "detector");
          }

          // tracker process
          if (isTracker)
          {
            track = tracker->process(detection.get(), time[0]/1000);
            timing_helper(timing_name, timing_time, time, "tracker");
          }

          // output IqData meta data
          jsonIqData = x->to_json(time[0]/1000);
          socket_iqdata->sendData(jsonIqData);
          timing_helper(timing_name, timing_time, time, "output_iqdata");

          // output map data
          mapJson = map->to_json(time[0]/1000, fs, true);
          if (saveMap)
          {
            map->save(mapJson, saveMapPath);
          }
          socket_map->sendData(mapJson);
          timing_helper(timing_name, timing_time, time, "output_map");

          // output detection data
          if (isDetection)
          {
            detectionJson = detection->to_json(time[0]/1000, fs, true);
            socket_detection->sendData(detectionJson);
            timing_helper(timing_name, timing_time, time, "output_detection");
          }
          if (isDetection && saveDetection)
          {
            detection->save(detectionJson, saveDetectionPath);
          }

          // output tracker data
          if (isTracker)
          {
            jsonTracker = track->to_json(time[0]/1000, fs, true);
            socket_track->sendData(jsonTracker);
            timing_helper(timing_name, timing_time, time, "output_tracker");
          }

          // output latest ADS-B truth cached by a background thread
          std::string jsonAdsb = "{}";
          if (isAdsb)
          {
            std::lock_guard<std::mutex> lock(adsbMutex);
            jsonAdsb = cachedAdsbJson;
          }
          const bool iqCaptureActive = CAPTURE_POINTER != nullptr && CAPTURE_POINTER->is_saving_iq();
          const Capture::ActiveIqCapture iqCapture =
            (CAPTURE_POINTER != nullptr) ? CAPTURE_POINTER->get_active_iq_capture() : Capture::ActiveIqCapture{};
          const std::string adsbSavePath = adsb_sidecar_path_from_iq_file(iqCapture.file);
          if (adsbSavePath != lastAdsbSavePath)
          {
            lastAdsbSavePath = adsbSavePath;
            adsbCaptureMetadataWritten = false;
            warnedAdsbSaveFailure = false;
          }
          if (isAdsb && iqCaptureActive && !adsbSavePath.empty())
          {
            bool sidecarReady = true;

            // Persist capture-start metadata once so replay alignment does not
            // depend on local-time filename parsing.
            if (!adsbCaptureMetadataWritten)
            {
              std::ostringstream adsbMetadata;
              adsbMetadata << "{\"captureStartMs\":" << iqCapture.startMs << "}";
              if (!append_json_array_entry(adsbMetadata.str(), adsbSavePath))
              {
                if (!warnedAdsbSaveFailure)
                {
                  std::cerr << "Warning: Failed to save ADS-B snapshots to " << adsbSavePath << "\n";
                  warnedAdsbSaveFailure = true;
                }
                sidecarReady = false;
              }
              else
              {
                adsbCaptureMetadataWritten = true;
              }
            }

            if (sidecarReady)
            {
              // Keep the ADS-B sidecar aligned one-to-one with each IQ capture file.
              std::ostringstream adsbSnapshot;
              adsbSnapshot << "{\"timestamp\":" << time[0]/1000 << ",\"targets\":" << jsonAdsb << "}";
              if (!append_json_array_entry(adsbSnapshot.str(), adsbSavePath) && !warnedAdsbSaveFailure)
              {
                std::cerr << "Warning: Failed to save ADS-B snapshots to " << adsbSavePath << "\n";
                warnedAdsbSaveFailure = true;
              }
            }
          }
          socket_adsb->sendData(jsonAdsb);
          timing_helper(timing_name, timing_time, time, "output_adsb");

          // output radar data timer
          timing_helper(timing_name, timing_time, time, "output_radar_data");

          // cpi timer
          time.push_back(current_time_us());
          double delta_ms = (double)(time.back()-time[0]) / 1000;
          timing_name.push_back("cpi");
          timing_time.push_back(delta_ms);
          std::cout << "CPI time (ms): " << delta_ms << "\n";

          // output timing data
          timing->update(time[0]/1000, timing_time, timing_name);
          jsonTiming = timing->to_json();
          socket_timing->sendData(jsonTiming);
          timing_time.clear();
          timing_name.clear();

          // output CPI timestamp for updating data
          std::string t0_string = std::to_string(time[0]/1000);
          socket_timestamp->sendData(t0_string);
          time.clear();

        }
        else
        {
          buffer1->unlock();
          buffer2->unlock();
        }
      }
    });
  t2.join();
  t1.join();

  return 0;
}

void signal_callback_handler(int signum) {
  std::cout << "Caught signal " << signum << "\n";
  if (CAPTURE_POINTER != nullptr)
  {
    CAPTURE_POINTER->device->kill();
  }
  else
  {
    exit(0);
  }
}

void getopt_print_help()
{
  std::cout << "--config <file.yml>: 	Set number of program\n"
               "--help:              	Show help\n";
  exit(1);
}

std::string getopt_process(int argc, char **argv)
{
  const char *const short_opts = "c:h";
  const option long_opts[] = {
      {"config", required_argument, nullptr, 'c'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, no_argument, nullptr, 0}};

  if (argc == 1)
  {
    std::cout << "Error: No arguments provided." << "\n";
    exit(1);
  }

  std::string file;

  while (true)
  {
    const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);

    // handle input "-", ":", etc
    if ((argc == 2) && (-1 == opt))
    {
      std::cout << "Error: No arguments provided." << "\n";
      exit(1);
    }

    if (-1 == opt)
      break;

    switch (opt)
    {
    case 'c':
      file = std::string(optarg);
      break;

    case 'h':
      getopt_print_help();

    // unrecognised option
    case '?':
      exit(1);

    default:
      break;
    }
  }

  return file;
}

std::string ryml_get_file(const char *filename)
{
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  if (!in)
  {
    std::cerr << "could not open " << filename << "\n";
    exit(1);
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::string adsb_sidecar_path_from_iq_file(const std::string &iqFile)
{
  if (iqFile.empty())
  {
    return "";
  }

  const std::string extension = ".iq";
  if (iqFile.size() >= extension.size() &&
    iqFile.compare(iqFile.size() - extension.size(), extension.size(), extension) == 0)
  {
    return iqFile.substr(0, iqFile.size() - extension.size()) + ".adsb";
  }

  return iqFile + ".adsb";
}

bool append_json_array_entry(const std::string &json, const std::string &filename)
{
  if (FILE *file = std::fopen(filename.c_str(), "r"); file == nullptr)
  {
    file = std::fopen(filename.c_str(), "w");
    if (file == nullptr)
    {
      return false;
    }
    std::fputs("[]", file);
    std::fclose(file);
  }
  else
  {
    std::fclose(file);
  }

  if (FILE *file = std::fopen(filename.c_str(), "rb+"); file != nullptr)
  {
    std::fseek(file, 0, SEEK_SET);
    if (std::getc(file) != '[')
    {
      std::fclose(file);
      return false;
    }

    bool isEmpty = false;
    if (std::getc(file) == ']')
    {
      isEmpty = true;
    }

    std::fseek(file, -1, SEEK_END);
    if (std::getc(file) != ']')
    {
      std::fclose(file);
      return false;
    }

    std::fseek(file, -1, SEEK_END);
    if (!isEmpty)
    {
      std::fputc(',', file);
    }

    std::fwrite(json.c_str(), sizeof(char), json.length(), file);
    std::fputc(']', file);
    std::fclose(file);
    return true;
  }

  return false;
}

uint64_t current_time_ms()
{
  // current time in POSIX ms
  return std::chrono::duration_cast<std::chrono::milliseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t current_time_us()
{
  // current time in POSIX us
  return std::chrono::duration_cast<std::chrono::microseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}

void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name)
{
  time_us.push_back(current_time_us());
  double delta_ms = (double)(time_us.back()-time_us[time_us.size()-2]) / 1000;
  timing_name.push_back(name);
  timing_time.push_back(delta_ms);
}
