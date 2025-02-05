#include "WBStreams.h"
#include "WifiCardCommandHelper.hpp"

#include "openhd-platform.hpp"
#include "openhd-log.hpp"
#include "OHDWifiCard.hpp"
#include "openhd-global-constants.hpp"

#include <iostream>
#include <utility>

WBStreams::WBStreams(OHDProfile profile,OHDPlatform platform,std::vector<std::shared_ptr<WifiCardHolder>> broadcast_cards1) :
   _profile(std::move(profile)),_platform(platform),_broadcast_cards(std::move(broadcast_cards1)),m_disable_all_frequency_checks(OHDFilesystemUtil::exists(FIlE_DISABLE_ALL_FREQUENCY_CHECKS)) {
  m_console = openhd::loggers::create_or_get("ohd_wb_streams");
  assert(m_console);
  m_console->set_level(spd::level::debug);
  m_console->debug("WBStreams::WBStreams: {}",_broadcast_cards.size());
  m_console->debug("WBStreams::m_disable_all_frequency_checks:"+OHDUtil::yes_or_no(m_disable_all_frequency_checks));
  // sanity checks
  if(_broadcast_cards.empty()) {
    // NOTE: Here we crash, since it would be a programmer(s) error to create a WBStreams instance without at least 1 wifi card.
    // In OHDInterface, we handle it more gracefully with an error code.
    m_console->error("Without at least one wifi card, the stream(s) cannot be started");
    exit(1);
  }
  // more than 4 cards would be completely insane, most likely a programming error
  assert(_broadcast_cards.size()<=4);
  // sanity checking
  for(const auto& card: _broadcast_cards){
    assert(card->get_settings().use_for==WifiUseFor::MonitorMode);
	assert(card->_wifi_card.supports_2ghz || card->_wifi_card.supports_5ghz);
  }
  if (_profile.is_air && _broadcast_cards.size() > 1) {
    // We cannot use more than 1 wifi card for injection
    m_console->warn("dangerous, the air unit should not have more than 1 wifi card for wifibroadcast");
    // We just use the first one, this points to a upper level programming error or something weird.
    _broadcast_cards.resize(1);
  }
  // this fetches the last settings, otherwise creates default ones
  _settings=std::make_unique<openhd::WBStreamsSettingsHolder>(openhd::tmp_convert(_broadcast_cards));
  // check if the cards connected match the previous settings.
  // For now, we check if the first wb card can do 2 / 4 ghz, and assume the rest can do the same
  const auto first_card=_broadcast_cards.at(0)->_wifi_card;
  if(_settings->get_settings().configured_for_2G()){
	if(! first_card.supports_2ghz){
	  // we need to switch to 5ghz, since the connected card cannot do 2ghz
	  m_console->warn("WB configured for 2G but card can only do 5G - overwriting old settings");
	  _settings->unsafe_get_settings().wb_channel_width=openhd::DEFAULT_CHANNEL_WIDTH;
	  _settings->unsafe_get_settings().wb_frequency=openhd::DEFAULT_5GHZ_FREQUENCY;
	  _settings->persist();
	}
  }else{
	if(!first_card.supports_5ghz){
	  // similar, we need to switch to 2G
	  m_console->warn("WB configured for 5G but card can only do 2G - overwriting old settings");
	  _settings->unsafe_get_settings().wb_channel_width=openhd::DEFAULT_CHANNEL_WIDTH;
	  _settings->unsafe_get_settings().wb_frequency=openhd::DEFAULT_2GHZ_FREQUENCY;
	  _settings->persist();
	}
  }
  takeover_cards();
  configure_cards();
  configure_streams();
}

