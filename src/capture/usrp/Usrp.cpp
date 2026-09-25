#include "Usrp.h"

#include <cassert>
#include <string.h>
#include <iostream>
#include <limits>
#include <vector>
#include <complex>
#include <uhd/usrp/multi_usrp.hpp>

// constructor
Usrp::Usrp(std::string _type, uint32_t _fc, uint32_t _fs,
  std::string _path, std::atomic<bool> *_saveIq, std::string _address,
  std::string _subdev, std::vector<std::string> _antenna,
  std::vector<double> _gain)
    : Source(_type, _fc, _fs, _path, _saveIq)
{
  address = _address;
  subdev = _subdev;
  antenna = _antenna;
  gain = _gain;
}

void Usrp::start()
{
}

void Usrp::stop()
{
}

void Usrp::process(IqData *buffer1, IqData *buffer2)
{
    // create a USRP object
    uhd::usrp::multi_usrp::sptr usrp =
      uhd::usrp::multi_usrp::make(address);

    usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t(subdev), 0);

    usrp->set_rx_antenna(antenna[0], 0);
    usrp->set_rx_antenna(antenna[1], 1);

    // set sample rate across all channels
    usrp->set_rx_rate((double(fs)));

    // set the center frequency
    double centerFrequency = (double)fc;
    usrp->set_rx_freq(centerFrequency, 0);
    usrp->set_rx_freq(centerFrequency, 1);

    // set the gain
    usrp->set_rx_gain(gain[0], 0);
    usrp->set_rx_gain(gain[1], 1);

    // create a receive streamer
    uhd::stream_args_t streamArgs("fc32", "sc16");
    streamArgs.channels = {0, 1};
    uhd::rx_streamer::sptr rxStreamer = usrp->get_rx_stream(streamArgs);

    // allocate buffers to receive with samples (one buffer per channel)
    const size_t samps_per_buff = rxStreamer->get_max_num_samps();
    std::vector<std::complex<float>> usrpBuffer1(samps_per_buff);
    std::vector<std::complex<float>> usrpBuffer2(samps_per_buff);

    // pre-converted double-precision scratch for the bulk append path
    std::vector<std::complex<double>> appendBuffer1(samps_per_buff);
    std::vector<std::complex<double>> appendBuffer2(samps_per_buff);

    // create a vector of pointers to point to each of the channel buffers
    std::vector<std::complex<float>*> buff_ptrs;
    buff_ptrs.push_back(&usrpBuffer1.front());
    buff_ptrs.push_back(&usrpBuffer2.front());

    // setup stream
    uhd::rx_metadata_t metadata;
    uhd::stream_cmd_t streamCmd = uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS;
    streamCmd.stream_now = false;
    streamCmd.time_spec  = usrp->get_time_now() + uhd::time_spec_t(0.05);
    rxStreamer->issue_stream_cmd(streamCmd);

    while(true)
    {
      // receive samples
      size_t nReceived = rxStreamer->recv(buff_ptrs, samps_per_buff, metadata);

      // UHD guarantees nReceived <= samps_per_buff for a multi-channel recv().
      assert(nReceived <= samps_per_buff);

      // print errors
      if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
          std::cerr << "Error: " << metadata.strerror() << std::endl;
      }

      if (nReceived > 0)
      {
        // convert float -> double outside the lock to minimise contention
        for (size_t i = 0; i < nReceived; i++)
        {
          appendBuffer1[i] = {static_cast<double>(buff_ptrs[0][i].real()),
                              static_cast<double>(buff_ptrs[0][i].imag())};
          appendBuffer2[i] = {static_cast<double>(buff_ptrs[1][i].real()),
                              static_cast<double>(buff_ptrs[1][i].imag())};
        }

        buffer1->lock();
        buffer2->lock();
        buffer1->append(appendBuffer1.data(), static_cast<uint32_t>(nReceived));
        buffer2->append(appendBuffer2.data(), static_cast<uint32_t>(nReceived));
        buffer1->unlock_and_notify();
        buffer2->unlock_and_notify();
      }

      // save IQ data to file
      if (saveIq != nullptr && saveIq->load())
      {
        constexpr double kUsrpIqScale = static_cast<double>(std::numeric_limits<int16_t>::max());
        write_blah2_iq_samples(buff_ptrs[0], buff_ptrs[1], nReceived, kUsrpIqScale);
      }
    }
}

void Usrp::replay(IqData *buffer1, IqData *buffer2, std::string _file, bool _loop)
{
  replay_blah2_iq_file(buffer1, buffer2, _file, _loop);
}
