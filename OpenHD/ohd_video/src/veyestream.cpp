//
// Created by consti10 on 13.08.22.
//

#include "veyestream.h"
#include "OHDGstHelper.hpp"
#include "openhd-util.hpp"
#include "veye-helper.hpp"

VEYEStream::VEYEStream(PlatformType platform, std::shared_ptr<CameraHolder> camera_holder, uint16_t video_udp_port)
	: CameraStream(platform, camera_holder, video_udp_port) {
  m_console = openhd::loggers::create_or_get("ohd_veye");
  assert(m_console);
  m_console->set_level(spd::level::debug);
  m_console->debug("VEYEStream::VEYEStream()");
  const auto& camera=_camera_holder->get_camera();
  const auto& setting=_camera_holder->get_settings();
  assert(camera.type==CameraType::RaspberryPiVEYE);
  _camera_holder->register_listener([this](){
	// right now, every time the settings for this camera change, we just re-start the whole stream.
	// That is not ideal, since some cameras support changing for example the bitrate or white balance during operation.
	// But wiring that up is not that easy.
	this->restart_async();
  });
  // sanity checks
  assert(setting.userSelectedVideoFormat.isValid());
  m_console->debug("VEYEStream::VEYEStream");
}

// WARNING: Changing anything camera-related is r.n is really
void VEYEStream::setup() {
  m_console->debug("VEYEStream::setup() begin");
  // kill any already running veye instances
  m_console->debug("kill any already running veye instances");
  openhd::veye::kill_all_running_veye_instances();

  /*_camera_holder->unsafe_get_settings().userSelectedVideoFormat.width=1920;
  _camera_holder->unsafe_get_settings().userSelectedVideoFormat.height=1080;
  _camera_holder->unsafe_get_settings().userSelectedVideoFormat.framerate=30;*/

  // create the pipeline
  const auto& setting=_camera_holder->get_settings();
  std::stringstream ss;
  // http://wiki.veye.cc/index.php/VEYE-MIPI-290/327_for_Raspberry_Pi
  // Not ideal, needs full path, but veye is hacky anyways
  ss<<"/usr/local/share/veye-raspberrypi/veye_raspivid ";
  const int bitrateBitsPerSecond = kbits_to_bits_per_second(setting.bitrateKBits);
  //const int bitrateBitsPerSecond=4000000;

  ss<<"-b "<<bitrateBitsPerSecond<<" ";
  ss<<"-w "<<setting.userSelectedVideoFormat.width<<" ";
  ss<<"-h "<<setting.userSelectedVideoFormat.height<<" ";
  // -fps, --framerate	: Specify the frames per second to record
  ss<<"--framerate "<<setting.userSelectedVideoFormat.framerate<<" ";
  if(setting.userSelectedVideoFormat.videoCodec==VideoCodec::H264){
	ss<<"--codec H264 ";
	ss<<"--profile baseline ";
	// TODO check
	// -g, --intra	: Specify the intra refresh period (key frame rate/GoP size). Zero to produce an initial I-frame and then just P-frames.
	ss<<"--intra "<<setting.keyframe_interval<<" ";
  }else if(setting.userSelectedVideoFormat.videoCodec==VideoCodec::MJPEG){
	ss<<"--codec MJPEG ";
  }else{
	m_console->warn("Veye only supports h264 and MJPEG");
  }
   // flush to decrease latency
   // NOTE: flush seems to cause issues on VEYE, it is bugged / doesn't work.
  //ss<<"--flush ";
  // no preview
  ss<<"-n ";
  // forever
  ss<<"-t 0 -o - ";
  //ss<<"| gst-launch-1.0 -v fdsrc ! ";
  ss<<"| gst-launch-1.0 fdsrc ! ";
  ss<<OHDGstHelper::createRtpForVideoCodec(setting.userSelectedVideoFormat.videoCodec);
  ss<<OHDGstHelper::createOutputUdpLocalhost(_video_udp_port);
  pipeline=ss.str();
  // NOTE: We do not execute the pipeline until start() is called
  m_console->debug("Veye Pipeline:["+pipeline+"]");
  m_console->debug("VEYEStream::setup() end");
}

void VEYEStream::restartIfStopped() {
  // unimplemented for veye
}

void VEYEStream::start() {
  m_console->debug("VEYEStream::start() begin");
  // cleanup if existing
  openhd::veye::kill_all_running_veye_instances();
  // don't stream unless enabled
  if(!_camera_holder->get_settings().enable_streaming){
	m_console->debug("Streaming disabled");
	return;
  }
  // start streaming (process in the background)
  // run in background and pipe std::cout and std::cerr to file
  OHDUtil::run_command(pipeline,{"&> /tmp/veye.log &"});
  m_console->debug("VEYEStream::start() end");
}

void VEYEStream::stop() {
  openhd::veye::kill_all_running_veye_instances();
}

std::string VEYEStream::createDebug() {
  return "Veye has no debug state";
}

void VEYEStream::restart_async() {
  m_console->debug("VEYEStream::restart_async() begin");
  stop();
  setup();
  start();
  m_console->debug("VEYEStream::restart_async() end");
}