void WBStreams::takeover_cards() {
  m_console->debug( "WBStreams::takeover_cards() begin");
  // We need to take "ownership" from the system over the cards used for monitor mode / wifibroadcast.
  // However, with the image set up by raphael they should be free from any (OS) prcoesses already
  // R.N we also try and blacklist the cards from NetworkManager - it is needed for RPI and Ubuntu
  // (as of 6.8.2022, since we re-added NetworkManager on pi) but it won't hurt on systems that don't
  // have network manager - there it just won't have any effect.
  // --
  // This is only needed if NetworkManager was not disabled (we might move onto leaving it enabled on all platforms, though)
  // TODO: does this return immediately or only after the change has been applied ?
  for(const auto& card: _broadcast_cards){
    WifiCardCommandHelper::network_manager_set_card_unmanaged(card->_wifi_card);
  }
  OHDUtil::run_command("rfkill",{"unblock","all"});
  // TODO: sometimes this happens:
  // 1) Running openhd fist time: pcap_compile doesn't work (fatal error)
  // 2) Running openhd second time: works
  // I cannot find what's causing the issue - a sleep here is the worst solution, but r.n the only one I can come up with
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // for now limited to the pi, since it breaks other kinds of connectivity
  if(_platform.platform_type==PlatformType::RaspberryPi){
    //OHDUtil::run_command("airmon-ng",{"check","kill"});
  }
  m_console->debug("WBStreams::takeover_cards() end");
}

void WBStreams::configure_cards() {
  m_console->debug("WBStreams::configure_cards() begin");
  if(OHDUtil::get_ohd_env_variable_bool("OHD_SKIP_WB_CONFIGURE_CARDS")){
	// This is for debugging / testing new wifi drivers that need a special startup method.
	// Note that here the developer has to configure the cards right before starting openhd, which
	// needs knowledge of wifibroadcast and its quirks.
	m_console->debug("WBStreams::configure_cards() skipping");
	return;
  }
  for(const auto& card: _broadcast_cards){
	//TODO we might not need this one
	//OHDUtil::run_command("rfkill",{"unblock",card->_wifi_card.interface_name});
	WifiCardCommandHelper::set_card_state(card->_wifi_card, false);
	WifiCardCommandHelper::enable_monitor_mode(card->_wifi_card);
	WifiCardCommandHelper::set_card_state(card->_wifi_card, true);
	const bool width_40=_settings->get_settings().wb_channel_width==40;
	//WifiCardCommandHelper::set_frequency(card->_wifi_card, _settings->get_settings().wb_frequency);
	WifiCardCommandHelper::set_frequency_and_channel_width(card->_wifi_card, _settings->get_settings().wb_frequency,width_40);
	// TODO check if this works - on rtl8812au, the displayed value at least changes
	// Not sure which is better, iw dev or iwconfig. However, iwconfig eats it in mW
	WifiCardCommandHelper::set_txpower2(card->_wifi_card, _settings->get_settings().wb_tx_power_milli_watt);
	//WifiCards::set_txpower(card->_wifi_card, card->get_settings().txpower);
  }
  m_console->debug("WBStreams::configure_cards() end");
}

void WBStreams::configure_streams() {
  m_console->debug("Streams::configure() begin");
  // Increase the OS max UDP buffer size (only works as root) such that the wb video UDP receiver
  // doesn't fail when requesting a bigger UDP buffer size
  OHDUtil::run_command("sysctl ",{"-w","net.core.rmem_max=26214400"});
  // Static for the moment
  configure_telemetry();
  configure_video();
  m_console->debug("Streams::configure() end");
}

void WBStreams::configure_telemetry() {
  m_console->debug("Streams::configure_telemetry()isAir:"+OHDUtil::yes_or_no(_profile.is_air));
  // Setup the tx & rx instances for telemetry. Telemetry is bidirectional,aka
  // uses 2 UDP streams in oposite directions.
  auto radioPort1 =
      _profile.is_air ? OHD_TELEMETRY_WIFIBROADCAST_RX_RADIO_PORT : OHD_TELEMETRY_WIFIBROADCAST_TX_RADIO_PORT;
  auto radioPort2 =
      _profile.is_air ? OHD_TELEMETRY_WIFIBROADCAST_TX_RADIO_PORT : OHD_TELEMETRY_WIFIBROADCAST_RX_RADIO_PORT;
  auto udpPort1 = _profile.is_air ? OHD_TELEMETRY_WIFIBROADCAST_LOCAL_UDP_PORT_GROUND_TX
								 : OHD_TELEMETRY_WIFIBROADCAST_LOCAL_UDP_PORT_GROUND_RX;
  auto udpPort2 = _profile.is_air ? OHD_TELEMETRY_WIFIBROADCAST_LOCAL_UDP_PORT_GROUND_RX
								 : OHD_TELEMETRY_WIFIBROADCAST_LOCAL_UDP_PORT_GROUND_TX;
  udpTelemetryRx = createUdpWbRx(radioPort1, udpPort1);
  udpTelemetryTx = createUdpWbTx(radioPort2, udpPort2,false);
  udpTelemetryRx->runInBackground();
  udpTelemetryTx->runInBackground();
}

void WBStreams::configure_video() {
  m_console->debug("Streams::configure_video()");
  // Video is unidirectional, aka always goes from air pi to ground pi
  if (_profile.is_air) {
	auto primary = createUdpWbTx(OHD_VIDEO_PRIMARY_RADIO_PORT, OHD_VIDEO_AIR_VIDEO_STREAM_1_UDP,true,1024*1024*25);
	primary->runInBackground();
	auto secondary = createUdpWbTx(OHD_VIDEO_SECONDARY_RADIO_PORT, OHD_VIDEO_AIR_VIDEO_STREAM_2_UDP,true,1024*1024*10);
	secondary->runInBackground();
	udpVideoTxList.push_back(std::move(primary));
	udpVideoTxList.push_back(std::move(secondary));
  } else {
	auto primary = createUdpWbRx(OHD_VIDEO_PRIMARY_RADIO_PORT, OHD_VIDEO_GROUND_VIDEO_STREAM_1_UDP);
	primary->runInBackground();
	auto secondary = createUdpWbRx(OHD_VIDEO_SECONDARY_RADIO_PORT, OHD_VIDEO_GROUND_VIDEO_STREAM_2_UDP);
	secondary->runInBackground();
	udpVideoRxList.push_back(std::move(primary));
	udpVideoRxList.push_back(std::move(secondary));
  }
}

std::unique_ptr<UDPWBTransmitter> WBStreams::createUdpWbTx(uint8_t radio_port, int udp_port,bool enableFec,
                                                           std::optional<int> udp_recv_buff_size)const {
  const auto mcs_index=static_cast<int>(_settings->get_settings().wb_mcs_index);
  const auto channel_width=static_cast<int>(_settings->get_settings().wb_channel_width);
  RadiotapHeader::UserSelectableParams wifiParams{channel_width, false, 0, false, mcs_index};
  TOptions options{};
  // We log them all manually together
  options.enableLogAlive= false;
  options.radio_port = radio_port;
  options.keypair = std::nullopt;
  if(enableFec){
	options.fec_k=static_cast<int>(_settings->get_settings().wb_video_fec_block_length);
	options.fec_percentage=static_cast<int>(_settings->get_settings().wb_video_fec_percentage); // Default to 20% fec overhead
        //options.fec_k="h264";
  }else{
	options.fec_k=0;
	options.fec_percentage=0;
  }
  options.wlan = _broadcast_cards.at(0)->_wifi_card.interface_name;
  m_console->debug("Starting WFB_TX with MCS:{}",mcs_index);
  return std::make_unique<UDPWBTransmitter>(wifiParams, options, "127.0.0.1", udp_port,udp_recv_buff_size);
}

std::unique_ptr<UDPWBReceiver> WBStreams::createUdpWbRx(uint8_t radio_port, int udp_port){
  ROptions options{};
  // We log them all manually together
  options.enableLogAlive= false;
  // TODO REMOVE ME FOR TESTING
  //options.enableLogAlive = udp_port==5600;
  options.radio_port = radio_port;
  options.keypair = std::nullopt;
  const auto cards = get_rx_card_names();
  assert(!cards.empty());
  options.rxInterfaces = cards;
  // use rx queue depth of 1 for now, this should at least reduce the problem of the burst /
  // high latency when blocks are lost.
  // Multiple rx wifi card's won't provide a benefit with this parameter set though.
  options.rx_queue_depth = 1;//_broadcast_cards.size() > 1 ? 10 : 2;
  return std::make_unique<UDPWBReceiver>(options, "127.0.0.1", udp_port,[this](OpenHDStatisticsWriter::Data stats){
	this->onNewStatisticsData(stats);
  });
}

std::string WBStreams::createDebug(){
  std::unique_lock<std::mutex> lock(_wbRxTxInstancesLock, std::try_to_lock);
  if(!lock.owns_lock()){
	// We can just discard statistics data during a re-start
	return "WBStreams::No debug during restart\n";
  }
  std::stringstream ss;
  // we use telemetry data only here
  bool any_data_received=false;
  if(udpTelemetryRx && udpTelemetryRx->anyDataReceived()){
    any_data_received=true;
  }
  ss<<"Any data received: "<<(any_data_received ? "Y":"N")<<"\n";
  if (udpTelemetryRx) {
	ss<<"TeleRx: "<<udpTelemetryRx->createDebug();
  }
  if (udpTelemetryTx) {
	ss<<"TeleTx: "<<udpTelemetryTx->createDebug();
  }
  for (const auto &txvid: udpVideoTxList) {
	ss<<"VidTx: "<<txvid->createDebug();
  }
  for (const auto &rxvid: udpVideoRxList) {
	ss<<"VidRx :"<<rxvid->createDebug();
  }
  ss<<_last_all_stats<<"\n";
  /*if(udpTelemetryTx){
	ss<<"PPS telemetry:"<<udpTelemetryTx->get_current_packets_per_second()<<"\n";
  }*/
  return ss.str();
}

void WBStreams::addExternalDeviceIpForwardingVideoOnly(const std::string& ip) {
  std::lock_guard<std::mutex> guard(_wbRxTxInstancesLock);
  bool first= true;
  assert(udpVideoRxList.size()==2);
  m_console->info("WBStreams::addExternalDeviceIpForwardingVideoOnly:"+ip);
  // forward video
  for(auto& rxVid:udpVideoRxList){
	const auto udpPort=first ? OHD_VIDEO_AIR_VIDEO_STREAM_1_UDP : OHD_VIDEO_AIR_VIDEO_STREAM_2_UDP;
	first= false;
	rxVid->addForwarder(ip,udpPort);
  }
  // TODO how do we deal with telemetry
}

void WBStreams::removeExternalDeviceIpForwardingVideoOnly(const std::string& ip) {
  std::lock_guard<std::mutex> guard(_wbRxTxInstancesLock);
  bool first= true;
  assert(udpVideoRxList.size()==2);
  for(auto& rxVid:udpVideoRxList){
	const auto udpPort=first ? OHD_VIDEO_AIR_VIDEO_STREAM_1_UDP : OHD_VIDEO_AIR_VIDEO_STREAM_2_UDP;
	first= false;
	rxVid->removeForwarder(ip,udpPort);
  }
}

std::vector<std::string> WBStreams::get_rx_card_names() const {
  std::vector<std::string> ret{};
  for(const auto& card: _broadcast_cards){
    ret.push_back(card->_wifi_card.interface_name);
  }
  return ret;
}

bool WBStreams::ever_received_any_data(){
  std::lock_guard<std::mutex> guard(_wbRxTxInstancesLock);
    if(_profile.is_air){
        // check if we got any telemetry data, we never receive video data
        assert(udpTelemetryRx);
        return udpTelemetryRx->anyDataReceived();
    }
    // ground
    bool any_data_received=false;
    // any telemetry data
    assert(udpTelemetryRx);
    if(udpTelemetryRx->anyDataReceived()){
        any_data_received=true;
    }
    // or any video data
    for(const auto& vidrx:udpVideoRxList){
        if(vidrx->anyDataReceived()){
            any_data_received=true;
        }
    }
    return any_data_received;
}

static void convert(openhd::link_statistics::StatsFECVideoStreamRx& dest,const FECStreamStats& src){
  dest.count_fragments_recovered=src.count_fragments_recovered;
  dest.count_blocks_recovered=src.count_blocks_recovered;
  dest.count_blocks_lost=src.count_blocks_lost;
  dest.count_blocks_total=src.count_blocks_total;
  dest.count_bytes_forwarded=src.count_bytes_forwarded;
}

void WBStreams::set_callback(openhd::link_statistics::STATS_CALLBACK stats_callback) {
  std::lock_guard<std::mutex> guard(_statisticsDataLock);
  _stats_callback=std::move(stats_callback);
}

// TDOO fixme
// This is completely not understandable, but I needed to quickly make it work for testing.
void WBStreams::onNewStatisticsData(const OpenHDStatisticsWriter::Data& data) {
  std::lock_guard<std::mutex> guard(_statisticsDataLock);
  std::unique_lock<std::mutex> lock(_wbRxTxInstancesLock, std::try_to_lock);
  if(!lock.owns_lock()){
	// We can just discard statistics data during a re-start
	m_console->debug("WBStreams::No statistics during restart");
	return;
  }
  // TODO make more understandable, but tele rx or tele tx is correct here
  if(data.radio_port==OHD_TELEMETRY_WIFIBROADCAST_TX_RADIO_PORT
  || data.radio_port==OHD_TELEMETRY_WIFIBROADCAST_RX_RADIO_PORT){
	_last_stats_per_rx_stream.at(0)=data;
  }else if(data.radio_port==OHD_VIDEO_PRIMARY_RADIO_PORT){
	_last_stats_per_rx_stream.at(1)=data;
  }else if(data.radio_port==OHD_VIDEO_SECONDARY_RADIO_PORT){
	_last_stats_per_rx_stream.at(2)=data;
  }else{
	m_console->warn("Unknown radio port on stats {}",(int)data.radio_port);
	return;
  }
  // other stuff is per stream / accumulated
  openhd::link_statistics::StatsTotalAllStreams stats_total_all_streams{};
  // accumulate all RX data
  for(const auto& rx_stat:_last_stats_per_rx_stream){
	stats_total_all_streams.count_wifi_packets_received+=rx_stat.wb_rx_stats.count_p_all;
	//count_all_bytes_received+=stats_per_rx_stream.wb_rx_stats.count_bytes_received;
	stats_total_all_streams.count_bytes_received+=rx_stat.wb_rx_stats.count_bytes_data_received;
  }
  // tx-es are a bit different
  if(udpTelemetryTx){
	// this one is total
	stats_total_all_streams.curr_telemetry_tx_bps=udpTelemetryTx->get_current_injected_bits_per_second();
	// these ones are accumulated
	stats_total_all_streams.count_wifi_packets_injected+=udpTelemetryTx->get_n_injected_packets();
	stats_total_all_streams.count_bytes_injected+=udpTelemetryTx->get_n_injected_bytes();
	stats_total_all_streams.count_telemetry_tx_injections_error_hint+=udpTelemetryTx->get_count_tx_injections_error_hint();
  }
  stats_total_all_streams.curr_telemetry_rx_bps=_last_stats_per_rx_stream.at(0).wb_rx_stats.curr_bits_per_second;

  for(const auto& videoTx:udpVideoTxList){
	// accumulated
	stats_total_all_streams.count_wifi_packets_injected+=videoTx->get_n_injected_packets();
	stats_total_all_streams.count_bytes_injected+=videoTx->get_n_injected_bytes();
	stats_total_all_streams.count_video_tx_injections_error_hint+=videoTx->get_count_tx_injections_error_hint();
  }
  if(_profile.is_air){
	if(!udpVideoTxList.empty()){
	  stats_total_all_streams.curr_video0_bps=udpVideoTxList.at(0)->get_current_provided_bits_per_second();
          stats_total_all_streams.curr_video0_tx_pps=udpVideoTxList.at(0)->get_current_packets_per_second();
	}
	if(udpVideoTxList.size()>=2){
	  stats_total_all_streams.curr_video1_bps=udpVideoTxList.at(1)->get_current_provided_bits_per_second();
          stats_total_all_streams.curr_video1_tx_pps=udpVideoTxList.at(1)->get_current_packets_per_second();
	}
  }else{
	stats_total_all_streams.curr_video0_bps=_last_stats_per_rx_stream.at(1).wb_rx_stats.curr_bits_per_second;
	stats_total_all_streams.curr_video1_bps=_last_stats_per_rx_stream.at(2).wb_rx_stats.curr_bits_per_second;
        stats_total_all_streams.curr_video0_tx_pps=0;
        stats_total_all_streams.curr_video1_tx_pps=0;
        if(udpTelemetryTx){
          stats_total_all_streams.curr_telemetry_tx_pps=udpTelemetryTx->get_current_packets_per_second();
        }
  }
  // dBm / rssi for all connected cards that are doing wifibroadcast
  openhd::link_statistics::StatsAllCards stats_all_cards{};
  // dBm is per card, not per stream
  assert(stats_all_cards.size()>=4);
  // only populate actually used cards
  assert(_broadcast_cards.size()<=stats_all_cards.size());
  for(int i=0;i<_broadcast_cards.size();i++){
	auto& card = stats_all_cards.at(i);
	if(_profile.is_air){
	  // on air, we use the dbm reported by the telemetry stream
	  card.rx_rssi= _last_stats_per_rx_stream.at(0).rssiPerCard.at(i).last_rssi;
	}else{
	  // on ground, we use the dBm reported by the telemetry stream or video stream
	  const auto rssi_telemetry=_last_stats_per_rx_stream.at(0).rssiPerCard.at(i).last_rssi;
	  const auto rssi_video0=_last_stats_per_rx_stream.at(1).rssiPerCard.at(i).last_rssi;
	  if(rssi_video0==0){
		// use telemetry
		card.rx_rssi=rssi_telemetry;
	  }else{
		card.rx_rssi=rssi_video0;
	  }
	}
	card.exists_in_openhd= true;
	// not yet supported
	card.count_p_injected=0;
	card.count_p_received=0;
  }
  //
  std::optional<openhd::link_statistics::StatsFECVideoStreamRx> stats_video_stream0_rx=std::nullopt;
  std::optional<openhd::link_statistics::StatsFECVideoStreamRx> stats_video_stream1_rx=std::nullopt;
  if(!_profile.is_air){
	if(_last_stats_per_rx_stream.at(1).fec_stream_stats.has_value()){
	  stats_video_stream0_rx=openhd::link_statistics::StatsFECVideoStreamRx{};
	  convert(stats_video_stream0_rx.value(),_last_stats_per_rx_stream.at(1).fec_stream_stats.value());
	}
	if(_last_stats_per_rx_stream.at(2).fec_stream_stats.has_value()){
	  stats_video_stream1_rx=openhd::link_statistics::StatsFECVideoStreamRx{};
	  convert(stats_video_stream1_rx.value(),_last_stats_per_rx_stream.at(2).fec_stream_stats.value());
	}
  }
  _last_all_stats=openhd::link_statistics::AllStats{stats_total_all_streams, stats_all_cards,stats_video_stream0_rx,stats_video_stream1_rx};
  if(_stats_callback){
	_stats_callback(_last_all_stats);
  }
}

void WBStreams::restart() {
  std::lock_guard<std::mutex> guard(_wbRxTxInstancesLock);
  m_console->info("WBStreams::restart() begin");
  if(udpTelemetryRx){
	udpTelemetryRx->stop_looping();
	udpTelemetryRx.reset();
  }
  if(udpTelemetryTx){
	udpTelemetryTx->stopBackground();
	udpTelemetryTx.reset();
  }
  for(auto& videoTx:udpVideoTxList){
	videoTx->stopBackground();
	videoTx.reset();
  }
  udpVideoTxList.resize(0);
  for(auto& videoRx:udpVideoRxList){
	videoRx->stop_looping();
	videoRx.reset();
  }
  udpVideoRxList.resize(0);
  configure_cards();
  configure_telemetry();
  configure_video();
  m_console->info("WBStreams::restart() end");
}

bool WBStreams::set_frequency(int frequency) {
  m_console->debug("WBStreams::set_frequency {}",frequency);
  if(m_disable_all_frequency_checks){
    m_console->warn("Not sanity checking frequency");
  }else{
    if(_settings->get_settings().configured_for_2G()){
      if(!openhd::is_valid_frequency_2G(frequency)){
        m_console->warn("Invalid 2.4G frequency {}",frequency);
        return false;
      }
    }else{
      if(!openhd::is_valid_frequency_5G(frequency)){
        m_console->warn("Invalid 5G frequency {}",frequency);
        return false;
      }
    }
  }
  _settings->unsafe_get_settings().wb_frequency=frequency;
  _settings->persist();
  // We can update the frequency without restarting the streams
  // Only save, need restart to apply
  /*for(const auto& holder:_broadcast_cards){
	const auto& card=holder->_wifi_card;
	const bool width_40=_settings->get_settings().wb_channel_width==40;
	WifiCardCommandHelper::set_frequency_and_channel_width(card,frequency,width_40);
  }*/
  return true;
}

bool WBStreams::set_txpower(int tx_power) {
  m_console->debug("WBStreams::set_txpower {}",tx_power);
  if(!openhd::is_valid_tx_power_milli_watt(tx_power)){
	m_console->warn("Invalid tx power:{}",tx_power);
	return false;
  }
  _settings->unsafe_get_settings().wb_tx_power_milli_watt=tx_power;
  _settings->persist();
  // We can update the tx power without restarting the streams
  for(const auto& holder:_broadcast_cards){
	const auto& card=holder->_wifi_card;
	WifiCardCommandHelper::set_txpower(card,tx_power);
  }
  return true;
}

bool WBStreams::set_mcs_index(int mcs_index) {
  m_console->debug("WBStreams::set_mcs_index {}",mcs_index);
  if(!openhd::is_valid_mcs_index(mcs_index)){
	m_console->warn("Invalid mcs index{}",mcs_index);
	return false;
  }
  if(!validate_cards_support_setting_mcs_index()){
    m_console->warn("Cannot change mcs index, it is fixed for at least one of the used cards");
    return false;
  }
  _settings->unsafe_get_settings().wb_mcs_index=mcs_index;
  _settings->persist();
  // Only save, need restart to apply
  // To set the mcs index, r.n we have to restart the tx instances
  /*std::lock_guard<std::mutex> guard(_wbRxTxInstancesLock);
  if(udpTelemetryTx){
	udpTelemetryTx->update_mcs_index(mcs_index);
  }
  for(auto& tx:udpVideoTxList){
	tx->update_mcs_index(mcs_index);
  }*/
  return true;
}
bool WBStreams::set_channel_width(int channel_width) {
  m_console->debug("WBStreams::set_channel_width {}",channel_width);
  if(!openhd::is_valid_channel_width(channel_width)){
	m_console->warn("Invalid channel width {}",channel_width);
	return false;
  }
  if(!validate_cards_support_setting_channel_width()){
    m_console->warn("Cannot change channel width, at least one card doesn't support it");
    return false;
  }
  _settings->unsafe_get_settings().wb_channel_width=channel_width;
  _settings->persist();
  // Only save, need restart to apply
  /*for(const auto& holder:_broadcast_cards){
	const auto& card=holder->_wifi_card;
	const bool width_40=_settings->get_settings().wb_channel_width==40;
	const auto frequency=_settings->get_settings().wb_frequency;
	WifiCardCommandHelper::set_frequency_and_channel_width(card,frequency,width_40);
  }
  restart_async();*/
  return true;
}

bool WBStreams::set_fec_block_length(int block_length) {
  m_console->debug("WBStreams::set_fec_block_length {}",block_length);
  if(!openhd::is_valid_fec_block_length(block_length)){
	m_console->warn("Invalid fec block length:{}",block_length);
	return false;
  }
  _settings->unsafe_get_settings().wb_video_fec_block_length=block_length;
  _settings->persist();
  restart_async();
  return true;
}

bool WBStreams::set_fec_percentage(int fec_percentage) {
  m_console->debug("WBStreams::set_fec_percentage {}",fec_percentage);
  if(!openhd::is_valid_fec_percentage(fec_percentage)){
	m_console->warn("Invalid fec percentage:{}",fec_percentage);
	return false;
  }
  _settings->unsafe_get_settings().wb_video_fec_percentage=fec_percentage;
  _settings->persist();
  restart_async();
  return true;
}


void WBStreams::restart_async(std::chrono::milliseconds delay){
  std::lock_guard<std::mutex> guard(_restart_async_lock);
  if(_restart_async_thread!= nullptr){
	m_console->warn("WBStreams::restart_async - settings changed too quickly");
	if(_restart_async_thread->joinable()){
	  _restart_async_thread->join();
	}
	_restart_async_thread=nullptr;
  }
  _restart_async_thread=
	  std::make_unique<std::thread>(
		  [this,delay]{
			std::this_thread::sleep_for(delay);
			this->restart();
		  }
	  );
}

std::vector<openhd::Setting> WBStreams::get_all_settings(){
  using namespace openhd;
  std::vector<openhd::Setting> ret{};
  auto change_freq=openhd::IntSetting{(int)_settings->get_settings().wb_frequency,[this](std::string,int value){
	return set_frequency(value);
  }};
  auto change_wb_channel_width=openhd::IntSetting{(int)_settings->get_settings().wb_channel_width,[this](std::string,int value){
	return set_channel_width(value);
  }};
  auto change_wb_mcs_index=openhd::IntSetting{(int)_settings->get_settings().wb_mcs_index,[this](std::string,int value){
	return set_mcs_index(value);
  }};
  auto change_tx_power=openhd::IntSetting{(int)_settings->get_settings().wb_tx_power_milli_watt,[this](std::string,int value){
	return set_txpower(value);
  }};
  ret.push_back(Setting{WB_FREQUENCY,change_freq});
  ret.push_back(Setting{WB_CHANNEL_WIDTH,change_wb_channel_width});
  ret.push_back(Setting{WB_MCS_INDEX,change_wb_mcs_index});
  ret.push_back(Setting{WB_TX_POWER_MILLI_WATT,change_tx_power});

  if(_profile.is_air){
	auto change_video_fec_block_length=openhd::IntSetting{(int)_settings->get_settings().wb_video_fec_block_length,[this](std::string,int value){
	  return set_fec_block_length(value);
	}};
	ret.push_back(Setting{WB_VIDEO_FEC_BLOCK_LENGTH,change_video_fec_block_length});
	auto change_video_fec_percentage=openhd::IntSetting{(int)_settings->get_settings().wb_video_fec_percentage,[this](std::string,int value){
	  return set_fec_percentage(value);
	}};
	ret.push_back(Setting{WB_VIDEO_FEC_PERCENTAGE,change_video_fec_percentage});
  }
  openhd::validate_provided_ids(ret);
  return ret;
}

bool WBStreams::validate_cards_support_setting_mcs_index() {
  for(const auto& card_handle:_broadcast_cards){
    if(!wifi_card_supports_variable_mcs(card_handle->_wifi_card)){
      return false;
    }
  }
  return true;
}

bool WBStreams::validate_cards_support_setting_channel_width() {
  for(const auto& card_handle:_broadcast_cards){
    if(!wifi_card_supports_40Mhz_channel_width(card_handle->_wifi_card)){
      return false;
    }
  }
  return true;
}
